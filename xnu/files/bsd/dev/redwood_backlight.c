/*
 * LCD backlight for xaga (Redmi Note 11T Pro, MT6895).
 *
 * The panel's LED driver is an RT4831 behind the I2C6 gate IC at 0x11
 * (arch/arm64/boot/dts/vendor/mediatek: gate_ic@11, MI_I2C_LCD_BIAS).
 * The display loader leaves the IC powered but this kernel has no I2C
 * controller driver, so the two bus pins (GPIO31 SCL, GPIO32 SDA) are
 * driven by hand, as the touch and MT6375 drivers do.
 *
 * The register sequence is the one in the vendor's rt4831a_drv.c:
 *   CONFIG1=0x68, CONFIG2=0x9D, OPTION1=0x06, OPTION2=0xB7,
 *   ENABLE=0x15, SMOOTH=0x03; brightness is a 3-byte write to 0x04.
 *
 * This is the hardware half.  The iOS brightness slider needs a userland
 * hook (CoreBrightness/BrightnessSystem -> a kernel control); the default
 * here keeps the panel comfortably lit at 50 % and gives the later hook a
 * place to land.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/redwood_audio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

#define GPIO_PHYS       0x10005000
#define GPIO_SIZE       0x1000

#define BL_SCL          31
#define BL_SDA          32
#define LEDEN           215

#define RT4831_ADDR     0x11
#define RT_BL_CONFIG1   0x02
#define RT_BL_CONFIG2   0x03
#define RT_BL_BRIGHT_L  0x04
#define RT_BL_ENABLE    0x08
#define RT_BL_OPTION1   0x10
#define RT_BL_OPTION2   0x11
#define RT_BL_SMOOTH    0x14

#define BL_DEFAULT      800             /* 0..2047, the panel's effective 11-bit range */

/* The iOS brightness hook writes this through sysctl kern.redwood.backlight. */
extern struct sysctl_oid_list sysctl__kern_redwood_children;   /* SYSCTL_NODE in redwood_battery.c */
static int backlight_request = -1;
SYSCTL_INT(_kern_redwood, OID_AUTO, backlight, CTLFLAG_RW, &backlight_request, 0,
    "RT4831 backlight level, 0..2047 (written by the backboardd hook)");

#define GPIO_DIR_SET(n)  (0x004 + (n) * 0x10u)
#define GPIO_DIR_CLR(n)  (0x008 + (n) * 0x10u)
#define GPIO_DOUT_SET(n) (0x104 + (n) * 0x10u)
#define GPIO_DOUT_CLR(n) (0x108 + (n) * 0x10u)
#define GPIO_DIN(n)      (0x200 + (n) * 0x10u)
#define GPIO_MODE_BASE   0x300
#define GPIO_MODE_STEP   0x10
#define GROUP_OF(pin)    ((pin) / 32u)
#define BIT_OF(pin)      (1u << ((pin) % 32u))

static vm_offset_t gpio;
static int bl_level = BL_DEFAULT;

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
pin_release(uint32_t pin)
{
	wr(GPIO_DIR_CLR(GROUP_OF(pin)), BIT_OF(pin));
}

static inline void
pin_low(uint32_t pin)
{
	wr(GPIO_DOUT_CLR(GROUP_OF(pin)), BIT_OF(pin));
	wr(GPIO_DIR_SET(GROUP_OF(pin)), BIT_OF(pin));
}

static inline void
pin_high(uint32_t pin)
{
	wr(GPIO_DOUT_SET(GROUP_OF(pin)), BIT_OF(pin));
	wr(GPIO_DIR_SET(GROUP_OF(pin)), BIT_OF(pin));
}

static inline bool
pin_read(uint32_t pin)
{
	return (rd(GPIO_DIN(GROUP_OF(pin))) & BIT_OF(pin)) != 0;
}

static inline void
i2c_delay(void)
{
	delay(2);
}

static void
i2c_start(void)
{
	pin_release(BL_SDA);
	pin_release(BL_SCL);
	i2c_delay();
	pin_low(BL_SDA);
	i2c_delay();
	pin_low(BL_SCL);
	i2c_delay();
}

static void
i2c_stop(void)
{
	pin_low(BL_SDA);
	i2c_delay();
	pin_release(BL_SCL);
	i2c_delay();
	pin_release(BL_SDA);
	i2c_delay();
}

static bool
i2c_write_byte(uint8_t value)
{
	for (int bit = 7; bit >= 0; bit--) {
		if ((value >> bit) & 1) {
			pin_release(BL_SDA);
		} else {
			pin_low(BL_SDA);
		}
		i2c_delay();
		pin_release(BL_SCL);
		i2c_delay();
		pin_low(BL_SCL);
		i2c_delay();
	}
	pin_release(BL_SDA);
	i2c_delay();
	pin_release(BL_SCL);
	i2c_delay();
	bool ack = !pin_read(BL_SDA);
	pin_low(BL_SCL);
	i2c_delay();
	return ack;
}

static bool
rt_write(uint8_t reg, const uint8_t *data, unsigned len)
{
	i2c_start();
	if (!i2c_write_byte((uint8_t)(RT4831_ADDR << 1)) || !i2c_write_byte(reg)) {
		i2c_stop();
		return false;
	}
	for (unsigned i = 0; i < len; i++) {
		if (!i2c_write_byte(data[i])) {
			i2c_stop();
			return false;
		}
	}
	i2c_stop();
	return true;
}

static bool
rt_write_byte(uint8_t reg, uint8_t value)
{
	return rt_write(reg, &value, 1);
}

static void
pins_init(void)
{
	const uint32_t pins[] = { BL_SCL, BL_SDA, LEDEN };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t pin = pins[i];
		uint32_t at = GPIO_MODE_BASE + (pin / 8) * GPIO_MODE_STEP;
		uint32_t shift = (pin % 8) * 4;

		wr(at, rd(at) & ~(0xfu << shift));
	}
	pin_release(BL_SCL);
	pin_release(BL_SDA);
	pin_high(LEDEN);
	__asm__ volatile ("dsb sy");
	(void)rd(GPIO_DIN(GROUP_OF(BL_SCL)));
}

static bool
rt_enable(void)
{
	bool ok = true;

	ok &= rt_write_byte(RT_BL_CONFIG1, 0x68);   /* pwm_enable = 0 */
	ok &= rt_write_byte(RT_BL_CONFIG2, 0x9D);
	ok &= rt_write_byte(RT_BL_OPTION1, 0x06);
	ok &= rt_write_byte(RT_BL_OPTION2, 0xB7);
	ok &= rt_write_byte(RT_BL_ENABLE, 0x15);
	ok &= rt_write_byte(RT_BL_SMOOTH, 0x03);
	return ok;
}

static bool
rt_set_brightness(int level)
{
	uint8_t cmd[3];

	if (level < 0) {
		level = 0;
	}
	if (level > 4095) {
		level = 4095;
	}
	cmd[0] = RT_BL_BRIGHT_L;
	cmd[1] = (uint8_t)(level & 0x07);
	cmd[2] = (uint8_t)((level >> 3) & 0xFF);
	bl_level = level;
	return rt_write(RT_BL_BRIGHT_L, cmd + 1, 2);
}


/* ---- the two Goodix TFA9874 amplifiers of xaga, for a first hardware probe ----------------------- */

#define TFA_RST         213
#define TFA_SPK_SW      160
#define TFA_REV         0x03

static void
tfa_say(const char *format, ...)
{
	char line[200];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	for (int i = 0; i < n; i++) {
		loader_log_putc(line[i]);
	}
	printf("%s", line);
}

static bool
i2c_read_byte(uint8_t * value, bool ack)
{
	uint8_t v = 0;

	pin_release(BL_SDA);
	for (int bit = 7; bit >= 0; bit--) {
		i2c_delay();
		pin_release(BL_SCL);
		i2c_delay();
		if (pin_read(BL_SDA)) {
			v |= (uint8_t)(1u << bit);
		}
		pin_low(BL_SCL);
	}
	if (ack) {
		pin_low(BL_SDA);
	} else {
		pin_release(BL_SDA);
	}
	i2c_delay();
	pin_release(BL_SCL);
	i2c_delay();
	pin_low(BL_SCL);
	pin_release(BL_SDA);
	*value = v;
	return true;
}

static bool
tfa_read(uint8_t address, uint8_t reg, uint16_t * value)
{
	uint8_t hi = 0, lo = 0;
	bool ack;

	i2c_start();
	ack = i2c_write_byte((uint8_t)(address << 1));
	if (!ack) {
		i2c_stop();
		return false;
	}
	if (!i2c_write_byte(reg)) {
		i2c_stop();
		return false;
	}
	i2c_start();
	if (!i2c_write_byte((uint8_t)((address << 1) | 1))) {
		i2c_stop();
		return false;
	}
	(void)i2c_read_byte(&hi, true);
	(void)i2c_read_byte(&lo, false);
	i2c_stop();
	*value = (uint16_t)((hi << 8) | lo);
	return true;
}

static void
tfa_pin_init(uint32_t pin, bool high)
{
	uint32_t at = GPIO_MODE_BASE + (pin / 8) * GPIO_MODE_STEP;
	uint32_t shift = (pin % 8) * 4;

	wr(at, rd(at) & ~(0xfu << shift));
	if (high) {
		pin_high(pin);
	} else {
		pin_low(pin);
	}
}

static void
tfa_probe_one(uint8_t address)
{
	uint16_t rev = 0, flags0 = 0, flags1 = 0, status = 0;
	bool ok;

	ok = tfa_read(address, TFA_REV, &rev);
	if (!ok) {
		tfa_say("tfa: 0x%02x: NO ACK\n", address);
		return;
	}
	(void)tfa_read(address, 0x00, &status);
	(void)tfa_read(address, 0x10, &flags0);
	(void)tfa_read(address, 0x11, &flags1);
	tfa_say("tfa: 0x%02x: rev 0x%04x%s, status 0x%04x, flags0 0x%04x, flags1 0x%04x\n",
	    address, rev, (rev & 0xff) == 0x74 ? " (TFA9874)" : "", status, flags0, flags1);
}

static void
tfa_probe(void)
{
	tfa_pin_init(TFA_RST, true);
	delay(10000);
	tfa_pin_init(TFA_RST, false);
	delay(10000);
	tfa_pin_init(TFA_SPK_SW, false);
	delay(1000);
	tfa_say("tfa: reset released, probing I2C6 0x34 and 0x35\n");
	tfa_probe_one(0x34);
	tfa_probe_one(0x35);
}


/* ---- a first xaga speaker test: TFA9873 on I2S3, bit-banged (diagnostic only) -------------------- */

#define TFA_I2S_BCK     21
#define TFA_I2S_LRCK    22
#define TFA_I2S_DATA    24

#define AFE_PHYS        0x1e100000u
#define AFE_SRAM_PHYS   0x1e102000u

static vm_offset_t afe_mmio;

static volatile int tfa_clock_run;
static volatile unsigned tfa_clock_frame;

static bool
tfa_write(uint8_t address, uint8_t reg, uint16_t value)
{
	i2c_start();
	if (!i2c_write_byte((uint8_t)(address << 1)) || !i2c_write_byte(reg) ||
	    !i2c_write_byte((uint8_t)(value >> 8)) || !i2c_write_byte((uint8_t)value)) {
		i2c_stop();
		return false;
	}
	i2c_stop();
	return true;
}

static void
tfa_init_one(uint8_t address)
{
	uint16_t v = 0;
	uint16_t id = 0;

	(void)tfa_read(address, 0x03, &id);
	tfa_write(address, 0x0f, 0x5a6b);
	(void)tfa_read(address, 0xfb, &v);
	tfa_write(address, 0xa0, (uint16_t)(v ^ 0x005a));
	tfa_write(address, 0xa1, 0x005a);
	tfa_write(address, 0x0f, 0x0000);
	/* the TFA9873 rev 0x0b73 init sequence from the vendor driver */
	tfa_write(address, 0x02, 0x0628); /* AUDFS = 48 kHz */
	tfa_write(address, 0x04, 0x0241);
	tfa_write(address, 0x05, 0x1000);
	tfa_write(address, 0x08, 0x0002);
	tfa_write(address, 0x61, 0x0183);
	tfa_write(address, 0x63, 0x005a);
	tfa_write(address, 0x6f, 0x0083);
	tfa_write(address, 0x70, 0xa3fb);
	tfa_write(address, 0x73, 0x0187);
	tfa_write(address, 0x83, 0x009a);
	tfa_write(address, 0x85, 0x0380);
	tfa_write(address, 0xd5, 0x004d);
	/* TDM/I2S input: two 32-bit slots, 64 BCK per frame, 16-bit sample */
	tfa_write(address, 0x20, 0x2883); /* Android TDM: 2 slots, 1-BCK FS */
	tfa_write(address, 0x21, 0xc1f0);
	tfa_write(address, 0x22, 0x047c);
	tfa_write(address, 0x23, 0x000b);
	tfa_write(address, 0x26, (address == 0x34) ? 0x0010 : 0x1001);
	tfa_write(address, 0x27, (address == 0x34) ? 0x0001 : 0x0000);
	/* set MANSCONF (hardware manager enable), then leave power-down */
	(void)tfa_read(address, 0x01, &v);
	v |= (1u << 2);
	tfa_write(address, 0x01, v);
	(void)tfa_read(address, 0x00, &v);
	v &= ~1u;
	v |= (1u << 3) | (1u << 4);
	tfa_write(address, 0x00, v);
	tfa_say("tfa: 0x%02x init, rev 0x%04x, ctrl 0x%04x, sys 0x%04x, gain 0x%04x\n", address, id, v,
	    ({ uint16_t m = 0; (void)tfa_read(address, 0x01, &m); m; }),
	    ({ uint16_t g = 0; (void)tfa_read(address, 0x52, &g); g; }));
}

static vm_offset_t afe_sram;
static volatile uint32_t afe_frame;
static volatile bool afe_started;
static uint64_t afe_last_write;

/* iOS already applies its software volume to these samples.  Keep them at
 * full scale here; the old /64 was 36 dB of extra attenuation and made the
 * speaker almost inaudible even at iOS volume 0.5. */
#define AFE_SINK_SHIFT  0

static void
aw(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(afe_mmio + offset) = value;
}

static uint32_t
ar(uint32_t offset)
{
	return *(volatile uint32_t *)(afe_mmio + offset);
}

/* From /dev/redwood-audio's writer: put the 16-bit stereo frames into the AFE's 16 KiB loop. */
static uint32_t
afe_sink(const int16_t *frames, uint32_t count)
{
	if (afe_mmio == 0 || afe_sram == 0) {
		return 0;
	}

	for (uint32_t i = 0; i < count; i++) {
		int16_t left = frames[2 * i] >> AFE_SINK_SHIFT;
		int16_t right = frames[2 * i + 1] >> AFE_SINK_SHIFT;
		uint32_t *slot = (uint32_t *)afe_sram + 2 * afe_frame;

		slot[0] = (uint32_t)(uint16_t)left << 16;
		slot[1] = (uint32_t)(uint16_t)right << 16;
		afe_frame = (afe_frame + 1) % 2048;
	}
	if (!afe_started) {
		aw(0x1b00, 0x00000010); /* AFE_AGENT_ON: DL1_ON */
		afe_started = true;
		tfa_say("tfa: AFE DMA started\n");
	}
	afe_last_write = mach_absolute_time();
	return count;
}

static void
afe_watchdog_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		delay(50000);
		if (afe_started) {
			uint64_t age = 0;
			absolutetime_to_nanoseconds(mach_absolute_time() - afe_last_write, &age);
			if (age > 300 * NSEC_PER_MSEC) {
				uint32_t *samples = (uint32_t *)afe_sram;
				for (unsigned i = 0; i < 4096; i++) {
					samples[i] = 0;
				}
				aw(0x1b00, 0x00000000);
				afe_started = false;
				afe_frame = 0;
				tfa_say("tfa: AFE underrun, loop stopped\n");
			}
		}
	}
}

static void
speaker_test_thread(__unused void *parameter, __unused wait_result_t result)
{
	vm_offset_t afe;

	delay(3000000);
	afe = ml_io_map(AFE_PHYS, 0x3000);
	afe_sram = ml_io_map(AFE_SRAM_PHYS, 0x4000);
	afe_mmio = afe;
	tfa_say("tfa: AFE map %#lx sram %#lx\n", (unsigned long)afe, (unsigned long)afe_sram);
	if (afe == 0 || afe_sram == 0) {
		return;
	}

	/* 16 KiB DL1 loop, initially silence, 48 kHz stereo 32-bit samples */
	uint32_t *samples = (uint32_t *)afe_sram;
	for (unsigned i = 0; i < 4096; i++) {
		samples[i] = 0;
	}
	aw(0x000c, 0x60000004);
	aw(0x0010, 0x00000001);
	aw(0x0020, 0x00000020);
	aw(0x0024, 0x00000040);
	aw(0x0038, 0x08000000);
	aw(0x0040, 0x00000a0b);
	aw(0x0048, 0xffffffff);
	aw(0x004c, 0x0a313020);
	aw(0x0050, 0x00000000);
	aw(0x0054, 0x1e102000);
	aw(0x0060, 0x00000000);
	aw(0x0064, 0x1e105fff);
	aw(0x0200, 0x00000000);
	aw(0x02ec, 0x00010001);
	aw(0x0310, 0x10000139);
	aw(0x0314, 0x00000001);
	/* The agent is started by the first sink write, so that the loop starts at its beginning. */
	tfa_init_one(0x34);
	tfa_init_one(0x35);
	tfa_pin_init(TFA_SPK_SW, true);
	redwood_audio_set_sink("the xaga AFE/I2S3 speakers", afe_sink);
	{
		thread_t watchdog;
		if (kernel_thread_start(afe_watchdog_thread, NULL, &watchdog) == KERN_SUCCESS) {
			thread_deallocate(watchdog);
		}
	}
	tfa_say("tfa: AFE DAC %#x I2S3 %#x DL1 %#x base %#x end %#x, audio sink ready\n",
	    ar(0x0010), ar(0x0040), ar(0x004c), ar(0x0054), ar(0x0064));
}

static void
backlight_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		int request = backlight_request;

		if (request >= 0 && request <= 4095 && request != bl_level) {
			(void)rt_set_brightness(request);
		}
		delay(100000);
	}
}

void
redwood_backlight_init(void)
{
	uint32_t arg = 0;

	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	if (gpio == 0) {
		printf("backlight: GPIO block could not be mapped\n");
		return;
	}
	pins_init();
	bool enabled = rt_enable();
	bool set = rt_set_brightness(BL_DEFAULT);
	printf("backlight: RT4831 %s, level %d %s; SCL %d SDA %d\n",
	    enabled ? "acked" : "no-ack", bl_level, set ? "set" : "failed",
	    pin_read(BL_SCL) ? 1 : 0, pin_read(BL_SDA) ? 1 : 0);
	(void)PE_parse_boot_argn("backlight", &arg, sizeof(arg));
	if (arg != 0) {
		(void)rt_set_brightness((int)arg);
	}
	tfa_probe();
	{
		uint32_t test = 1;   /* diagnostic boot: always try the tone */

		if (test != 0) {
			thread_t thread;

			if (kernel_thread_start(speaker_test_thread, NULL, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
			}
		}
	}
	{
		thread_t thread;

		if (kernel_thread_start(backlight_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
