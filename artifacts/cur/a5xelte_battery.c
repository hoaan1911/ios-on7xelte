/*
 * A5XELTE / Galaxy J7 Prime (SM-G610F, Exynos7870) battery fuel gauge.
 *
 * The gauge is the S2MU004 block at 0x3B on the i2c@13830000 bus (Samsung's
 * "s2mu004-fuelgauge", sec-fuelgauge), a sibling of the S2MPU03 PMIC that
 * a5xelte_touch.c already talks to.  Read-only, polled by SweetBattery's work
 * loop (iokit/Kernel/arm/SweetBattery.cpp):
 *   RSOC     0x08  1/16384 of full; the DTB capacity_max trims 99 % to 100 %
 *   RVBAT    0x04  mV = raw * 1000 >> 13
 *   RCUR_CC  0x06  signed mA = raw * 1000 >> 12 (bit 15 = charging)
 *   RBATCAP  0x0e  remaining capacity
 * Boot argument j7fg=0 disables the probe and keeps the static fallback.
 */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
int a5xelte_fg_init(void);
int a5xelte_fg_read(unsigned *percent, unsigned *mv, int *ma, unsigned *rem_mah,
    unsigned *full_mah);

#define FG_I2C_PA       0x13830000u     /* S3C2440-layout I2C, as i2c@13830000 */
static uint8_t fg_addr = 0x76u;     /* 7-bit 0x3b; the scan moves it if the gauge is elsewhere */
#define FG_CON          0x00u           /* IICCON */
#define FG_STAT         0x04u           /* IICSTAT */
#define FG_DS           0x0cu           /* IICDS */
#define FG_FILTER       0x10u           /* SDA line delay/filter, as a5xelte_touch.c */
#define FG_REPSOC       0x08u
#define FG_RVBAT        0x04u
#define FG_RCUR         0x06u
#define FG_RBATCAP      0x0eu
#define FG_CAPACITY_MAX 990u            /* DTB fuelgauge,capacity_max */
#define FG_MODE_TX      0xd0u           /* master transmit, START */
#define FG_MODE_RX      0x90u           /* master receive, START */

/*
 * The high-speed I2C blocks (0x13860000 and up) are a different controller from the
 * S3C2440-layout one at 0x13830000-0x13850000, and the PMIC pair - S2MPU05 at 0x66
 * and the S2MU005 fuel gauge at 0x3b - sits on the first of them.  Writing the S3C
 * register layout there read back exactly the value written (stat=0xf0, the byte
 * itself) on every bus, which is why the gauge scan never found anything.  The
 * register map and the start sequence are the ones in Samsung's exynos5-hsi2c:
 * direction and master in HSI2C_CTL, one byte per HSI2C_TX_FIFO word, and writing
 * the address byte to the FIFO is what starts the transfer.
 */
#define HSI2C_CTL       0x00u
#define HSI2C_FIFO_CTL  0x04u
#define HSI2C_TRAILING  0x08u
#define HSI2C_INT_EN    0x10u
#define HSI2C_INT_STAT  0x14u
#define HSI2C_TX_FIFO   0x1cu
#define HSI2C_RX_FIFO   0x20u

#define HSI2C_CTL_FUNC_I2C  (1u << 0)
#define HSI2C_CTL_MASTER    (1u << 3)
#define HSI2C_CTL_RXCHON    (1u << 6)
#define HSI2C_CTL_TXCHON    (1u << 7)
#define HSI2C_CTL_SW_RST    (1u << 31)

#define HSI2C_INT_RX_READY  (1u << 0)
#define HSI2C_INT_TX_READY  (1u << 1)
#define HSI2C_INT_RX_NAK    (1u << 2)
#define HSI2C_INT_TX_NAK    (1u << 3)
#define HSI2C_INT_ARB_LOST  (1u << 4)
#define HSI2C_INT_STOP      (1u << 5)
#define HSI2C_INT_ADDR_NAK  (1u << 6)
#define HSI2C_INT_ADDR_ACK  (1u << 7)

#define HSI2C_FIFO_RX_RDY   (1u << 0)
#define HSI2C_FIFO_TX_RDY   (1u << 4)

static int fg_hsi2c;

static vm_offset_t fg_i2c;
static unsigned fg_errors;

static uint32_t
fg_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(fg_i2c + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
fg_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(fg_i2c + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

/* IICCON bit 4 is the transfer-complete flag (write 0 to clear); IICSTAT bit 5 is
 * bus busy and bit 0 is the ACK of the byte just transferred. */
static bool
fg_wait(void)
{
	for (unsigned n = 0; n < 20000; n++) {
		if (fg_rd(FG_CON) & 0x10u) {
			return true;
		}
		delay(1);
	}
	return false;
}

static void
fg_clear(void)
{
	fg_wr(FG_CON, fg_rd(FG_CON) & ~0x10u);
}

static void
fg_stop(void)
{
	fg_wr(FG_STAT, fg_rd(FG_STAT) & ~0x20u);
	fg_clear();
	for (unsigned n = 0; n < 5000; n++) {
		if (!(fg_rd(FG_STAT) & 0x20u)) {
			break;
		}
		delay(1);
	}
}

/* One START..STOP message; bytes[0] is the register pointer for a write. */
static bool
fg_msg(bool receive, uint8_t *bytes, unsigned count)
{
	bool ok = false;

	if (!count || count > 4) {
		return false;
	}
	for (unsigned n = 0; n < 5000; n++) {
		if (!(fg_rd(FG_STAT) & 0x20u)) {
			break;
		}
		if (n == 4999) {
			return false;
		}
		delay(1);
	}
	/* The order and the values are a5xelte_touch.c's, which are the ones that answer on this
	 * SoC: control and filter, address into IICDS, the mode byte into IICSTAT, then the same
	 * byte with ENABLE. Written address-then-mode with a STOP in between, IICSTAT read back
	 * exactly the 0xf0 just written, which is a transfer that never started rather than one
	 * that failed. */
	fg_wr(FG_CON, 0xe3u);
	fg_wr(FG_FILTER, 7u);
	fg_wr(FG_STAT, receive ? FG_MODE_RX : FG_MODE_TX);
	fg_wr(FG_DS, (uint32_t)fg_addr | (receive ? 1u : 0u));
	delay(1);
	fg_wr(FG_STAT, (receive ? FG_MODE_RX : FG_MODE_TX) | 0x20u);
	if (!fg_wait() || (fg_rd(FG_STAT) & 1u)) {
		goto out;
	}
	for (unsigned n = 0; n < count; n++) {
		if (receive) {
			if (n + 1 == count) {
				fg_wr(FG_CON, fg_rd(FG_CON) & ~0x80u); /* no ACK on the last byte */
			}
		} else {
			fg_wr(FG_DS, bytes[n]);
			delay(1);
		}
		fg_clear();
		if (!fg_wait()) {
			goto out;
		}
		if (receive) {
			bytes[n] = (uint8_t)fg_rd(FG_DS);
		} else if (fg_rd(FG_STAT) & 1u) {
			goto out;
		}
	}
	ok = true;
out:
	fg_stop();
	if (!ok && ++fg_errors <= 8) {
		printf("A5XELTE FG: %s of %u byte(s) failed stat=%#x con=%#x\n",
		    receive ? "read" : "write", count, fg_rd(FG_STAT), fg_rd(FG_CON));
	}
	return ok;
}

/* Waits for one of `mask` in HSI2C_INT_STAT, bounded; leaves the status in *st. */
static bool
fg_hsi2c_wait(uint32_t mask, uint32_t *st)
{
	for (unsigned n = 0; n < 2000; n++) {
		*st = fg_rd(HSI2C_INT_STAT);
		if (*st & mask) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* One message on the high-speed controller; bytes[0] is the register pointer. */
static bool
fg_hsi2c_msg(bool receive, uint8_t *bytes, unsigned count)
{
	uint32_t ctl = HSI2C_CTL_FUNC_I2C | HSI2C_CTL_MASTER;
	uint32_t st = 0;
	bool ok = false;

	if (!count || count > 4) {
		return false;
	}
	fg_wr(HSI2C_CTL, ctl | HSI2C_CTL_SW_RST);
	delay(1);
	fg_wr(HSI2C_CTL, ctl | (receive ? HSI2C_CTL_RXCHON : HSI2C_CTL_TXCHON));
	fg_wr(HSI2C_FIFO_CTL, HSI2C_FIFO_RX_RDY | HSI2C_FIFO_TX_RDY);
	fg_wr(HSI2C_TRAILING, receive ? (count - 1) : 0);
	fg_wr(HSI2C_INT_STAT, 0xffffffffu);
	fg_wr(HSI2C_INT_EN, HSI2C_INT_ADDR_ACK | HSI2C_INT_ADDR_NAK | HSI2C_INT_TX_NAK |
	    HSI2C_INT_RX_NAK | (receive ? HSI2C_INT_RX_READY : HSI2C_INT_TX_READY));
	/* The address byte is what starts the transfer. */
	fg_wr(HSI2C_TX_FIFO, (uint32_t)((fg_addr & 0xfeu) | (receive ? 1u : 0u)));
	if (!fg_hsi2c_wait(HSI2C_INT_ADDR_ACK | HSI2C_INT_ADDR_NAK, &st) ||
	    (st & HSI2C_INT_ADDR_NAK)) {
		goto out;
	}
	if (receive) {
		for (unsigned n = 0; n < count; n++) {
			if (!fg_hsi2c_wait(HSI2C_INT_RX_READY, &st)) {
				goto out;
			}
			bytes[n] = (uint8_t)fg_rd(HSI2C_RX_FIFO);
		}
	} else {
		for (unsigned n = 0; n < count; n++) {
			if (!fg_hsi2c_wait(HSI2C_INT_TX_READY, &st) ||
			    (st & HSI2C_INT_TX_NAK)) {
				goto out;
			}
			fg_wr(HSI2C_TX_FIFO, bytes[n]);
		}
	}
	ok = true;
out:
	fg_wr(HSI2C_INT_EN, 0);
	fg_wr(HSI2C_CTL, ctl);
	if (!ok && ++fg_errors <= 4) {
		printf("A5XELTE FG: hsi2c %s of %u byte(s) failed stat=%#x ctl=%#x\n",
		    receive ? "read" : "write", count, st, fg_rd(HSI2C_CTL));
	}
	return ok;
}

static bool
fg_hsi2c_word(uint8_t reg, uint16_t *value)
{
	uint8_t in[2];

	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (fg_hsi2c_msg(false, &reg, 1) && fg_hsi2c_msg(true, in, 2)) {
			*value = (uint16_t)(in[0] | ((unsigned)in[1] << 8));
			return true;
		}
		delay(1000);
	}
	return false;
}

static bool fg_word(uint8_t reg, uint16_t *value);

/* Whichever controller answered last: S3C2440 layout or high-speed. */
static bool
fg_word_any(uint8_t reg, uint16_t *value)
{
	return fg_hsi2c ? fg_hsi2c_word(reg, value) : fg_word(reg, value);
}

/* Register pointer, STOP, then read the two bytes: i2c_smbus_read_word_data. */
static bool
fg_word(uint8_t reg, uint16_t *value)
{
	uint8_t in[2];

	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (fg_msg(false, &reg, 1) && fg_msg(true, in, 2)) {
			*value = (uint16_t)(in[0] | ((unsigned)in[1] << 8));
			return true;
		}
		delay(1000);
	}
	return false;
}

/* CMU_PERI at 0x101f0000. CLK_CON_GAT_PERI_BUSP1_PERIC0_HCLK is 0x0810 bit 3 and
 * CLK_CON_GAT_PERI_I2C0_PCLK is 0x0810 bit 21 (the same register carries I2C1..I2C8
 * at bits 23, 22, 20, 17..19, 24, 25). The touch buses answer because their gates
 * are already open; on an unclocked block every transfer reads back exactly the
 * IICSTAT value that was written, which is what all five probes logged. */
#define FG_CMU_PERI     0x101f0000u
#define FG_PERI_GATES   0x0810u
#define FG_GATE_PERIC0   (1u << 3)
#define FG_GATE_I2C0     (1u << 21)
#define FG_GATE_I2C1     (1u << 23)
#define FG_GATE_I2C2     (1u << 22)

static void
fg_clock_on(void)
{
	vm_offset_t cmu = ml_io_map(FG_CMU_PERI, 0x1000);

	if (!cmu) {
		printf("A5XELTE FG: no CMU_PERI map\n");
		return;
	}
	uint32_t before = *(volatile uint32_t *)(cmu + FG_PERI_GATES);
	__asm__ volatile ("dmb oshld" ::: "memory");
	uint32_t want = before | FG_GATE_PERIC0 | FG_GATE_I2C0 | FG_GATE_I2C1 | FG_GATE_I2C2;
	*(volatile uint32_t *)(cmu + FG_PERI_GATES) = want;
	__asm__ volatile ("dsb osh" ::: "memory");
	delay(100);
	uint32_t after = *(volatile uint32_t *)(cmu + FG_PERI_GATES);
	__asm__ volatile ("dmb oshld" ::: "memory");
	printf("A5XELTE FG: CMU_PERI gates 0x%08x -> 0x%08x -> 0x%08x\n", before, want, after);
}

int
a5xelte_fg_init(void)
{
	int enabled = 1;

	(void)PE_parse_boot_argn("j7fg", &enabled, sizeof(enabled));
	if (!enabled) {
		return -1;
	}
	fg_clock_on();
	/* Samsung's tree puts the gauge on I2C4 at 0x3b, and this phone answers on none of the
	 * six controllers at that address.  The charger/FG part is an S2MU005 block whose sibling
	 * PMIC answers as 0x66, so ask the three addresses it can have, on the bus Samsung
	 * documents first and then on the rest.  The first pair that ACKs is kept. */
	static const struct { uint32_t pa; int hsi2c; } buses[] = {
		{ 0x13830000u, 0 }, { 0x13840000u, 0 }, { 0x13850000u, 0 },
		{ 0x13860000u, 1 }, { 0x13870000u, 1 }, { 0x13880000u, 1 },
		{ 0x13890000u, 1 }, { 0x138a0000u, 1 }, { 0x138b0000u, 1 },
		{ 0x138c0000u, 1 },
	};
	static const uint8_t addrs[] = { 0x76u, 0x66u, 0x1bu };

	for (unsigned i = 0; i < sizeof(buses) / sizeof(buses[0]); i++) {
		fg_i2c = ml_io_map(buses[i].pa, 0x1000);
		if (!fg_i2c) {
			continue;
		}
		fg_hsi2c = buses[i].hsi2c;
		for (unsigned a = 0; a < sizeof(addrs) / sizeof(addrs[0]); a++) {
			uint16_t soc = 0xffff;
			fg_addr = addrs[a];
			fg_errors = 1000000u;   /* the scan logs itself, one line per bus */
			if (fg_word_any(FG_REPSOC, &soc)) {
				fg_errors = 0;
				printf("A5XELTE FG: gauge on 0x%08x (%s) at 0x%02x, RSOC=%#x\n",
				    buses[i].pa, fg_hsi2c ? "hsi2c" : "i2c", fg_addr, soc);
				return 0;
			}
		}
		if (fg_hsi2c) {
			printf("A5XELTE FG: nothing on 0x%08x (hsi2c), istat=%#x ctl=%#x\n",
			    buses[i].pa, fg_rd(HSI2C_INT_STAT), fg_rd(HSI2C_CTL));
		} else {
			printf("A5XELTE FG: nothing on 0x%08x, stat=%#x con=%#x\n",
			    buses[i].pa, fg_rd(FG_STAT), fg_rd(FG_CON));
		}
	}
	printf("A5XELTE FG: no gauge answered on any I2C bus\n");
	return -1;
}

int
a5xelte_fg_read(unsigned *percent, unsigned *mv, int *ma, unsigned *rem_mah,
    unsigned *full_mah)
{
	uint16_t soc, vbat, cur, cap;
	unsigned raw, p;

	if (!fg_i2c || !fg_word_any(FG_REPSOC, &soc) || !fg_word_any(FG_RVBAT, &vbat) ||
	    !fg_word_any(FG_RCUR, &cur) || !fg_word_any(FG_RBATCAP, &cap)) {
		return -1;
	}
	/* Two layouts are in the vendor tree for this register: hundredths of a percent
	 * (0..10000, full = capacity_max 99.0 %) and 1/16384 of full.  Take the one the value
	 * fits instead of assuming, which is why the percentage never matched the phone. */
	raw = soc & 0x7fffu;
	if (raw <= 10000u) {
		p = raw * 1000u / FG_CAPACITY_MAX;
	} else {
		p = raw * 1000u / 16384u;
	}
	*percent = p >= 1000u ? 100u : p / 10u;
	*mv = (unsigned)(((uint64_t)(vbat & 0x7fffu) * 1000u) >> 13);
	*ma = (cur & 0x8000u)
	    ? -(int)(((((unsigned)~cur & 0xffffu) + 1u) * 1000u) >> 12)
	    :  (int)((((unsigned)cur & 0x7fffu) * 1000u) >> 12);
	/* RBATCAP is in the gauge's own units; left at zero rather than invented. */
	*rem_mah = 0;
	*full_mah = 0;
	(void)cap;
	return 0;
}
#endif
