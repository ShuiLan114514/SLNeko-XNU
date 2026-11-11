/*
 * Battery and charger status for xaga (Redmi Note 11T Pro, MT6895).
 *
 * The board has an MT6375 on I2C5 (address 0x34 for the PMU/charger bank, 0x1A for the
 * battery-monitor bank).  There is no I2C controller driver in this kernel, so this file
 * drives the two bus pins (GPIO33 SCL, GPIO34 SDA) by hand, the same way redwood_nvt.c drives
 * the touch bus.  It deliberately implements only what iOS needs to draw the battery:
 *
 *   - VBAT through the MT6375's VBAT monitor (raw * 1.25 mV);
 *   - PWR_RDY and the charger's IC_STAT nibble;
 *   - a voltage-derived state of charge (the real MT6375 coulomb counter is a much larger
 *     driver; this is the bring-up value, and the sysctl prints that it is estimated).
 *
 * The values are reported through redwood_battery_report(), which already owns the IOKit
 * RedwoodBattery power source.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GPIO_PHYS       0x10005000
#define GPIO_SIZE       0x1000

#define I2C_SCL         33
#define I2C_SDA         34

#define MT6375_PMU      0x34
#define MT6375_BM       0x1A

#define MT6375_REG_CHG_STAT       0x134
#define MT6375_REG_VBAT_MON_RPT   0x19C
#define MT6375_REG_ADC_CONFG1     0x1A4
#define MT6375_REG_CHG_STAT0      0x1E0

#define VBAT_MON_EN     (1u << 5)
#define PWR_RDY         (1u << 0)

#define BATTERY_FULL_UAH        5000000
#define BATTERY_DESIGN_UAH      5000000

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
static int last_percent = -1, last_external = -1, last_status = -1, last_mv = -1;
static unsigned reads, errors;

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
	/* input: the external pull-up makes the line high */
	wr(GPIO_DIR_CLR(GROUP_OF(pin)), BIT_OF(pin));
}

static inline void
pin_low(uint32_t pin)
{
	/* open drain: drive zero, then turn the pin into an output */
	wr(GPIO_DOUT_CLR(GROUP_OF(pin)), BIT_OF(pin));
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
	pin_release(I2C_SDA);
	pin_release(I2C_SCL);
	i2c_delay();
	pin_low(I2C_SDA);
	i2c_delay();
	pin_low(I2C_SCL);
	i2c_delay();
}

static void
i2c_stop(void)
{
	pin_low(I2C_SDA);
	i2c_delay();
	pin_release(I2C_SCL);
	i2c_delay();
	pin_release(I2C_SDA);
	i2c_delay();
}

static bool
i2c_write_byte(uint8_t value)
{
	for (int bit = 7; bit >= 0; bit--) {
		if ((value >> bit) & 1) {
			pin_release(I2C_SDA);
		} else {
			pin_low(I2C_SDA);
		}
		i2c_delay();
		pin_release(I2C_SCL);
		i2c_delay();
		pin_low(I2C_SCL);
		i2c_delay();
	}
	/* release SDA and read the acknowledgment */
	pin_release(I2C_SDA);
	i2c_delay();
	pin_release(I2C_SCL);
	i2c_delay();
	bool ack = !pin_read(I2C_SDA);
	pin_low(I2C_SCL);
	i2c_delay();
	return ack;
}

static uint8_t
i2c_read_byte(bool ack)
{
	uint8_t value = 0;

	pin_release(I2C_SDA);
	for (int bit = 7; bit >= 0; bit--) {
		i2c_delay();
		pin_release(I2C_SCL);
		i2c_delay();
		value = (uint8_t)((value << 1) | (pin_read(I2C_SDA) ? 1 : 0));
		pin_low(I2C_SCL);
		i2c_delay();
	}
	if (ack) {
		pin_low(I2C_SDA);
	} else {
		pin_release(I2C_SDA);
	}
	i2c_delay();
	pin_release(I2C_SCL);
	i2c_delay();
	pin_low(I2C_SCL);
	i2c_delay();
	pin_release(I2C_SDA);
	return value;
}

static bool
mt6375_read(uint8_t addr, uint8_t reg, uint8_t *data, unsigned len)
{
	i2c_start();
	if (!i2c_write_byte((uint8_t)(addr << 1)) ||
	    !i2c_write_byte(reg)) {
		i2c_stop();
		return false;
	}
	i2c_start();
	if (!i2c_write_byte((uint8_t)((addr << 1) | 1))) {
		i2c_stop();
		return false;
	}
	for (unsigned i = 0; i < len; i++) {
		data[i] = i2c_read_byte(i + 1 < len);
	}
	i2c_stop();
	return true;
}

static bool
mt6375_write(uint8_t addr, uint8_t reg, const uint8_t *data, unsigned len)
{
	i2c_start();
	if (!i2c_write_byte((uint8_t)(addr << 1)) ||
	    !i2c_write_byte(reg)) {
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
mt6375_read_byte(uint8_t addr, uint8_t reg, uint8_t *value)
{
	return mt6375_read(addr, reg, value, 1);
}

static bool
mt6375_write_byte(uint8_t addr, uint8_t reg, uint8_t value)
{
	return mt6375_write(addr, reg, &value, 1);
}

/* The board's two I2C5 pins, left as plain GPIO and released. */
static void
i2c_pins_init(void)
{
	const uint32_t pins[] = { I2C_SCL, I2C_SDA };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t pin = pins[i];
		uint32_t at = GPIO_MODE_BASE + (pin / 8) * GPIO_MODE_STEP;
		uint32_t shift = (pin % 8) * 4;

		wr(at, rd(at) & ~(0xfu << shift));
	}
	pin_release(I2C_SCL);
	pin_release(I2C_SDA);
	__asm__ volatile ("dsb sy");
	(void)rd(GPIO_DIN(GROUP_OF(I2C_SCL)));
}

static bool
read_vbat_mv(int *mv)
{
	uint8_t cfg, raw[2];

	if (!mt6375_read_byte(MT6375_PMU, (uint8_t)MT6375_REG_ADC_CONFG1, &cfg)) {
		return false;
	}
	if (!(cfg & VBAT_MON_EN)) {
		uint8_t enabled = (uint8_t)(cfg | VBAT_MON_EN);

		if (!mt6375_write_byte(MT6375_PMU, (uint8_t)MT6375_REG_ADC_CONFG1, enabled)) {
			return false;
		}
	}
	delay(5000);   /* ADC_CONV_TIME_US * 2 */
	if (!mt6375_read(MT6375_PMU, (uint8_t)MT6375_REG_VBAT_MON_RPT, raw, 2)) {
		return false;
	}
	if (cfg & VBAT_MON_EN) {
		(void)mt6375_write_byte(MT6375_PMU, (uint8_t)MT6375_REG_ADC_CONFG1, cfg);
	} else {
		(void)mt6375_write_byte(MT6375_PMU, (uint8_t)MT6375_REG_ADC_CONFG1,
		    (uint8_t)(cfg & ~VBAT_MON_EN));
	}
	*mv = (int)((((uint32_t)raw[0] << 8) | raw[1]) * 1250u / 1000u);
	return true;
}

/* A coarse Li-ion curve; the MT6375 coulomb counter is not implemented yet. */
static int
soc_from_mv(int mv)
{
	static const struct {
		int mv;
		int percent;
	} curve[] = {
		{ 4450, 100 }, { 4350, 98 }, { 4250, 92 }, { 4150, 82 }, { 4050, 70 },
		{ 3950, 58 }, { 3850, 45 }, { 3750, 32 }, { 3650, 20 }, { 3550, 10 },
		{ 3450, 4 }, { 3350, 0 },
	};

	if (mv >= curve[0].mv) {
		return 10000;
	}
	for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
		if (mv >= curve[i].mv) {
			int dv = curve[i - 1].mv - curve[i].mv;
			int dp = curve[i - 1].percent - curve[i].percent;
			int off = curve[i - 1].mv - mv;

			return (curve[i - 1].percent - off * dp / dv) * 100;
		}
	}
	return 0;
}

static void
battery_poll(void)
{
	uint8_t chg_stat = 0, pwr_stat = 0;
	int mv = 0;
	bool have_v = read_vbat_mv(&mv);
	bool have_chg = mt6375_read_byte(MT6375_PMU, (uint8_t)MT6375_REG_CHG_STAT, &chg_stat);
	bool have_pwr = mt6375_read_byte(MT6375_PMU, (uint8_t)MT6375_REG_CHG_STAT0, &pwr_stat);

	if (!have_v || mv <= 2500 || mv >= 5000) {
		errors++;
		return;
	}

	int external = have_pwr && (pwr_stat & PWR_RDY);
	int stat = have_chg ? (chg_stat & 0x0f) : 0;
	int status;

	/* Linux power_supply_status: 2 discharging, 1 charging, 4 full, 3 not charging */
	if (!external) {
		status = 2;
	} else if (stat == 7) {
		status = 4;
	} else if (stat >= 2 && stat <= 6) {
		status = 1;
	} else {
		status = 3;
	}

	struct redwood_battery_state b;
	int percent = soc_from_mv(mv);

	memset(&b, 0, sizeof(b));
	b.size = sizeof(b);
	b.percent_hundredths = percent;
	b.voltage_uv = mv * 1000;
	b.current_ua = 0;
	b.temperature_hundredths = 2500;
	b.present = 1;
	b.external = external;
	b.status = status;
	b.charge_uah = (int32_t)((int64_t)percent * BATTERY_FULL_UAH / 10000);
	b.full_uah = BATTERY_FULL_UAH;
	b.design_uah = BATTERY_DESIGN_UAH;
	b.cycles = 0;
	b.usb_type = external ? 2 : 0;
	b.usb_voltage_uv = external ? 5000000 : 0;
	b.usb_current_ua = external ? 500000 : 0;
	redwood_battery_report(&b);
	reads++;

	if (percent / 100 != last_percent || external != last_external || status != last_status ||
	    (mv / 10) != last_mv) {
		printf("xaga-battery: %d%% (%d mV), charger %d, status %d, chg_stat %#x, pwr %#x, "
		    "%u reads, %u errors\n", percent / 100, mv, external, status, chg_stat, pwr_stat,
		    reads, errors);
		last_percent = percent / 100;
		last_external = external;
		last_status = status;
		last_mv = mv / 10;
	}
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		battery_poll();
		delay(5000000);
	}
}

void
redwood_xaga_battery_init(void)
{
	thread_t thread;

	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	if (gpio == 0) {
		printf("xaga-battery: GPIO block could not be mapped\n");
		return;
	}
	i2c_pins_init();
	printf("xaga-battery: MT6375 on I2C5 (SCL GPIO%d, SDA GPIO%d)\n", I2C_SCL, I2C_SDA);
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("xaga-battery: cannot start the polling thread\n");
	}
}
