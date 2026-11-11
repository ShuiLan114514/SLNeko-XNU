/*
 * The touch screen of the Redmi Note 11T Pro (xaga): a Novatek NT36672E on the MT6895's SPI2, as a
 * source of touches for /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg touchscreen=3:
 * every pin and address below belongs to this phone.
 *
 * Why the pins are driven by hand and not through the SPI controller: the controller's register
 * layout for this SoC is the same as the MT6768's (Linux drivers/spi/spi-mt65xx.c covers both), but
 * the *pin function number* that turns GPIO109..112 into SPI2_A_MI/MO/CLK/CSB is in a pinctrl table
 * this tree does not have -- xaga-mt6895-touch.dtsi names it (PINMUX_GPIO110__FUNC_SPI2_A_CSB) and
 * the number it stands for is generated into a header the vendor does not publish. The four pins
 * driven as plain GPIO need no such number, and at the speed a touch report needs (66 bytes at
 * 100 kHz, under a millisecond) the difference does not show. The loader's own mtkspi.h makes the
 * same trade for the MT6768's firmware download.
 *
 * The chip runs its firmware out of its own flash and comes up reporting after a power-on, which
 * is why this driver sends it nothing: Android's driver only downloads firmware when the version
 * on the chip is older than the one in /vendor/firmware. A reset would put it back into its ROM
 * bootloader, which answers but cannot report touches, so nothing here resets it.
 *
 * What is read: address 0 is the report, 65 bytes, the last a checksum. Every finger after the
 * first byte is six of them:
 *
 *   [0]   the finger's number in the top five bits, in bits 0..2 what it is doing
 *         (1 landed, 2 moved, 0 or 3 lifted)
 *   [1]   x, the top eight bits
 *   [2]   y, the top eight bits
 *   [3]   x's low four bits in the top half, y's in the bottom
 *   [4]   the pressure
 *   [5]   the pressure's high byte, for the first two fingers only
 *
 * Register and protocol references: the driver the phone ships, in the Redmi Note 11T Pro's kernel
 * tree under drivers/input/touchscreen/NT36672C/ (nt36xxx.c: CTP_SPI_READ, nvt_set_page,
 * nvt_ts_point_data_checksum, nvt_ts_work_func).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

/* The image the chip has to be given: mtk/tools/mknvtfw.py makes this out of the vendor blob. */
#include "redwood.nvt_fw.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GPIO_PHYS       0x10005000
#define GPIO_SIZE       0x1000

/* The four bus pins and the interrupt line, from the phone's own device tree. */
#define PIN_MI          109             /* the chip drives this one */
#define PIN_CS          110
#define PIN_MO          111
#define PIN_CLK         112
#define PIN_IRQ         135

/*
 * The block keeps a direction, an output and an input register per thirty-two pins, sixteen bytes
 * apart: pins 0..31 at 0x004/0x104/0x200, 32..63 at 0x014/0x114/0x210, and so on (Linux
 * drivers/pinctrl/mediatek/pinctrl-mtk-common.c, mtk_gpio_get: din_offset + (pin / 32) * 16). Every
 * pin this driver touches is above 108, so it lives in the block's third and fourth thirty-two --
 * the MT6768's four pins were all in the first, which is why the reference driver could write the
 * registers without the stride.
 */
#define GPIO_DIR_SET(n)  (0x004 + (n) * 0x10u)          /* n: which thirty-two */
#define GPIO_DIR_CLR(n)  (0x008 + (n) * 0x10u)
#define GPIO_DOUT_SET(n) (0x104 + (n) * 0x10u)
#define GPIO_DOUT_CLR(n) (0x108 + (n) * 0x10u)
#define GPIO_DIN(n)      (0x200 + (n) * 0x10u)

/*
 * A pin's function is four bits in a register at 0x300 plus sixteen per eight pins: pins 8..15 are
 * at 0x310, 104..111 at 0x3d0, 112..119 at 0x3e0, and 128..135 at 0x400. Writing zero leaves the
 * pin as a plain GPIO, which is what bit-banging wants -- and is also the state the pin is in when
 * this driver first sees it, since the kernel has no pinctrl.
 */
#define GPIO_MODE_BASE  0x300
#define GPIO_MODE_STEP  0x10

#define GROUP_OF(pin)   ((pin) / 32u)
#define BIT_OF(pin)     (1u << ((pin) % 32u))

static vm_offset_t gpio;

static inline uint32_t
rd(uint32_t at)
{
	return *(volatile uint32_t *)(gpio + at);
}

static inline void
wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(gpio + at) = value;
}

static inline void
pin_out(uint32_t pin, bool high)
{
	wr(high ? GPIO_DOUT_SET(GROUP_OF(pin)) : GPIO_DOUT_CLR(GROUP_OF(pin)), BIT_OF(pin));
}

static inline bool
pin_in(uint32_t pin)
{
	return (rd(GPIO_DIN(GROUP_OF(pin))) & BIT_OF(pin)) != 0;
}

/*
 * Long enough for a pad to settle. One register read is already most of a microsecond on this SoC,
 * and the chip's device tree asks for nothing faster than 13 MHz, which a hand-driven bus does not
 * come close to. The read is of the input register the bus pins live in, so that it is a real
 * access to a block that is on rather than one whose clock may be gated.
 */
static inline void
pin_settle(void)
{
	(void)rd(GPIO_DIN(GROUP_OF(PIN_CLK)));
	(void)rd(GPIO_DIN(GROUP_OF(PIN_CLK)));
}

static void
pins_as_gpio(void)
{
	/*
	 * The interrupt line as well as the four bus pins: the device tree hands it to the chip's
	 * driver as a plain GPIO with an interrupt on it (novatek,irq-gpio = <&pio 135 0x2001>),
	 * and what the bootloader left in its function-select field is not known here -- if it is
	 * still an EINT or an SPI2 alternate function, the input register reads nothing and the
	 * chip's assertion is never seen.
	 */
	const uint32_t pins[] = { PIN_MI, PIN_CS, PIN_MO, PIN_CLK, PIN_IRQ };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t pin = pins[i];
		uint32_t at = GPIO_MODE_BASE + (pin / 8) * GPIO_MODE_STEP;
		uint32_t shift = (pin % 8) * 4;

		wr(at, rd(at) & ~(0xfu << shift));
	}
	/* three of them we drive, the chip drives the fourth and the interrupt line */
	wr(GPIO_DIR_SET(GROUP_OF(PIN_MO)), BIT_OF(PIN_MO) | BIT_OF(PIN_CLK) | BIT_OF(PIN_CS));
	wr(GPIO_DIR_CLR(GROUP_OF(PIN_MI)), BIT_OF(PIN_MI));
	/*
	 * Its own group, not the one PIN_MI is in. The interrupt line is GPIO135, group 4, while the
	 * four bus pins are 109 to 112, group 3: one write naming GROUP_OF(PIN_MI) clears bit 7 of
	 * group 3, which is pin 103, and leaves GPIO135's direction as the bootloader left it. If
	 * that is an output driven low, the input register reads 0 for ever and the chip's
	 * assertion is never seen -- which is exactly what the logs showed (0x40003e7e, bit 7
	 * clear, on every boot, and 0 reports read).
	 */
	wr(GPIO_DIR_CLR(GROUP_OF(PIN_IRQ)), BIT_OF(PIN_IRQ));
	pin_out(PIN_CS, true);          /* chip select idles high */
	pin_out(PIN_CLK, false);        /* mode 0: the clock idles low */
	__asm__ volatile ("dsb sy");
	pin_settle();
}

/*
 * Mode 0: the clock idles low, the data changes while it is low and is read on the rising edge.
 * `len` bytes out of tx (0xff where tx is NULL) and the same number in.
 */
static void
spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	pin_out(PIN_CLK, false);
	pin_out(PIN_CS, false);
	pin_settle();

	for (uint32_t at = 0; at < len; at++) {
		uint8_t out = tx ? tx[at] : 0xff, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_out(PIN_MO, (out >> bit) & 1);
			pin_settle();
			pin_out(PIN_CLK, true);
			pin_settle();
			in = (uint8_t)((in << 1) | (pin_in(PIN_MI) ? 1 : 0));
			pin_out(PIN_CLK, false);
			pin_settle();
		}
		if (rx) {
			rx[at] = in;
		}
	}
	pin_out(PIN_CS, true);
	pin_settle();
}

/*
 * The chip's own addressing: 0xff, then the address's top eight bits, then its next eight. What is
 * left of the address is the byte's own low seven bits, sent with the request.
 */
#define NVT_SWRST_N8_ADDR   0x03F0FEu   /* novatek,swrst-n8-addr, from this phone's device tree */
#define NVT_SPI_RD_FAST_ADDR 0x03F310u  /* novatek,spi-rd-fast-addr */
#define NVT_ENG_RST_ADDR    0x7FFF80u   /* ENG_RST_ADDR, nt36xxx.c:57 */
#define NVT_REG_PAGE    0xff
#define NVT_READ_MASK   0x7f
#define NVT_WRITE_MASK  0x80

/*
 * The widest transfer here is one report: 108 bytes plus the address byte and its dummy, so
 * 110 on the bus. It was 80 when the report was believed to be 65 bytes long.
 */
#define XFER_MAX        128

static bool
nvt_set_page(uint32_t address)
{
	uint8_t out[3];

	out[0] = NVT_REG_PAGE;
	out[1] = (uint8_t)((address >> 15) & 0xff);
	out[2] = (uint8_t)((address >> 7) & 0xff);
	spi_xfer(out, NULL, sizeof(out));
	return true;
}

/*
 * One byte written to the chip's address space: nvt_write_addr() in the vendor tree. The page is
 * set the same way a read sets it, then two bytes go out -- the address with its top bit set (which
 * is what marks this as a write rather than a read) and the value.
 *
 * This was missing until now, and it mattered: everything the vendor does before it trusts a report
 * is a *write*. Two of them, in particular, are the difference between a chip that answers and a
 * chip that reports, and neither can be done by reading.
 */
static bool
nvt_write_addr(uint32_t address, uint8_t value)
{
	uint8_t out[2];

	nvt_set_page(address);
	out[0] = (uint8_t)((address & NVT_READ_MASK) | NVT_WRITE_MASK);
	out[1] = value;
	spi_xfer(out, NULL, sizeof(out));
	return true;
}

/*
 * One read of `len` bytes starting at `address` into `data`.
 *
 * The byte count on the bus is the vendor's, not the obvious one: CTP_SPI_READ() is handed
 * POINT_DATA_LEN + 1 = 66 bytes and spi_read_write() transfers len + DUMMY_BYTES = 67, because the
 * first byte out is the address and the first byte back is that same byte shifted out. What comes
 * back after it is a dummy, and the report starts at index 2. Get this wrong by one and every
 * report fails its checksum, which is exactly what the first version of this driver did.
 */
static bool
nvt_read(uint32_t address, uint8_t *data, uint32_t len)
{
	uint8_t out[XFER_MAX], in[XFER_MAX];

	if (len == 0 || len + 2 > sizeof(out)) {
		return false;
	}
	out[0] = (uint8_t)(address & NVT_READ_MASK);
	for (uint32_t i = 1; i < len + 2; i++) {
		out[i] = 0;
	}
	spi_xfer(out, in, len + 2);
	for (uint32_t i = 0; i < len; i++) {
		data[i] = in[i + 2];
	}
	return true;
}

/* ---- what a report holds ---- */

/*
 * How long one report is depends on the resolution mode the panel is wired for, and the two
 * lengths are not interchangeable.
 *
 * The vendor driver carries both: nt36xxx.h:110 sets NVT_SUPER_RESOLUTION_10S to 10, and on the
 * line after it POINT_DATA_LEN becomes 108 instead of 65. In the ten-times mode each finger is
 * still six bytes, but x and y are sixteen bits wide and the pressure array moves to byte 98.
 * This phone's panel is ten-times: Android registers ABS_MT_POSITION_X with a maximum of 10799,
 * which is 1080 * 10 - 1, and the device tree asks for super-resolution-factor = <10>.
 *
 * Reading 65 bytes out of a 108-byte report is what made the vertical axis look unsolvable: the
 * upper bits of y were never on the wire as far as this driver was concerned, so no amount of
 * fitting could find them. Four corners pressed on the real panel and compared against Android
 * left a least-squares slope of -9.15 that no flip or scale could explain.
 */
/*
 * The read length and the decoding are separate questions and are answered separately here.
 *
 * The length is not in doubt: the vendor driver that is actually compiled on this phone
 * (drivers/input/touchscreen/NVT36672C, selected by Makefile:122 through
 * CONFIG_TOUCHSCREEN_NVT36672C_HOSTDL_SPI) sets NVT_SUPER_RESOLUTION_10S to 10 at nt36xxx.h:110,
 * and the line under it makes POINT_DATA_LEN 108. So it asks the chip for 109 bytes on every
 * interrupt, on every panel, whatever mode the coordinates then turn out to be in. This driver
 * has been asking for sixty-six and reading the first sixty-five.
 *
 * Those extra bytes turned out to hold nothing this driver needs: bytes 65..107 were byte for
 * byte identical across forty reports. What settled the decode was the image, not the length.
 * With fw02 -- the file the device downloads on its own, which nvt_match_fw picks from the
 * lockdown bytes -- the ten-times branch is the right one: five presses gave x 112, 969, 101,
 * 999, 574 and y 2346, 2347, 110, 156, 1367 for physical (91,2363) (1003,2383) (91,157)
 * (997,116) (573,1290), all within a fingertip. With novatek_ts_fw.bin the same branch gave
 * 1568 pixels of x, which is why it was once rejected; that image simply reports differently.
 */
#define NVT_SUPER_10S   1               /* the chip reports tenths of a pixel, as Android sees it */
#define POINT_DATA_LEN  108             /* what the vendor driver asks for, in every mode */
#define POINT_DATA_CHECKSUM_LEN 65      /* the checksum still covers only the first sixty-five */
#define FINGER_BYTES    6
#define MAX_FINGERS     10
#define FINGER_PRESSURE 98              /* the ten-times mode keeps pressures at 98 + i */

/*
 * How often the line is sampled while waiting for a report, and for how long.
 *
 * One sample per tick was too coarse to ever see the assertion: the line goes high for a few
 * microseconds while a tick is two hundred milliseconds, and Android's own eint counter reports
 * tens of thousands of edges, so the pulses are real and frequent. A short burst of back-to-back
 * reads was the first answer, but a burst of two hundred and fifty-six register reads covers
 * perhaps twenty-five microseconds and then sleeps for the rest of the window -- at an edge every
 * six or seven milliseconds that misses better than ninety-nine percent of them.
 *
 * So the wait itself is the sampling loop: fifty-microsecond steps across the whole window. Four
 * thousand single-register reads per two hundred milliseconds is nothing, and it means every
 * assertion is seen rather than gambled on.
 */
#define NVT_IRQ_STEP_US    50u
#define NVT_IRQ_WINDOW_US  200000u

/*
 * How many quiet ticks to let pass before asking anyway. At the default two-hundred-millisecond
 * tick this is one read every two seconds, which is slow enough that it cannot be mistaken for the
 * pacing that four-thousand-reads-a-second turned out to be.
 */
#define NVT_QUIET_READ_TICKS 10u

#define PANEL_W         1080
#define PANEL_H         2460
#define PICTURE_W       1125            /* what iOS thinks the display is, and draws for */
#define PICTURE_H       2436

/*
 * The report is checked before anything is done with it: a byte read off a bus that is not in step
 * is worse than no report at all, and the chip computes the same sum for us. nvt_ts_point_data_
 * checksum() sums buf[1] .. buf[length-1] and compares ~sum + 1 with buf[length], where buf is the
 * buffer CTP_SPI_READ filled -- so with this driver's `data`, which already starts at the report,
 * the sum is over data[0] .. data[63] and the value is data[64]. The first version of this file
 * started at data[1] and never matched.
 */
static bool
report_ok(const uint8_t *data)
{
	uint8_t sum = 0;

	/*
	 * The checksum keeps its old extent in the ten-times mode: it sums the first sixty-four
	 * report bytes and sits in byte sixty-four, while the report itself is a hundred and eight
	 * bytes long. The vendor says the same thing twice -- POINT_DATA_CHECKSUM_LEN is 65 on both
	 * sides of the #if that changes POINT_DATA_LEN -- and nvt_ts_point_data_checksum() sums
	 * length - 1 bytes and compares against byte `length`.
	 */
	for (unsigned i = 0; i < POINT_DATA_CHECKSUM_LEN - 1; i++) {
		sum = (uint8_t)(sum + data[i]);
	}
	return (uint8_t)(~sum + 1) == data[POINT_DATA_CHECKSUM_LEN - 1];
}

static uint32_t fit;                    /* boot-arg fbfit: how the picture sits on the panel */

/* fbfit=1: the picture keeps its proportions, the full width, centred. */
#define PICTURE_H_ON_W  (PANEL_W * PICTURE_H / PICTURE_W)
#define PICTURE_TOP     ((PANEL_H - PICTURE_H_ON_W) / 2)

/*
 * No mirror. The earlier one was an artefact of the wrong image: with novatek_ts_fw.bin the chip
 * reported a compressed four-bit x, and 1080 - x happened to fit four corners. With fw02, the
 * image Android itself loads, the plain reading already runs the right way -- five presses gave
 * x 112, 969, 101, 999, 574 for physical 91, 1003, 91, 997, 573.
 */
static uint32_t
picture_x(uint32_t x)
{
	if (x >= PANEL_W) {
		x = PANEL_W - 1;
	}
	return x * 65535 / (PANEL_W - 1);
}

/*
 * The vertical axis reads straight through once the reports are decoded as tenths of a pixel:
 * fw02 gave y 2346, 2347, 110, 156, 1367 for physical 2363, 2383, 157, 116, 1290. The compressed
 * span that a flip could not explain was the four-bit decode, not the chip.
 */
static uint32_t
picture_y(uint32_t y)
{
	if (fit == 1) {
		if (y <= PICTURE_TOP) {
			return 0;
		}
		y -= PICTURE_TOP;
		return y >= PICTURE_H_ON_W ? 65535 : y * 65535 / (PICTURE_H_ON_W - 1);
	}
	if (y >= PANEL_H) {
		y = PANEL_H - 1;
	}
	return y * 65535 / (PANEL_H - 1);
}

static bool down[MAX_FINGERS];
static uint32_t last_x[MAX_FINGERS], last_y[MAX_FINGERS];
static unsigned reports, touches, bad;

static void
report(const uint8_t *data)
{
	bool seen[MAX_FINGERS] = { false };

	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		/*
		 * nvt_ts_work_func() walks position = 1 + 6*i, but that is into the buffer CTP_SPI_
		 * READ filled, whose byte 0 is the address it sent. This driver's `data` starts at
		 * the report, so the fingers start at 6*i.
		 */
		uint32_t at = FINGER_BYTES * i;
		uint32_t which = data[at] >> 3;
		uint32_t what = data[at] & 0x07;
		uint32_t x, y;

		if (which >= MAX_FINGERS) {
			continue;               /* not a finger we can carry */
		}
		if (what != 1 && what != 2) {
			continue;               /* lifted, or a gloved touch: nothing this queue has a word for */
		}
#if NVT_SUPER_10S
		/*
		 * Ten-times mode: x and y are sixteen bits each and are already expressed in tenths
		 * of a pixel, exactly as nvt_ts_work_func() reads them
		 * (nt36xxx.c:1614-1616, point_data[position+1..4]).
		 */
		x = ((uint32_t)data[at + 1] << 8) | (uint32_t)data[at + 2];
		y = ((uint32_t)data[at + 3] << 8) | (uint32_t)data[at + 4];
		x /= 10;
		y /= 10;
#else
		x = ((uint32_t)data[at + 1] << 4) | (data[at + 3] >> 4);
		y = ((uint32_t)data[at + 2] << 4) | (data[at + 3] & 0x0f);
#endif
		if (x >= PANEL_W) {
			x = PANEL_W - 1;
		}
		if (y >= PANEL_H) {
			y = PANEL_H - 1;
		}
		last_x[which] = x;
		last_y[which] = y;
		seen[which] = true;
		touches++;
		redwood_touch_post(down[which] ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN, which,
		    picture_x(x), picture_y(y));
		down[which] = true;
	}
	for (unsigned which = 0; which < MAX_FINGERS; which++) {
		if (down[which] && !seen[which]) {
			redwood_touch_post(REDWOOD_TOUCH_UP, which,
			    picture_x(last_x[which]), picture_y(last_y[which]));
			down[which] = false;
		}
	}
	reports++;
}


/*
 * Where the report lives in the chip's address space.
 *
 * nvt_ts_work_func() calls CTP_SPI_READ(buf, POINT_DATA_LEN + 1) with buf[0] = 0 and does *not*
 * set a page first: the page is latched once during probe, by nvt_set_page(ts->mmap->EVENT_BUF_ADDR),
 * and a read of address 0 then means EVENT_BUF_ADDR. A driver that never latches one reads whatever
 * page the chip happens to be on.
 *
 * The number is no longer a guess. nvt_ts_check_chip_ver_trim() reads six bytes at 0x1F64E and
 * matches them, byte by byte through a mask, against trim_id_table; this chip answers
 * 0e 00 00 72 66 03, which is that table's fifth entry, and the fifth entry points at
 * NT36675_memory_map -- whose EVENT_BUF_ADDR is 0x22D00 (nt36xxx_mem_map.h:92-93). The first
 * attempt here used 0x21C00, which is NT36672A's, from a different entry of the same table: the
 * bus answered, the bytes were stable, and every report still failed its checksum, because the
 * page being read was not the report. Still a boot argument (nvt_page) so a wrong guess costs one
 * reboot rather than a rebuild.
 */
#define NVT_EVENT_BUF_ADDR  0x22D00u

/*
 * ---- giving the chip its firmware ----
 *
 * Everything above assumed the controller runs an image out of its own flash, and that a report
 * failing its checksum therefore meant the wrong page. Both halves of that turned out to be wrong,
 * and the vendor's own driver says so: at nt36xxx.c:1778 it queues Boot_Update_Firmware for 14
 * seconds after probing, and that calls nvt_update_firmware() -- on every boot, not as a recovery.
 * The image is sent from the host over SPI into the controller's SRAM. nvt_sw_reset() (0x55) and
 * nvt_sw_reset_idle() (0xAA) are defined in that tree and called from nowhere.
 *
 * The measurements agree. This chip answers its trim id (0e 00 00 72 66 03) because that lives in
 * ROM and is readable with the MCU in its bootloader; the event buffer at 0x22D00 returns bytes
 * that are stable within a run and different across runs, which is SRAM nobody is maintaining; the
 * handshake byte at 0x22D51 answers neither 0xA0 nor 0x00; and the reset-state byte at 0x22D60
 * stays 0x3A through an eng reset, a 0x55 and fifty polls -- 0x3A is below RESET_STATE_INIT 0xA0,
 * so the firmware never started. Sending an image is the only thing left that the vendor does and
 * this driver did not.
 *
 * The addresses are NT36675_memory_map's, chosen the same way EVENT_BUF_ADDR was: through the trim
 * id. hw_crc is 2 for that map (NT36675_hw_info, nt36xxx_mem_map.h:222-225), which is why the
 * LENGTH register gets four bytes rather than three.
 */
#define NVT_EVENT_MAP_HOST_CMD      0x50u
#define NVT_EVENT_MAP_FWINFO        0x78u
#define NVT_EVENT_MAP_RESET_COMPLETE 0x60u

#define NVT_ILM_DES_ADDR        0x3F128u
#define NVT_DLM_DES_ADDR        0x3F12Cu
#define NVT_BLD_DES_ADDR        0x3F114u
#define NVT_ILM_LENGTH_ADDR     0x3F118u
#define NVT_DLM_LENGTH_ADDR     0x3F130u
#define NVT_BLD_LENGTH_ADDR     0x3F138u
#define NVT_G_ILM_CHECKSUM_ADDR 0x3F100u
#define NVT_G_DLM_CHECKSUM_ADDR 0x3F104u
#define NVT_BLD_CRC_EN_ADDR     0x3F30Eu
#define NVT_BOOT_RDY_ADDR       0x3F10Du
#define NVT_R_ILM_CHECKSUM_ADDR 0x3F120u
#define NVT_R_DLM_CHECKSUM_ADDR 0x3F124u
#define NVT_BLD_ILM_DLM_CRC_ADDR 0x3F133u

/*
 * A buffer to write, with the address byte already marked as a write.
 *
 * CTP_SPI_WRITE() does this itself -- its first statement is buf[0] = SPI_WRITE_MASK(buf[0]) --
 * which is why the vendor's call sites look like plain `fwbuf[0] = addr & 0x7F`. Driving the bus
 * directly gives no such help, and the first version of this driver got it wrong in every
 * multi-byte write here: the address byte went out with the read mask, so the chip answered each
 * chunk instead of storing it, and 139264 bytes of firmware were read commands. Every write in
 * this file goes through this function or nvt_write_addr() for that reason.
 */
static void
nvt_write_buf(uint8_t *buf, uint32_t len)
{
	buf[0] = (uint8_t)(buf[0] | NVT_WRITE_MASK);
	spi_xfer(buf, NULL, len);
}

#define NVT_HW_CRC              2       /* NT36675_hw_info.hw_crc */
#define NVT_RESET_STATE_INIT    0xA0u
#define NVT_RESET_STATE_REK     0xA1u
#define NVT_RESET_STATE_REK_FINISH 0xA2u
#define NVT_RESET_STATE_NORMAL_RUN 0xA3u
#define NVT_RESET_STATE_MAX     0xAFu

/*
 * A chunk of the image held here rather than on a thread's stack, and a multiple of 128 so that the
 * offset within the chip's page -- the low seven bits of the SRAM address, sent with each chunk --
 * is the same for the whole transfer. The vendor uses 63 KiB chunks; there is nothing to gain from
 * that here, because each chunk costs one page set and one chip-select either way and the bus is
 * driven by hand.
 */
#define NVT_CHUNK       4096u
static uint8_t nvt_chunk[NVT_CHUNK + 1];

/*
 * nvt_write_sram(): the image, a chunk at a time, to the SRAM address the partition names.
 *
 * `size` here is what nvt_write_firmware() passes -- the header's field plus one -- while the
 * header value itself is what the bootloader's LENGTH register is told. The vendor makes exactly
 * this distinction and both numbers matter, so the caller adds the one only where it writes.
 */
static bool
nvt_write_sram(uint32_t sram, uint32_t size, uint32_t bin)
{
	while (size != 0) {
		uint32_t len = size < NVT_CHUNK ? size : NVT_CHUNK;

		nvt_set_page(sram);
		nvt_chunk[0] = (uint8_t)(sram & NVT_READ_MASK);
		for (uint32_t i = 0; i < len; i++) {
			nvt_chunk[i + 1] = nvt_fw[bin + i];
		}
		nvt_write_buf(nvt_chunk, len + 1);
		sram += NVT_CHUNK;
		bin += NVT_CHUNK;
		size -= len;
	}
	return true;
}

/* nvt_write_firmware(): the partitions that carry data, and nothing for the ones that do not. */
static void
nvt_write_firmware(void)
{
	for (unsigned i = 0; i < NVT_FW_PARTS; i++) {
		const struct nvt_fw_part *p = &nvt_fw_parts[i];

		if (p->size == 0) {
			continue;
		}
		nvt_write_sram(p->sram, p->size + 1u, p->bin);
	}
}

/*
 * nvt_set_bld_crc_bank(): tell the bootloader where a partition went, how long it is, and what it
 * should checksum to. The length register does not get its own page write -- the vendor comments
 * the call out -- because DES, LENGTH and G_CHECKSUM of one partition share a page: 0x3F128,
 * 0x3F118 and 0x3F100 all live in the page that nvt_set_page(0x3F128) selects.
 */
static void
nvt_set_bld_crc_bank(uint32_t des, uint32_t sram, uint32_t length, uint32_t size, uint32_t gsum, uint32_t crc)
{
	uint8_t out[5];

	nvt_set_page(des);
	out[0] = (uint8_t)(des & NVT_READ_MASK);
	out[1] = (uint8_t)(sram & 0xFF);
	out[2] = (uint8_t)((sram >> 8) & 0xFF);
	out[3] = (uint8_t)((sram >> 16) & 0xFF);
	nvt_write_buf(out, 4);

	out[0] = (uint8_t)(length & NVT_READ_MASK);
	out[1] = (uint8_t)(size & 0xFF);
	out[2] = (uint8_t)((size >> 8) & 0xFF);
	out[3] = (uint8_t)((size >> 16) & 0x01);
	nvt_write_buf(out, NVT_HW_CRC > 1 ? 4 : 3);

	out[0] = (uint8_t)(gsum & NVT_READ_MASK);
	out[1] = (uint8_t)(crc & 0xFF);
	out[2] = (uint8_t)((crc >> 8) & 0xFF);
	out[3] = (uint8_t)((crc >> 16) & 0xFF);
	out[4] = (uint8_t)((crc >> 24) & 0xFF);
	nvt_write_buf(out, 5);
}

static void
nvt_set_bld_hw_crc(void)
{
	nvt_set_bld_crc_bank(NVT_ILM_DES_ADDR, NVT_FW_ILM_SRAM, NVT_ILM_LENGTH_ADDR,
	    NVT_FW_ILM_SIZE, NVT_G_ILM_CHECKSUM_ADDR, NVT_FW_ILM_CRC);
	nvt_set_bld_crc_bank(NVT_DLM_DES_ADDR, NVT_FW_DLM_SRAM, NVT_DLM_LENGTH_ADDR,
	    NVT_FW_DLM_SIZE, NVT_G_DLM_CHECKSUM_ADDR, NVT_FW_DLM_CRC);
}

/*
 * nvt_bld_crc_enable(): one bit, read-modify-written, in the bootloader's own control byte.
 *
 * Through nvt_read() rather than a hand-rolled transfer, because the vendor's CTP_SPI_READ puts
 * the first real byte at buf[1] only after shifting the received stream by two -- the address it
 * sent and the dummy that follows it. A raw spi_xfer() leaves that shifting to the caller, and the
 * first version of this function read the dummy instead of the register.
 */
static void
nvt_bld_crc_enable(void)
{
	uint8_t value = 0;

	nvt_set_page(NVT_BLD_CRC_EN_ADDR);
	if (!nvt_read(NVT_BLD_CRC_EN_ADDR & NVT_READ_MASK, &value, 1)) {
		return;
	}
	nvt_write_addr(NVT_BLD_CRC_EN_ADDR, (uint8_t)(value | 0x80));
}

/* nvt_fw_crc_enable(): tell the firmware to check itself once it starts. */
static void
nvt_fw_crc_enable(void)
{
	uint8_t out[2];

	nvt_set_page(NVT_EVENT_BUF_ADDR);
	out[0] = NVT_EVENT_MAP_RESET_COMPLETE & NVT_READ_MASK;
	out[1] = 0x00;
	nvt_write_buf(out, 2);
	out[0] = NVT_EVENT_MAP_HOST_CMD & NVT_READ_MASK;
	out[1] = 0xAE;
	nvt_write_buf(out, 2);
}

/* nvt_boot_ready(): release the MCU. With hardware CRC the vendor does not clear the bit again. */
static void
nvt_boot_ready(void)
{
	nvt_write_addr(NVT_BOOT_RDY_ADDR, 1);
	delay(5000);
}

/*
 * nvt_eng_reset(): the first thing the vendor's probe does.
 *
 * nt36xxx.c:1598 runs nvt_eng_reset() -- one write of 0x5A to ENG_RST_ADDR 0x7FFF80, then a
 * millisecond -- before it asks for the trim id, and only then registers the input device and
 * queues the firmware push fourteen seconds later. This driver had the address defined and never
 * used it, which means it has been talking to a chip nobody put into a known state first.
 *
 * It is behind touch_eng_rst rather than unconditional: the reset is harmless in the vendor's
 * order but this driver's order is not the vendor's, and a change that cannot be undone without
 * a reflash should be something the log can attribute.
 */
static void
nvt_eng_reset(void)
{
	nvt_write_addr(NVT_ENG_RST_ADDR, 0x5A);
	delay(1000);
}

/* nvt_bootloader_reset(): park the MCU, then turn off its fast read, which changes frame shapes. */
static void
nvt_bootloader_reset(void)
{
	nvt_write_addr(NVT_SWRST_N8_ADDR, 0x69);
	delay(5000);
	nvt_write_addr(NVT_SPI_RD_FAST_ADDR, 0x00);
}

/*
 * nvt_check_fw_reset_state(): wait for the firmware to reach `want`. The four states are a
 * sequence, not a threshold: 0xA0 just reset, 0xA1 baseline running, 0xA2 baseline done, 0xA3
 * normal. Waiting for 0xA0 proves only that the MCU restarted; a report exists from 0xA3 on.
 * The vendor allows ten tries for INIT and fifty for anything later, ten milliseconds each.
 */
static bool
nvt_check_fw_reset_state(uint8_t want, uint8_t *last, uint8_t *raw)
{
	uint8_t buf[8];
	unsigned tries = want == NVT_RESET_STATE_INIT ? 10u : 50u;

	/*
	 * The page is latched once, outside the loop, the way nvt_check_fw_reset_state() does it in
	 * the vendor driver. Latching it on every pass was a difference between the two, and the
	 * baseline runs while this loop polls: page selects are not obviously free on this chip.
	 */
	nvt_set_page(NVT_EVENT_BUF_ADDR | NVT_EVENT_MAP_RESET_COMPLETE);
	for (unsigned i = 0; i < tries; i++) {
		if (nvt_read(NVT_EVENT_MAP_RESET_COMPLETE, buf, 5)) {
			*last = buf[0];
			if (raw) {
				for (unsigned k = 0; k < 5; k++) {
					raw[k] = buf[k];
				}
			}
			if (buf[0] >= want && buf[0] <= NVT_RESET_STATE_MAX) {
				return true;
			}
		}
		delay(10000);
	}
	return false;
}

/*
 * nvt_read_bld_hw_crc(): what the bootloader made of the image it was given.
 *
 * This is the question the reset state cannot answer. 0xA0 says the MCU restarted, but not that
 * the image was accepted -- and a rejected image looks exactly like that from the outside: the
 * reset state reaches 0xA0 and then nothing reports, because the CRC check refused to jump into
 * the code. These five registers are the check itself: BLD_ILM_DLM_CRC holds done/ILM/DLM flags,
 * and for each of ILM and DLM the golden checksum the driver supplied sits next to the one the
 * hardware computed over what actually arrived in SRAM.
 */
static void
nvt_read_bld_hw_crc(void)
{
	uint8_t flag[2] = { 0 }, ilm_g[4] = { 0 }, ilm_r[4] = { 0 };
	uint8_t dlm_g[4] = { 0 }, dlm_r[4] = { 0 };
	uint32_t g_ilm, r_ilm, g_dlm, r_dlm;

	/*
	 * nvt_read() does not set a page -- it reads at an offset in whichever page the chip is
	 * already on, the way nvt_ts_work_func() relies on. The vendor therefore calls
	 * nvt_set_page(addr) before every one of these; the first version here passed the whole
	 * address to nvt_read(), which masked it to seven bits and read five offsets of the event
	 * buffer instead. That is where a golden ILM checksum of 77777777 came from.
	 */
	nvt_set_page(NVT_BLD_ILM_DLM_CRC_ADDR);
	if (!nvt_read(NVT_BLD_ILM_DLM_CRC_ADDR & NVT_READ_MASK, flag, 1)) {
		printf("nvt: the bootloader's crc flags could not be read\n");
		return;
	}
	nvt_set_page(NVT_G_ILM_CHECKSUM_ADDR);
	nvt_read(NVT_G_ILM_CHECKSUM_ADDR & NVT_READ_MASK, ilm_g, 4);
	nvt_read(NVT_R_ILM_CHECKSUM_ADDR & NVT_READ_MASK, ilm_r, 4);
	nvt_read(NVT_G_DLM_CHECKSUM_ADDR & NVT_READ_MASK, dlm_g, 4);
	nvt_read(NVT_R_DLM_CHECKSUM_ADDR & NVT_READ_MASK, dlm_r, 4);
	g_ilm = ilm_g[0] | (ilm_g[1] << 8) | (ilm_g[2] << 16) | ((uint32_t)ilm_g[3] << 24);
	r_ilm = ilm_r[0] | (ilm_r[1] << 8) | (ilm_r[2] << 16) | ((uint32_t)ilm_r[3] << 24);
	g_dlm = dlm_g[0] | (dlm_g[1] << 8) | (dlm_g[2] << 16) | ((uint32_t)dlm_g[3] << 24);
	r_dlm = dlm_r[0] | (dlm_r[1] << 8) | (dlm_r[2] << 16) | ((uint32_t)dlm_r[3] << 24);

	printf("nvt: crc flags %02x (done %u ilm %u dlm %u); ilm %08x vs %08x%s; dlm %08x vs %08x%s\n",
	    flag[0], (flag[0] >> 2) & 1u, flag[0] & 1u, (flag[0] >> 1) & 1u,
	    r_ilm, g_ilm, r_ilm == g_ilm ? " same" : " DIFFER",
	    r_dlm, g_dlm, r_dlm == g_dlm ? " same" : " DIFFER");
}

/*
 * find_fw_state(): after the image is in, wait for the firmware to finish bringing itself up.
 *
 * 0xA0 and 0xA1 are the chip working on a baseline; until 0xA2 finishes it and 0xA3 follows, the
 * event buffer still holds the fill pattern the bootloader leaves behind -- six bytes of 0x77,
 * which is exactly what nvt_fw_recovery() in the vendor driver treats as "the firmware is not
 * ready" and what this driver kept reading as a bad report. Waiting here is what turns that into
 * a real report; there is nothing else to do about it.
 */
static uint8_t
find_fw_state(uint8_t *state, uint8_t *raw)
{
	/*
	 * The vendor stops waiting at RESET_STATE_REK (0xA1) and goes straight to nvt_get_fw_info(),
	 * which is why its budget of fifty ten-millisecond polls looked sufficient. On this chip the
	 * baseline takes longer than that: a1 is still there half a second later, and until 0xA2 the
	 * event buffer keeps the bootloader's 0x77 fill -- six bytes of it, which is the pattern
	 * nvt_fw_recovery() names as "the firmware is not ready" and this driver kept reporting as a
	 * bad report. Five seconds, in ten-millisecond steps, then the caller decides what to say.
	 */
	uint8_t seen = 0;

	/*
	 * Sixty rounds of half a second is thirty seconds. The previous budget was five, which is
	 * enough for one image and not for another -- so the budget was guessing. Every change of
	 * state is printed instead: if the baseline is merely slow the log shows a1 then a2 then a3,
	 * and if it is stuck the log shows one value for thirty seconds and says so.
	 */
	for (unsigned round = 0; round < 60; round++) {
		uint8_t now = 0;

		if (nvt_check_fw_reset_state(NVT_RESET_STATE_REK_FINISH, &now, raw)) {
			printf("nvt: the baseline finished after %u rounds; state %02x\n", round, now);
			*state = now;
			return NVT_RESET_STATE_NORMAL_RUN;
		}
		if (now != seen) {
			printf("nvt: the baseline is at %02x (round %u)\n", now, round);
			seen = now;
		}
		*state = now;
	}
	printf("nvt: the baseline never left %02x in thirty seconds\n", seen);
	return NVT_RESET_STATE_MAX;
}

/*
 * nvt_download_firmware(): nvt_download_firmware_hw_crc() with the retries it has -- three attempts
 * at the whole sequence, since a transfer that fails its own bootloader CRC leaves the MCU parked
 * and the next attempt starts from a clean park anyway.
 */
static bool
nvt_download_firmware(void)
{
	uint8_t state = 0, raw[5] = { 0 };

	for (unsigned attempt = 1; attempt <= 3; attempt++) {
		nvt_bootloader_reset();
		nvt_write_firmware();
		nvt_set_bld_hw_crc();
		nvt_bld_crc_enable();
		nvt_fw_crc_enable();
		nvt_boot_ready();
		if (nvt_check_fw_reset_state(NVT_RESET_STATE_INIT, &state, raw)) {
			printf("nvt: the image was accepted on attempt %u; the reset state reads %02x\n",
			    attempt, state);
			nvt_read_bld_hw_crc();
			switch (find_fw_state(&state, raw)) {
			case NVT_RESET_STATE_NORMAL_RUN:
				printf("nvt: the firmware finished its baseline and is reporting\n");
				return true;
			case NVT_RESET_STATE_MAX:
				printf("nvt: the firmware is still starting (%02x); the reports will come\n",
				    state);
				return true;
			default:
				printf("nvt: the firmware stopped at %02x, which is not a running state\n",
				    state);
				break;
			}
			return false;
		}
		printf("nvt: attempt %u left the reset state at %02x (%02x %02x %02x %02x %02x)\n",
		    attempt, state, raw[0], raw[1], raw[2], raw[3], raw[4]);
		nvt_read_bld_hw_crc();
	}
	return false;
}

/* ---- the thread that watches the line ---- */


static void
nvt_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t data[POINT_DATA_LEN];
	uint32_t poll_us = 4000, raw = 0, idle_ok = 0, idle_bad = 0;
	uint32_t page = NVT_EVENT_BUF_ADDR;
	bool poll_anyway = false;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("fbfit", &fit, sizeof(fit));
	PE_parse_boot_argn("touch_raw", &raw, sizeof(raw));
	PE_parse_boot_argn("nvt_page", &page, sizeof(page));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 4000;
	}
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	/*
	 * One line per step, because the first version of this thread went quiet after its first
	 * printf and there was no way to tell which of the few calls in between had stopped it: the
	 * only registers it had touched so far were in GPIO groups 3 and below, and the next thing it
	 * did was read group 4 for the first time (the interrupt line, GPIO135). If reading a group
	 * whose clock is off hangs the interconnect rather than raising a data abort -- which is what
	 * the log looked like: no panic, no fault, just silence while the rest of the system kept
	 * drawing frames -- then the step count below is what names it.
	 */
	printf("nvt: step 1, the gpio block is mapped at %#lx\n", (unsigned long)gpio);

	/*
	 * What the bootloader left in the function-select fields, read before this driver writes
	 * them. The Android pinctrl dump taken while the touch screen worked there shows
	 * MI=1 CS=0 MO=1 CLK=1 IRQ=0 -- three of the five are not on function zero while the chip
	 * is being talked to, and the one that is (CS) is the one the vendor's own pinctrl state
	 * drives as plain GPIO. Bit-banging works with all five at zero because the pad then
	 * belongs to the GPIO block either way, but a chip ID and a firmware CRC both arriving
	 * through a path nobody has measured is not the same as a path known to be right, and this
	 * line is the measurement that separates the two.
	 */
	printf("nvt: step 1a, the function select was MI %u CS %u MO %u CLK %u IRQ %u\n",
	    (rd(GPIO_MODE_BASE + (PIN_MI / 8) * GPIO_MODE_STEP) >> ((PIN_MI % 8) * 4)) & 0xfu,
	    (rd(GPIO_MODE_BASE + (PIN_CS / 8) * GPIO_MODE_STEP) >> ((PIN_CS % 8) * 4)) & 0xfu,
	    (rd(GPIO_MODE_BASE + (PIN_MO / 8) * GPIO_MODE_STEP) >> ((PIN_MO % 8) * 4)) & 0xfu,
	    (rd(GPIO_MODE_BASE + (PIN_CLK / 8) * GPIO_MODE_STEP) >> ((PIN_CLK % 8) * 4)) & 0xfu,
	    (rd(GPIO_MODE_BASE + (PIN_IRQ / 8) * GPIO_MODE_STEP) >> ((PIN_IRQ % 8) * 4)) & 0xfu);

	pins_as_gpio();
	printf("nvt: step 2, the five pins are plain gpio and their directions are set\n");

	/*
	 * The vendor's probe resets the controller before it reads anything (nt36xxx.c:1676), and
	 * it does so for a reason that this driver had backwards for a while: the reset puts the
	 * chip into its bootloader, and nothing runs on the other side of that bus until an image
	 * has been written back and the reset state has reached RESET_STATE_INIT. A run that reset
	 * the chip and then skipped the download (touch_eng_rst=1 with touch_keep_fw=1) therefore
	 * read a bootloader that was waiting for an image, which is exactly what its FWINFO looked
	 * like: ver c0, node counts that match no panel, and 0x77 everywhere the event buffer
	 * should have been. The two switches answer different questions and must not be combined.
	 */
	uint32_t eng = 0;
	uint32_t keep = 0;

	PE_parse_boot_argn("touch_eng_rst", &eng, sizeof(eng));
	PE_parse_boot_argn("touch_keep_fw", &keep, sizeof(keep));

	if (eng != 0 && keep != 0) {
		/*
		 * Refuse the combination rather than run it. Resetting into the bootloader and then
		 * declining to load an image leaves the part in a state this driver cannot read, and
		 * the readings it produced were mistaken for evidence about the panel and the wiring.
		 */
		printf("nvt: touch_eng_rst and touch_keep_fw ask for opposite things; also "
		    "dropping the reset so the chip keeps whatever boot chain left in it\n");
		eng = 0;
	}

	if (eng != 0) {
		nvt_eng_reset();
		printf("nvt: step 2b, eng reset sent (0x5A to 0x7FFF80) and one millisecond given\n");
	}

	/*
	 * Latch the page once, the way probe does, and then read address 0 for every report: each
	 * report read must not carry a page write of its own, because that would sit between the
	 * interrupt line going low and the data coming back.
	 */
	nvt_set_page(page);
	printf("nvt: step 3, page %#x is latched; the bus is GPIO%d/%d/%d/%d driven by hand\n",
	    page, PIN_MI, PIN_CS, PIN_MO, PIN_CLK);


	/*
	 * Group 3 first: pin_settle() reads it, so this one is already known to answer. It is
	 * printed as a control, so that a value arriving here and silence at step 5 is evidence
	 * about the group rather than about the whole block.
	 */
	printf("nvt: step 4, din of the bus pins' group reads %#x\n", rd(GPIO_DIN(GROUP_OF(PIN_MI))));

	printf("nvt: step 5, about to read the interrupt line's group\n");
	{
		uint32_t raw_irq = rd(GPIO_DIN(GROUP_OF(PIN_IRQ)));

		printf("nvt: step 6, din of the interrupt line's group reads %#x, so GPIO%d is %d\n",
		    raw_irq, PIN_IRQ, (raw_irq & BIT_OF(PIN_IRQ)) ? 1 : 0);
	}
	printf("nvt: the touch screen's interrupt line (GPIO%d) is looked at every %u us\n",
	    PIN_IRQ, poll_us);

	/*
	 * Then read the bus whether or not the chip asks for it, four times.
	 *
	 * Everything above proves only that the GPIO block answers; it says nothing about whether
	 * the chip is out of reset, because the main loop below only reads when the interrupt line
	 * is high and that line has read low on every boot so far. Four unconditional reads settle
	 * it in one run: a chip that is alive answers with bytes that look like a report or at least
	 * with something other than 00 and ff, and a chip that is held in reset leaves the bus
	 * pulled to one level, which reads as all 00 or all ff.
	 *
	 * The page is already latched, so these are the same four bytes a real interrupt would have
	 * fetched, only fetched on our schedule instead of the chip's.
	 */
	for (unsigned probe = 0; probe < 4; probe++) {
		uint8_t probe_data[POINT_DATA_LEN];

		nvt_read(0, probe_data, POINT_DATA_LEN);
		printf("nvt: probe %u reads %02x %02x %02x %02x %02x %02x %02x %02x, last %02x, %s\n",
		    probe, probe_data[0], probe_data[1], probe_data[2], probe_data[3],
		    probe_data[4], probe_data[5], probe_data[6], probe_data[7],
		    probe_data[POINT_DATA_LEN - 1],
		    report_ok(probe_data) ? "checksum good" : "checksum bad");
		/*
		 * The bus is pulled to one level when nothing drives it, so a chip that is out of
		 * reset cannot answer with all zeros or all ones across a whole report. Either
		 * extreme means the reads reached nothing, and polling anyway would only fill the
		 * log with the same bytes.
		 */
		uint8_t all_and = 0xff, all_or = 0;

		for (unsigned i = 0; i < POINT_DATA_LEN; i++) {
			all_and = (uint8_t)(all_and & probe_data[i]);
			all_or = (uint8_t)(all_or | probe_data[i]);
		}
		/*
		 * "Uniform" is the tell, not any particular level. A bus that nothing drives reads
		 * as every byte the same -- all 00 when pulled down, all ff when pulled up -- so the
		 * test is whether the extremes agree, not whether they sit at 00 and ff. (The first
		 * version of this checked `and != 0 || or != 0xff`, which a real report passes in
		 * reverse: a report holds a 00 somewhere and the tail is ff, so both halves of that
		 * test are false and a live chip was called undriven.)
		 */
		if (all_and != all_or) {
			/* Something drove the bus: stop treating the interrupt line as the gate. */
			poll_anyway = true;
		}
		delay(2000);
	}
	/*
	 * The four reads above say whether the chip's SRAM answers at all, which is worth knowing;
	 * they do not say the bus may be read freely. Reports are taken strictly on the interrupt
	 * line -- see the comment on the main loop for why polling turned a touch into four copies
	 * of the bootloader's fill.
	 */
	printf("nvt: the event buffer %s at rest; reports are taken on the line only\n",
	    poll_anyway ? "holds data" : "reads back as it was");

	/*
	 * What state is the firmware in? The vendor asks two read-only questions before it trusts a
	 * single report, and neither had ever been asked here:
	 *
	 *   nvt_clear_fw_status()  sets the page to EVENT_BUF_ADDR | 0x51, reads two bytes, and
	 *                          expects the second to be 0x00 -- that is the handshake byte the
	 *                          firmware clears when it has nothing in flight.
	 *   nvt_check_fw_status()  does the same read and expects (byte & 0xF0) == 0xA0, which is
	 *                          the "firmware is running" answer.
	 *
	 * A chip that answers 0x00 to both is alive but idle; one that answers neither is still in
	 * its bootloader or has not finished booting its own firmware -- and that would explain a
	 * report whose 65 bytes are stable, structured and never checksum: they are not a report.
	 */
	{
		uint8_t hand[8];
		uint32_t status = 0;

		nvt_set_page(page | 0x51u);
		/*
		 * One byte, not two: the vendor calls CTP_SPI_READ(client, buf, 2) and reads buf[1],
		 * and that convention -- an argument of n, with the first n-1 bytes landing in
		 * buf[1..n-1] -- is what nvt_read() reproduces as data[0..n-2]. So the handshake byte
		 * is data[0] here, and reading two and looking at the second would be off by one.
		 */
		if (nvt_read(0x51u, hand, 1)) {
			printf("nvt: fw handshake reads %02x at page %#x; firmware %s\n",
			    hand[0], page | 0x51u,
			    (hand[0] & 0xF0u) == 0xA0u ? "is running" :
			    (hand[0] == 0x00u ? "has nothing queued" : "answers neither way"));
			status = hand[0];
		} else {
			printf("nvt: the fw handshake could not be read\n");
		}
		nvt_set_page(page);

		/*
		 * And the reset state, 0xA0..0xAF: 0xA0 means the chip has just reset, 0xA1 has a
		 * baseline, 0xA2 finished it, 0xA3 is normal running. Anything below 0xA0 means the
		 * firmware never reached a usable state, and a report cannot exist yet.
		 */
		nvt_set_page(page | 0x60u);
		if (nvt_read(0x60u, hand, 5)) {
			printf("nvt: reset state reads %02x %02x %02x %02x %02x (%s)\n",
			    hand[0], hand[1], hand[2], hand[3], hand[4],
			    hand[0] == 0xA0u ? "just reset" : hand[0] == 0xA1u ? "baseline" :
			    hand[0] == 0xA2u ? "baseline finished" :
			    hand[0] == 0xA3u ? "normal" : "not in 0xA0..0xAF");
		} else {
			printf("nvt: the reset state could not be read\n");
		}
		nvt_set_page(page);
		(void) status;
	}

	/*
	 * Which chip is this, and at which address does it keep its report?
	 *
	 * The probe above says the bus answers and the bytes are stable, which means the chip is
	 * out of reset and running its own firmware; a checksum that never matches then says the
	 * address is wrong rather than the chip being dead. Two read-only questions settle it:
	 *
	 *   1. The chip ID lives at page 0x1F64E, address 0x4E, six bytes, and nt36xxx.c matches
	 *      those six against trim_id_table to pick the memory map -- so the ID names both the
	 *      part and the map, and the map names EVENT_BUF_ADDR.
	 *   2. Failing that, try every EVENT_BUF_ADDR the maps in nt36xxx_mem_map.h carry: the one
	 *      that yields a good checksum on a real report is the right one.
	 *
	 * Both are pure reads: no reset, no page that would disturb a running firmware.
	 */
	/*
	 * Give the chip an image to run.
	 *
	 * This is what the vendor does at nt36xxx.c:1778 -- fourteen seconds after probing it queues
	 * Boot_Update_Firmware, which calls nvt_update_firmware() and writes ILM, DLM, the header
	 * block and both overlays into the controller's SRAM. It is not a recovery path: the chip
	 * comes up in its ROM bootloader with a readable trim id and nothing to report with, and
	 * every boot sends it the image again.
	 *
	 * Everything before this point was reading a chip that had no firmware. That is why the trim
	 * id answered (it lives in ROM), why the event buffer's bytes were stable within a run and
	 * different across runs (SRAM nothing was maintaining), why the handshake at 0x22D51 was
	 * neither 0xA0 nor 0x00, and why the reset state stayed at 0x3A -- below RESET_STATE_INIT
	 * 0xA0 -- through an eng reset, a 0x55 software reset and fifty polls.
	 *
	 * The writes touch nothing but this touch controller. "bootloader" here is the name of a few
	 * hundred bytes of Novatek boot code inside the chip, and has nothing to do with the
	 * phone's bootloader or its lock.
	 */
	/*
	 * Its keep is the default now.
	 *
	 * The Android driver reports fw_ver=22 through /proc/nvt_fw_version. None of the twelve
	 * novatek*.bin files in the firmware directory has version 22 -- the closest are 5, 17 and
	 * 201 -- so what Android is talking to is the image in the chip's own flash, not one it
	 * pushed. That matters here: this driver has twice written its own image (version 5, then
	 * version 201), and twice the chip answered FWINFO correctly and then reported nothing but
	 * the bootloader's 0x77 fill. Reading a chip that was never overwritten is the control
	 * experiment that tells the two apart.
	 *
	 * keep=1 skips the download and leaves whatever is in flash alone.
	 */
	if (keep != 0) {
		printf("nvt: leaving the image in the chip alone; no download\n");
	} else if (nvt_download_firmware()) {
		printf("nvt: the chip is running the image this driver sent it\n");
		/*
		 * Then let the firmware settle. Every read of the event buffer moves the
		 * chip's SPI state machine on by one report, so a driver that reads four
		 * thousand times a second is not observing the chip, it is pacing it.
		 */
		delay(200000);
	} else {
		printf("nvt: the image did not take; the bus is there but the chip will not report\n");
	}

	{
		uint8_t id[8];

		nvt_set_page(0x1F64Eu);
		if (nvt_read(0x4Eu, id, 6)) {
			printf("nvt: chip id reads %02x %02x %02x %02x %02x %02x\n",
			    id[0], id[1], id[2], id[3], id[4], id[5]);
		} else {
			printf("nvt: the chip id could not be read\n");
		}
	}

	/*
	 * The firmware's own idea of itself, read the way nvt_get_fw_info() reads it: the event
	 * buffer's FWINFO window, seventeen bytes. A chip whose image is running answers here with
	 * its version, its panel's node counts and its abs limits -- and nvt_get_fw_info() calls
	 * the answer broken when the version and its complement do not add up to 0xFF. That is the
	 * one reading that separates "the image was accepted" from "the image is running".
	 */
	{
		uint8_t info[20];

		nvt_set_page(NVT_EVENT_BUF_ADDR | NVT_EVENT_MAP_FWINFO);
		if (nvt_read(NVT_EVENT_MAP_FWINFO, info, 17)) {
			printf("nvt: fw info ver %02x (~%02x) xnum %u ynum %u xmax %u ymax %u buttons %u; %s\n",
			    info[0], info[1], info[2], info[3],
			    (unsigned)((info[4] << 8) | info[5]),
			    (unsigned)((info[6] << 8) | info[7]),
			    info[10],
			    ((unsigned)info[0] + info[1]) == 0xFFu ? "consistent" : "broken");
		} else {
			printf("nvt: the fw info could not be read\n");
		}
	}

	{
		static const uint32_t pages[] = { 0x22D00u, 0x21C00u, 0x11E00u, 0x11A00u };

		for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
			uint8_t here[POINT_DATA_LEN];

			nvt_set_page(pages[i]);
			if (!nvt_read(0, here, POINT_DATA_LEN)) {
				printf("nvt: page %#x could not be read\n", pages[i]);
				continue;
			}
			printf("nvt: page %#x reads %02x %02x %02x %02x %02x %02x %02x %02x, %s\n",
			    pages[i], here[0], here[1], here[2], here[3], here[4], here[5],
			    here[6], here[7], report_ok(here) ? "checksum good" : "checksum bad");
		}
		nvt_set_page(page);
	}

	/*
	 * And how often does the interrupt line actually go high? A line that never moves while
	 * the chip answers on the bus means the chip has nothing to report (no one is touching it),
	 * which is the expected state at boot; a line that moves means reports are being missed
	 * because this thread is polling instead of taking an interrupt.
	 */
	{
		/*
		 * Counting edges, not levels.
		 *
		 * The earlier version of this sampled the line every hundred microseconds and reported
		 * "high 0 times and low 20000". That number said more about the sampler than the line:
		 * Android's own interrupt controller logs this pin as an edge source and had counted
		 * thirty-five thousand of them, so the pulses exist and are shorter than the gap. A
		 * spin loop sees the line at roughly register-read speed, which is what it takes.
		 */
		unsigned edges = 0, high = 0, spins = 0;
		bool last = pin_in(PIN_IRQ);

		for (unsigned n = 0; n < 2000000 && edges < 1000; n++) {
			bool now = pin_in(PIN_IRQ);

			spins++;
			if (now) {
				high++;
			}
			if (now && !last) {
				edges++;
			}
			last = now;
		}
		printf("nvt: over %u spins the interrupt line saw %u rising edges and was high %u times\n",
		    spins, edges, high);
	}

	/*
	 * Is GPIO135 even muxed to GPIO? Its mode register is at 0x300 + (135/8)*0x10 = 0x400, and
	 * that arithmetic assumes the mode block runs from 0x300 for every pin in order. Nothing has
	 * confirmed that for a pin this high: the tables read out of pinctrl-mt6895.ko showed eight
	 * entries per block, which would end at 0x370 and leave pin 135's mux somewhere else. Rather
	 * than keep guessing, print the whole mode block and every group's direction, output and
	 * input words, then compare them with what the vendor's own driver leaves behind: after
	 * Android has run, pin 135 is an input at mode 0, and the bus pins are mode 0 as well.
	 */
	printf("nvt: mode words 0x300..0x4f0:");
	for (uint32_t at = 0x300; at <= 0x4f0; at += 0x10) {
		printf(" %08x", rd(at));
	}
	printf("\n");
	for (unsigned g = 0; g < 8; g++) {
		printf("nvt: group %u dir %08x dout %08x din %08x\n", g,
		    rd(GPIO_DIR_SET(g) - 0x004u), rd(GPIO_DOUT_SET(g) - 0x104u),
		    rd(GPIO_DIN(g)));
	}

	/*
	 * A running count of the line's rising edges, and of the state the last stats line saw, so
	 * that a finger on the screen shows up as a number even when the reports themselves are the
	 * bootloader's 0x77 fill. Nothing else in this driver can tell "the chip never asked" from
	 * "the chip asked and the answer was unusable", and those two need different fixes.
	 */
	uint8_t pattern[6][5];
	uint32_t pattern_hits[6];
	unsigned patterns = 0;

	unsigned edges = 0;
	bool was_high = pin_in(PIN_IRQ);
	uint32_t since_stats = 0;
	uint32_t quiet_ticks = 0;

	/*
	 * The read budget has to live here and not inside the loop: it was declared in the body
	 * once, so every tick started again at zero, "taken < 16" was always true, and the loop
	 * read the event buffer on every pass instead of sixteen times per run.
	 */
	unsigned taken = 0;

	for (unsigned tick = 1;; tick++) {

		/*
		 * The line is active high: the vendor driver registers its handler with
		 * int_trigger_type = IRQ_TYPE_EDGE_RISING (nt36xxx.h), so the chip pulls it up to say
		 * it has something, and reads as 1 rather than 0. The first version of this file
		 * waited for a low level and so never read a single report.
		 */
		/*
		 * Strictly on the line, which is what the vendor does. poll_anyway was tried and it is
		 * the wrong idea: the chip's side of the bus is a state machine, and reading its event
		 * buffer when it has nothing queued advances that state machine past a report that has
		 * not arrived yet. That is exactly why a touch on this build produced four identical
		 * reads of the bootloader's 0x77 fill instead of a report -- the driver was asking every
		 * four milliseconds, whether or not there was anything to ask for. The edges counter
		 * is what makes the strict version decidable: zero edges while a finger is on the
		 * screen means the chip is not talking, more than zero means it is and the answer was
		 * still wrong.
		 */
		/*
		 * Read only when the chip asks, and look for the asking hard enough to catch it.
		 *
		 * Both extremes have now been tried and both were wrong. A strict gate that sampled the
		 * line once per tick read nothing for a whole run: the interrupt is an edge into
		 * MediaTek's external interrupt block -- the Android side reads "mtk-eint 135 Edge
		 * NVT-ts-spi" and counts tens of thousands of them -- so the line is high for
		 * microseconds while a once-per-tick sample comes around every two hundred
		 * milliseconds. Reading unconditionally instead got reads, but every read of an empty
		 * event buffer moves the chip's SPI state machine on by one report, so the driver spent
		 * the run pacing the firmware and collecting its fill pattern.
		 *
		 * The wait is now the sampling loop itself: the window is walked in fifty-microsecond
		 * steps and the read only happens when an assertion was seen, which is what the vendor
		 * driver does with its interrupt handler. Four thousand register reads across two
		 * hundred milliseconds costs nothing and sees every edge instead of gambling on one.
		 */
		{
			bool asserted = pin_in(PIN_IRQ);

			if (asserted && !was_high) {
				edges++;
			}
			was_high = asserted;
			for (unsigned step = 0; !asserted && step < NVT_IRQ_WINDOW_US / NVT_IRQ_STEP_US;
			    step++) {
				bool now;

				delay(NVT_IRQ_STEP_US);
				now = pin_in(PIN_IRQ);
				if (now) {
					asserted = true;
					if (!was_high) {
						edges++;
					}
				}
				was_high = now;
			}
			if (asserted && !was_high) {
				edges++;
				was_high = true;
			}
			if (!asserted) {
				/*
				 * Fall back to reading after a stretch of quiet, but at a cadence slow
				 * enough not to pace the chip: one read every two seconds. A chip that
				 * only asserts on touch would otherwise tell us nothing at all between
				 * touches, and this driver cannot tell "no touch" from "the sampler missed
				 * it" without occasionally asking.
				 *
				 * No extra delay here: the window above already took the two hundred
				 * milliseconds that used to be at the foot of the loop.
				 */
				if (++quiet_ticks >= NVT_QUIET_READ_TICKS) {
					quiet_ticks = 0;
				} else {
					continue;
				}
			} else {
				quiet_ticks = 0;
			}
			if (!nvt_read(0, data, POINT_DATA_LEN)) {
				break;
			}
			taken++;
			if (taken <= 3) {
				printf("nvt: read %u of the whole buffer:", taken);
				for (unsigned j = 0; j < POINT_DATA_LEN; j++) {
					printf(" %02x", data[j]);
					if (j % 20 == 19) {
						printf("\n");
						printf("nvt:   ");
					}
				}
				printf("  (%s)\n", report_ok(data) ? "checksum good" : "checksum bad");
			}
			{
				unsigned k;

				for (k = 0; k < patterns; k++) {
					if (pattern[k][0] == data[0] && pattern[k][1] == data[1] &&
					    pattern[k][2] == data[2] && pattern[k][3] == data[3] &&
					    pattern[k][4] == data[4]) {
						pattern_hits[k]++;
						break;
					}
				}
				if (k == patterns && patterns < 6) {
					for (unsigned j = 0; j < 5; j++) {
						pattern[patterns][j] = data[j];
					}
					pattern_hits[patterns] = 1;
					patterns++;
				}
			}
			if (!report_ok(data)) {
				bad++;
				/*
				 * The chip fills its event buffer with 0x77 until the firmware finishes its
				 * baseline, and this thread polls every four milliseconds, so an unbounded
				 * print here put a line in the log every two milliseconds and pushed
				 * everything else -- including whether anyone touched the screen -- out of
				 * the four megabytes it shares with the kernel. Eight lines at the start and
				 * then one every thousand, which is enough to see the pattern change and
				 * cheap enough that a minute of it is a few dozen lines.
				 */
				if (raw && (bad <= 8 || bad % 1000 == 0)) {
					printf("nvt: bad report %u, first bytes %02x %02x %02x %02x %02x %02x %02x "
					    "%02x, last %02x\n", bad, data[0], data[1], data[2], data[3], data[4],
					    data[5], data[6], data[7], data[POINT_DATA_LEN - 1]);
				}
				/*
				 * No continue here. It used to skip the delay at the foot of the loop, so a
				 * bad report turned the two-hundred-millisecond cadence into a two-and-a-half
				 * millisecond one -- measured at 378 reads in 1.044 s. The comment above says
				 * exactly what that does: reading a chip that has nothing queued advances its
				 * side of the bus past a report that has not arrived yet. The bad path is the
				 * one that must be slowest, and it was the fastest.
				 */
			} else {
				idle_ok++;
				/*
				 * Every report, not the first eight. The window was there to keep a busy log
				 * small, but a run collects a few dozen reports at most, and any fixed head
				 * count samples exactly the part of a session that nobody is asking about:
				 * calibration taps arrive after the screen has settled, which is well past
				 * eight. Decoded and raw together, so one line is enough to place a finger.
				 */
				if (raw) {
					/*
					 * Every slot, not only the first. The report carries MAX_FINGERS
					 * six-byte records and nothing in this driver says which one a
					 * resting finger lands in: the first slot is the one the vendor's
					 * example reads, but a firmware that pages its report differently
					 * puts the same finger in another record. Printing all of them
					 * turns "the coordinates are wrong" into "the coordinates are in
					 * slot n", which is a question one line of log can answer.
					 *
					 * Each record: [0] = (slot << 3) | state, [1] high x, [2] high y,
					 * [3] low nibbles of both, [4] width, [5] pressure.
					 */
					printf("nvt: slots");
					for (unsigned s = 0; s < MAX_FINGERS; s++) {
						uint32_t at = FINGER_BYTES * s;
						uint32_t id = data[at] >> 3;
						uint32_t st = data[at] & 0x07;

						if (st != 1 && st != 2) {
							continue;
						}
#if NVT_SUPER_10S
						printf(" %u:%u/%u,%u", id, st,
						    (((uint32_t)data[at + 1] << 8) | data[at + 2]) / 10u,
						    (((uint32_t)data[at + 3] << 8) | data[at + 4]) / 10u);
#else
						printf(" %u:%u/%u,%u", id, st,
						    ((uint32_t)data[at + 1] << 4) | (data[at + 3] >> 4),
						    ((uint32_t)data[at + 2] << 4) | (data[at + 3] & 0x0f));
#endif
					}
					printf(" all");
					for (unsigned b = 0; b < POINT_DATA_LEN; b++) {
						printf(" %02x", data[b]);
					}
					printf("\n");
				}
				report(data);
			}
		}
		/*
		 * Every five seconds rather than every sixty: the kernel only runs for seventy, and the
		 * thread starts at about twenty-seven, so a sixty-second cadence never fired once and
		 * every run looked silent no matter what the screen was doing.
		 */
		/*
		 * Watch every input bit of every group. If a finger on the screen moves a bit anywhere
		 * in the GPIO block, this prints it, and then the interrupt line is not GPIO135 or is
		 * not where the device tree says it is. If nothing anywhere moves while the screen is
		 * being stroked, the chip is not signalling at all and the wiring is not the question.
		 */
		/*
		 * What the buffer actually contains, not just how often a read failed. A single
		 * repeated pattern is a chip that is not writing; several patterns is a chip that is
		 * writing and whose framing this driver still has wrong. The first eight bytes are
		 * enough to tell those apart, and keeping a small table of them costs nothing.
		 */
		if (++since_stats >= 10u) {
			since_stats = 0;
			printf("nvt: %u distinct buffer patterns so far:", patterns);
			for (unsigned k = 0; k < patterns && k < 6; k++) {
				printf(" [%u x%u %02x %02x %02x %02x %02x]", k, pattern_hits[k],
				    pattern[k][0], pattern[k][1], pattern[k][2], pattern[k][3],
				    pattern[k][4]);
			}
			printf("\n");
			/*
			 * The buffer is deliberately not read here any more. This line used to peek at
			 * offset zero every five seconds whether or not the chip had asked to be read,
			 * which is the same desynchronisation the main loop is careful to avoid, and it
			 * fired often enough to eat whatever single report the chip had queued.
			 */
			printf("nvt: %u reports, %u positions, %u edges on the line, %u bad; line %s\n",
			    reports, touches, edges, bad, pin_in(PIN_IRQ) ? "asserted" : "idle");
		}
		/*
		 * Two hundred milliseconds between reads, not the four this loop started with. The
		 * number is not a tuning knob: it is what makes the read rate lower than the report
		 * rate by two orders of magnitude, so a read that finds 0x77 means the chip has
		 * nothing rather than that this thread took the report away.
		 */
		delay(200000);
	}
}

void
redwood_nvt_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	/* 3 is this phone; 1 is redwood's Goodix and 2 the X6826B's TD4160, each with a driver beside this. */
	if (!PE_parse_boot_argn("touchscreen", &which, sizeof(which)) || which != 3) {
		return;
	}
	if (kernel_thread_start(nvt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("nvt: cannot start the thread that reads the touch screen\n");
	}
}
