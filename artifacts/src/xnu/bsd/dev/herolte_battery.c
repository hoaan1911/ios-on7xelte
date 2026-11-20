/*
 * HEROLTE (Galaxy S7 SM-G930F, Exynos 8890) fuel gauge: the MAX77854 FG
 * (BOOT DTB /battery fuelgauge_name "max77854-fuelgauge"), 0x36 next to
 * the MFD at 0x66 on HSI2C_1 0x13650000, pins GPD1-2/3 function 2
 * (hs-i2c1-bus). HSI2C_0's sec-fuelgauge@36 is not on this board: no ACK
 * there on the phone. Read-only:
 * REPSOC 0x06 (1/256 %), REPCAP 0x05 and FULLCAP 0x10 (0.5 mAh with the
 * 10 mOhm sense resistor), VCELL 0x09 (78.125 uV), CURRENT 0x0a (signed,
 * 156.25 uA, positive = charging). Polled by SweetBattery's work loop.
 */
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
int herolte_fg_init(void);
int herolte_fg_read(unsigned *percent, unsigned *mv, int *ma, unsigned *rem_mah, unsigned *full_mah);

#define FG_HSI2C_PA     0x13650000u
#define FG_PINS_PA      0x136d0000u     /* GPI0 +0x00, GPD0 +0x20, GPD1 +0x40 */
/* GPD1 pins 0/1 (HSI2C_0, speaker amp) are left as found. */
#define FG_GPD1         0x40
#define FG_ADDR         0x36u
#define FG_REPCAP       0x05
#define FG_REPSOC       0x06
#define FG_VCELL        0x09
#define FG_CURRENT      0x0a
#define FG_FULLCAP      0x10
/* DTB fuelgauge,capacity_max: Samsung shows 99.0 % of REPSOC as 100 %. */
#define FG_CAPACITY_MAX 990u

#define HSI2C_CTL       0x00
#define HSI2C_FIFO_CTL  0x04
#define HSI2C_TRAILING  0x08
#define HSI2C_INT_EN    0x20
#define HSI2C_INT_STAT  0x24
#define HSI2C_FIFO_STAT 0x30
#define HSI2C_TX_DATA   0x34
#define HSI2C_RX_DATA   0x38
#define HSI2C_CONF      0x40
#define HSI2C_AUTO_CONF 0x44
#define HSI2C_TIMEOUT   0x48
#define HSI2C_TRANS     0x50
#define HSI2C_FS1       0x60
#define HSI2C_FS2       0x64
#define HSI2C_FS3       0x68
#define HSI2C_SLA       0x6c
#define HSI2C_ADDR      0x70

static vm_offset_t fg_i2c, fg_pins;

static uint32_t
fg_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
fg_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

/* Same AUTO-mode setup as the touch bus (herolte_touch.c ht_bus_init). */
static void
fg_bus_init(void)
{
	fg_wr(fg_i2c, HSI2C_CTL, 0x80000009u);
	fg_wr(fg_i2c, HSI2C_CTL, 0x00000009u);
	fg_wr(fg_i2c, HSI2C_TIMEOUT, 0);
	fg_wr(fg_i2c, HSI2C_FS1, 0x78787800u);
	fg_wr(fg_i2c, HSI2C_FS2, 0x3c007878u);
	fg_wr(fg_i2c, HSI2C_FS3, 0x000100f0u);
	fg_wr(fg_i2c, HSI2C_SLA, 0x3cu);
	fg_wr(fg_i2c, HSI2C_TRAILING, 0xfu);
	fg_wr(fg_i2c, HSI2C_CONF, 0x80000000u);
	fg_wr(fg_i2c, HSI2C_INT_EN, 0);
}

/*
 * One message; without stop the next one starts with a repeated START, as
 * Linux i2c-exynos5.c does for every message but the last.
 */
static bool
fg_message(bool receive, uint8_t *bytes, unsigned count, bool stop)
{
	uint32_t last = 0;
	bool xfer_done = false;
	unsigned done = 0;

	fg_wr(fg_i2c, HSI2C_INT_STAT, fg_rd(fg_i2c, HSI2C_INT_STAT));
	fg_wr(fg_i2c, HSI2C_FIFO_CTL, 3u | (receive ? (1u << 4) : (1u << 16)));
	fg_wr(fg_i2c, HSI2C_ADDR, FG_ADDR << 10);
	fg_wr(fg_i2c, HSI2C_CTL, 9u | (receive ? 0x40u : 0x80u));
	fg_wr(fg_i2c, HSI2C_INT_EN, 0xf80u | (receive ? 0x42u : 1u));
	/* Bit 16 read, bit 17 STOP_AFTER_TRANS, bit 31 MASTER_RUN. */
	fg_wr(fg_i2c, HSI2C_AUTO_CONF, count | (receive ? 0x10000u : 0) |
	    (stop ? 0x20000u : 0) | 0x80000000u);
	for (unsigned us = 0; us < 20000; us++) {
		uint32_t irq = fg_rd(fg_i2c, HSI2C_INT_STAT);
		if (irq) {
			last |= irq;
			fg_wr(fg_i2c, HSI2C_INT_STAT, irq);
		}
		if (irq & 0xf00u) {
			break;
		}
		if (irq & 0x80u) {
			xfer_done = true;
		}
		if (receive) {
			while (done < count && ((fg_rd(fg_i2c, HSI2C_FIFO_STAT) >> 16) & 0x7fu)) {
				bytes[done++] = (uint8_t)fg_rd(fg_i2c, HSI2C_RX_DATA);
			}
		} else {
			while (done < count && !(fg_rd(fg_i2c, HSI2C_FIFO_STAT) & (1u << 7))) {
				fg_wr(fg_i2c, HSI2C_TX_DATA, bytes[done++]);
			}
		}
		if (done == count && (xfer_done || (receive && stop))) {
			fg_wr(fg_i2c, HSI2C_INT_EN, 0);
			if (!stop) {
				return true;
			}
			for (unsigned n = 0; n < 10000; n++) {
				if (!(fg_rd(fg_i2c, HSI2C_TRANS) & (1u << 17))) {
					return true;
				}
				delay(1);
			}
			break;
		}
		delay(1);
	}
	static unsigned errors_logged;
	if (errors_logged++ < 8) {
		printf("HEROLTE FG: %s %u/%u failed int=%#x trans=%#x fifo=%#x\n",
		    receive ? "rx" : "tx", done, count, last,
		    fg_rd(fg_i2c, HSI2C_TRANS), fg_rd(fg_i2c, HSI2C_FIFO_STAT));
	}
	fg_wr(fg_i2c, HSI2C_INT_EN, 0);
	fg_bus_init();
	return false;
}

static bool
fg_word(uint8_t reg, uint16_t *value)
{
	uint8_t b[2];
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (fg_message(false, &reg, 1, false) && fg_message(true, b, 2, true)) {
			*value = (uint16_t)(b[0] | (b[1] << 8));
			return true;
		}
		delay(500);
	}
	return false;
}

int
herolte_fg_init(void)
{
	int enabled = 1;
	PE_parse_boot_argn("herofg", &enabled, sizeof(enabled));
	if (!enabled) {
		return -1;
	}
	fg_i2c = ml_io_map(FG_HSI2C_PA, 0x1000);
	fg_pins = ml_io_map(FG_PINS_PA, 0x1000);
	if (!fg_i2c || !fg_pins) {
		return -1;
	}
	uint32_t con = fg_rd(fg_pins, FG_GPD1);
	printf("HEROLTE FG: GPD1 con=%#x dat=%#x pud=%#x; HSI2C ctl=%#x conf=%#x (as found)\n",
	    con, fg_rd(fg_pins, FG_GPD1 + 4), fg_rd(fg_pins, FG_GPD1 + 8),
	    fg_rd(fg_i2c, HSI2C_CTL), fg_rd(fg_i2c, HSI2C_CONF));
	/* GPD1-2/3 to HSI2C (function 2), no pulls, as the DTB hs-i2c1-bus.
	 * The phone left them as outputs (con 0x1100). */
	fg_wr(fg_pins, FG_GPD1 + 8, fg_rd(fg_pins, FG_GPD1 + 8) & ~0xf0u);
	fg_wr(fg_pins, FG_GPD1, (con & ~0xff00u) | 0x2200u);
	fg_bus_init();
	uint16_t soc;
	if (!fg_word(FG_REPSOC, &soc)) {
		printf("HEROLTE FG: no answer at 0x36\n");
		return -1;
	}
	printf("HEROLTE FG: MAX77854 answers, REPSOC=%#x\n", soc);
	return 0;
}

int
herolte_fg_read(unsigned *percent, unsigned *mv, int *ma, unsigned *rem_mah, unsigned *full_mah)
{
	uint16_t soc, vcell, current, rem, full;
	if (!fg_i2c || !fg_word(FG_REPSOC, &soc) || !fg_word(FG_VCELL, &vcell) ||
	    !fg_word(FG_CURRENT, &current) || !fg_word(FG_REPCAP, &rem) ||
	    !fg_word(FG_FULLCAP, &full)) {
		return -1;
	}
	unsigned permille = ((unsigned)soc * 10u) >> 8;
	unsigned p = (permille * 1000u / FG_CAPACITY_MAX + 5u) / 10u;
	*percent = p > 100 ? 100 : p;
	*mv = (unsigned)(((uint64_t)vcell * 78125u) / 1000000u);
	*ma = (int)(((int64_t)(int16_t)current * 15625) / 100000);
	*rem_mah = rem / 2u;
	*full_mah = full / 2u;
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_HEROLTE */
