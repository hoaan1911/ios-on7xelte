/*
 * HEROLTE (Galaxy S7 SM-G930F, Exynos 8890) touchscreen: Samsung sec_ts at
 * 0x48 on HSI2C 0x14E10000 (pins GPF1-0/1, function 2), INT on GPA1-0
 * (active low), from the BOOT DTB. Protocol from Linux sec_ts.c: wait for
 * the boot-complete ACK, SENSE_ON (0x40), then while INT is low read 8-byte
 * events with READ_ONE_EVENT (0x71). Polled (no XNU IRQ for the GPIO), in
 * its own thread so boot never waits on the bus.
 *
 * Supplies tsp_io/tsp_avdd are S2MPS16 (0x66) LDO32 (1.85 V) and LDO33
 * (3.3 V), off after S-Boot (SDA/SCL and INT read low). The PMIC sits on
 * HSI2C 0x10550000, shared with the APM firmware: like Linux
 * (i2c-exynos5.c exynos5_i2c_xfer_batcher) every PMIC transfer goes through
 * the controller's batcher, which takes the AP/APM hardware semaphore. The
 * LDOs are switched on only if their programmed voltage is the expected one.
 * Boot-arg herotouch=0 disables everything here.
 */
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);
void herolte_touch_probe(void);

#define HT_ALIVE_PA     0x10580000u     /* GPA1 CON +0x20, DAT +0x24 */
#define HT_PINS_PA      0x14ce0000u     /* GPF1 CON +0, DAT +4, PUD +8 */
#define HT_HSI2C_PA     0x14e10000u
#define HT_ADDR         0x48u

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

#define SEC_TS_READ_DEVICE_ID   0x52
#define SEC_TS_READ_SUB_ID      0x53
#define SEC_TS_READ_BOOT_STATUS 0x55
#define SEC_TS_READ_TS_STATUS   0x70
#define SEC_TS_READ_ONE_EVENT   0x71
#define SEC_TS_CMD_SENSE_ON     0x40
#define SEC_TS_CMD_CLEAR_EVENTS 0x60

#define HT_FINGERS      10

/* PMIC bus: HSI2C 0x10550000 batcher (offsets from i2c-exynos5.c). */
#define PM_HSI2C_PA     0x10550000u
#define PM_ADDR         0x66u
#define PM_SMRELEASE    0x200
#define PM_BAT_CON      0x500
#define PM_BAT_STATE    0x504
#define PM_BAT_INT_EN   0x508
#define PM_BAT_INT_STAT 0x510
#define PM_BAT_OPCODE   0x600
#define PM_PAYLOAD      0x1000
#define PM_PAYLOAD_END  0x1060
#define S2MPS16_L32CTRL 0x5c    /* tsp_io: 0.7 V + 25 mV * sel */
#define S2MPS16_L33CTRL 0x5d    /* tsp_avdd: 1.8 V + 25 mV * sel */
#define S2MPS16_SEL_1V85 0x2e
#define S2MPS16_SEL_3V3 0x3c
#define S2MPS16_ENABLE  0xc0u

static vm_offset_t ht_alive, ht_pins, ht_i2c, ht_pm;
static uint32_t pm_cmd_buffer;
static unsigned pm_cmd_index, pm_cmd_pointer, pm_desc;
static int ht_sleep;
static bool ht_down[HT_FINGERS];
static int ht_x[HT_FINGERS], ht_y[HT_FINGERS];

static uint32_t
ht_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
ht_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static void
ht_msleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ht_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/*
 * Wait for the persistent log (flushed every 10 s, herolte_ufs.c) before a
 * risky step, so a hang still leaves the step in the log.
 */
static void
ht_checkpoint(void)
{
	static int slow = -1;
	if (slow < 0) {
		slow = 0;
		PE_parse_boot_argn("herotouchslow", &slow, sizeof(slow));
	}
	/* The APM path works (0216); keep the pauses only for bring-up. */
	if (slow) {
		ht_msleep(11000);
	}
}

/*
 * Read-only APM state (Linux apm-exynos8890.c / mailbox-exynos8.c): PMU
 * 0x105c0000 + 0x2500 CONFIGURATION (bit 0 run), + 0x2504 STATUS (bit 28
 * STANDBYWFI), + 0x2508 OPTION (bit 15 ENABLE_APM); mailbox 0x11240000
 * (TX_MSG 0x00.., TX_IRQ0 0x40, RX_MSG 0x80.., RX_IRQ0 0xc0); APM SRAM
 * 0x11200000, where Linux copies the firmware before starting the M3.
 */
static void
ht_apm_probe(void)
{
	vm_offset_t pmu = ml_io_map(0x105c0000, 0x4000);
	if (!pmu) {
		printf("HEROLTE APM: PMU mapping failed\n");
		return;
	}
	printf("HEROLTE APM: PMU config=%#x status=%#x option=%#x (run=bit0, wfi=bit28, enable=bit15)\n",
	    ht_rd(pmu, 0x2500), ht_rd(pmu, 0x2504), ht_rd(pmu, 0x2508));
	printf("HEROLTE APM: reading the mailbox in 11 s\n");
	ht_checkpoint();
	vm_offset_t mbox = ml_io_map(0x11240000, 0x1000);
	if (!mbox) {
		printf("HEROLTE APM: mailbox mapping failed\n");
		return;
	}
	printf("HEROLTE APM: mbox tx %#x %#x %#x %#x txirq=%#x rx %#x %#x %#x %#x rxirq=%#x\n",
	    ht_rd(mbox, 0x00), ht_rd(mbox, 0x04), ht_rd(mbox, 0x08), ht_rd(mbox, 0x0c),
	    ht_rd(mbox, 0x40), ht_rd(mbox, 0x80), ht_rd(mbox, 0x84), ht_rd(mbox, 0x88),
	    ht_rd(mbox, 0x8c), ht_rd(mbox, 0xc0));
	printf("HEROLTE APM: reading the APM SRAM in 11 s\n");
	ht_checkpoint();
	vm_offset_t sram = ml_io_map(0x11200000, 0x1000);
	if (!sram) {
		printf("HEROLTE APM: SRAM mapping failed\n");
		return;
	}
	printf("HEROLTE APM: SRAM %08x %08x %08x %08x %08x %08x %08x %08x\n",
	    ht_rd(sram, 0x00), ht_rd(sram, 0x04), ht_rd(sram, 0x08), ht_rd(sram, 0x0c),
	    ht_rd(sram, 0x10), ht_rd(sram, 0x14), ht_rd(sram, 0x18), ht_rd(sram, 0x1c));
	printf("HEROLTE APM: probe done\n");
}

/* Batcher program: register offsets as opcodes, 4 per word; 0xff ends it. */
static void
pm_batch(uint32_t value, uint32_t reg)
{
	if (PM_PAYLOAD + pm_desc * 4 > PM_PAYLOAD_END) {
		return;
	}
	pm_cmd_buffer &= ~(0xffu << (8 * pm_cmd_index));
	pm_cmd_buffer |= reg << (8 * pm_cmd_index);
	ht_wr(ht_pm, PM_BAT_OPCODE + pm_cmd_pointer * 4, pm_cmd_buffer);
	ht_wr(ht_pm, PM_PAYLOAD + pm_desc++ * 4, value);
	if (++pm_cmd_index == 4) {
		pm_cmd_index = 0;
		pm_cmd_pointer++;
		pm_cmd_buffer = 0xffffffffu;
	}
}

static void
pm_dump(const char *what)
{
	printf("HEROLTE PMIC: %s: ctl=%#x conf=%#x auto=%#x int=%#x trans=%#x fifo=%#x "
	    "bat_con=%#x bat_state=%#x bat_int=%#x bat_fifo=%#x\n", what,
	    ht_rd(ht_pm, HSI2C_CTL), ht_rd(ht_pm, HSI2C_CONF), ht_rd(ht_pm, HSI2C_AUTO_CONF),
	    ht_rd(ht_pm, HSI2C_INT_STAT), ht_rd(ht_pm, HSI2C_TRANS), ht_rd(ht_pm, HSI2C_FIFO_STAT),
	    ht_rd(ht_pm, PM_BAT_CON), ht_rd(ht_pm, PM_BAT_STATE), ht_rd(ht_pm, PM_BAT_INT_STAT),
	    ht_rd(ht_pm, 0x50c));
}

/*
 * No direct HSI2C register writes on this bus: Linux's probe reset runs
 * before the APM firmware uses the controller, but S-Boot leaves the APM
 * running, and resetting the controller under it hung the phone (0213).
 * Only batcher programs, which hold the AP/APM semaphore, configure it.
 *
 * Batcher recovery as 0212 (which did not hang): reset the batcher only
 * when stuck in CONFIG..CLEAN_INTR, then release the AP semaphore.
 */
static void
pm_batcher_recover(uint32_t state)
{
	if (state >= (1u << 6) && state <= (1u << 13)) {
		ht_wr(ht_pm, PM_BAT_CON, 1u << 31);
		ht_wr(ht_pm, PM_BAT_CON, 0);
		ht_wr(ht_pm, PM_SMRELEASE, 1u);
	}
	ht_wr(ht_pm, PM_BAT_STATE, ht_rd(ht_pm, PM_BAT_STATE) | 1u);
	ht_wr(ht_pm, PM_BAT_CON, 0);
}

/*
 * One PMIC message through the batcher, as exynos5_i2c_xfer_batcher.
 * ext_read: Linux's read form (10-bit mode + EXT_ADDR); otherwise a plain
 * 7-bit read is tried as the alternative.
 */
static bool
pm_message(bool receive, uint8_t *bytes, unsigned count, bool stop, bool ext_read)
{
	uint32_t read_at;
	uint32_t state = 0;
	bool ext = receive && ext_read;

	pm_cmd_buffer = 0xffffffffu;
	pm_cmd_index = pm_cmd_pointer = pm_desc = 0;
	ht_wr(ht_pm, PM_BAT_CON, 0);                                    /* idle */
	ht_wr(ht_pm, PM_BAT_CON, ht_rd(ht_pm, PM_BAT_CON) | 3u);        /* enable, dedicate */
	/* hs_clock 3 MHz (DTB clock-frequency) and the fixed FS timing. */
	pm_batch((7u << 24) | (7u << 16) | (7u << 8) | 7u, 0x54);
	pm_batch((0xfu << 16) | (3u << 24) | (7u << 8) | 1u, 0x58);
	pm_batch(8u, 0x5c);
	pm_batch(3u, HSI2C_SLA);
	pm_batch((38u << 24) | (38u << 16) | (38u << 8), HSI2C_FS1);
	pm_batch((0xfu << 16) | (19u << 24) | (38u << 8) | 38u, HSI2C_FS2);
	pm_batch((1u << 16) | 76u, HSI2C_FS3);
	pm_batch(0x2ffu, HSI2C_TRAILING);
	pm_batch((ext ? (1u << 30) : 0) | (1u << 29) | (1u << 31) | (3u << 16) | (3u << 13) |
	    (1u << 28) | (1u << 27) | 0xffu, HSI2C_CONF);
	pm_batch(0, HSI2C_TIMEOUT);
	pm_batch(3u | (8u << 16) | (count << 4), HSI2C_FIFO_CTL);
	pm_batch((1u << 3) | (receive ? ((1u << 6) | (ext ? (1u << 30) : 0)) : (1u << 7)),
	    HSI2C_CTL);
	pm_batch(PM_ADDR << 10, HSI2C_ADDR);
	pm_batch((receive ? (1u << 16) : 0) | (stop ? (1u << 17) : 0) | count | (1u << 31),
	    HSI2C_AUTO_CONF);
	ht_wr(ht_pm, PM_BAT_INT_STAT, ht_rd(ht_pm, PM_BAT_INT_STAT));
	pm_batch(receive ? (1u << 1) : ((1u << 0) | (1u << 7)), HSI2C_INT_EN);
	read_at = PM_PAYLOAD + pm_desc * 4;
	for (unsigned n = 0; n < count; n++) {
		pm_batch(receive ? 0x77u : bytes[n], receive ? HSI2C_RX_DATA : HSI2C_TX_DATA);
	}
	pm_batch(0, 0xff);
	/* Start: the batcher waits for the APM semaphore, runs, releases it. */
	ht_wr(ht_pm, PM_BAT_INT_EN, 1u);
	ht_wr(ht_pm, PM_BAT_CON, ht_rd(ht_pm, PM_BAT_CON) | (1u << 4));
	for (unsigned n = 0; n < 2000; n++) {          /* >= 200 ms */
		state = ht_rd(ht_pm, PM_BAT_STATE);
		if (state & 3u) {
			break;
		}
		delay(100);
	}
	ht_wr(ht_pm, PM_BAT_INT_EN, 0);
	if ((state & 3u) != 1u) {
		printf("HEROLTE PMIC: %s%s failed, state=%#x\n", receive ? "rx" : "tx",
		    ext ? " (ext)" : "", state);
		pm_dump("after failure");
		pm_batcher_recover(state);
		return false;
	}
	if (receive) {
		for (unsigned n = 0; n < count; n++) {
			bytes[n] = (uint8_t)ht_rd(ht_pm, read_at + n * 4);
		}
	}
	ht_wr(ht_pm, PM_BAT_STATE, state | 1u);
	ht_wr(ht_pm, PM_BAT_INT_STAT, ht_rd(ht_pm, PM_BAT_INT_STAT));
	ht_wr(ht_pm, PM_BAT_CON, 0);
	return true;
}

/*
 * Read forms: 0 = Linux regmap over the batcher ([reg] without STOP, then
 * the 10-bit/EXT read), 1 = the same with a plain 7-bit read, 2 = [reg]
 * with STOP, then a plain read. The first one that works is kept.
 */
static int pm_read_form;
static const char *const pm_form_name[3] = {"ext", "plain", "stop"};

static bool
pm_read(uint8_t reg, uint8_t *value)
{
	return pm_message(false, &reg, 1, pm_read_form == 2, false) &&
	       pm_message(true, value, 1, true, pm_read_form == 0);
}

static bool
pm_write(uint8_t reg, uint8_t value)
{
	uint8_t data[2] = {reg, value};
	return pm_message(false, data, 2, true, false);
}

/* Switch on tsp_io (LDO32) then tsp_avdd (LDO33), as sec_ts_power(). */
static bool
pm_touch_power_on(void)
{
	uint8_t id = 0, l32 = 0, l33 = 0, c32 = 0, c33 = 0;

	pm_dump("as found");
	/* Try the read forms in order; keep the first that works. */
	bool read_ok = false;
	for (pm_read_form = 0; pm_read_form < 3; pm_read_form++) {
		printf("HEROLTE PMIC: trying read form %s in 11 s\n", pm_form_name[pm_read_form]);
		ht_checkpoint();
		read_ok = pm_read(0x00, &id);
		printf("HEROLTE PMIC: read form %s: %s id=%#x\n", pm_form_name[pm_read_form],
		    read_ok ? "ok" : "failed", id);
		if (read_ok) {
			break;
		}
	}
	if (!read_ok || !pm_read(S2MPS16_L32CTRL, &l32) || !pm_read(S2MPS16_L33CTRL, &l33)) {
		printf("HEROLTE PMIC: S2MPS16 read failed\n");
		return false;
	}
	printf("HEROLTE PMIC: S2MPS16 id=%#x L32CTRL=%#x L33CTRL=%#x\n", id, l32, l33);
	if ((l32 & 0x3fu) != S2MPS16_SEL_1V85 || (l33 & 0x3fu) != S2MPS16_SEL_3V3) {
		printf("HEROLTE PMIC: unexpected LDO32/33 voltage, leaving them off\n");
		return false;
	}
	printf("HEROLTE PMIC: enabling LDO32/LDO33 in 11 s\n");
	ht_checkpoint();
	if (!(l32 & S2MPS16_ENABLE) && !pm_write(S2MPS16_L32CTRL, l32 | S2MPS16_ENABLE)) {
		printf("HEROLTE PMIC: LDO32 enable failed\n");
		return false;
	}
	delay(1000);
	if (!(l33 & S2MPS16_ENABLE) && !pm_write(S2MPS16_L33CTRL, l33 | S2MPS16_ENABLE)) {
		printf("HEROLTE PMIC: LDO33 enable failed\n");
		return false;
	}
	if (!pm_read(S2MPS16_L32CTRL, &c32) || !pm_read(S2MPS16_L33CTRL, &c33)) {
		printf("HEROLTE PMIC: LDO readback failed\n");
		return false;
	}
	printf("HEROLTE PMIC: LDO32=%#x LDO33=%#x (on = bits 7:6 set)\n", c32, c33);
	return (c32 & S2MPS16_ENABLE) && (c33 & S2MPS16_ENABLE);
}

/*
 * PMIC access through the APM, as Linux does whenever the screen is on
 * (mailbox-exynos8.c thread_mailbox_work, apm-exynos8890.c): S-Boot leaves
 * the APM firmware in its SRAM and the Cortex-M3 powered (APM_CONFIGURATION
 * run) but held (APM_OPTION ENABLE_APM clear). exynos8890_apm_reset_release
 * clears the mailbox IRQ words and sets ENABLE_APM; then each PMIC access
 * is one mailbox message: TX_MSG0..3, then TX_IRQ0 = 1; the M3 answers in
 * RX_MSG0..3 and raises RX_IRQ0 bit 0 (polled, then cleared).
 *   write: msg0 = 0 (PMIC section, write), msg1 = reg, msg2 = value;
 *          RX_MSG1 = 0xa on success.
 *   read:  msg0 = 1 << 27 (READ_MODE), msg1 = reg; RX_MSG1 = the value.
 * CL-DVFS is never configured (the DTB margins and period are 0).
 */
#define APM_PMU_PA      0x105c0000u
#define APM_CONFIG      0x2500
#define APM_STATUS      0x2504
#define APM_OPTION      0x2508
#define APM_MBOX_PA     0x11240000u
#define MBOX_TX_MSG(i)  ((i) * 4)
#define MBOX_TX_IRQ0    0x40
#define MBOX_RX_MSG(i)  (0x80 + (i) * 4)
#define MBOX_RX_IRQ0    0xc0
#define APM_NO_ACK      0xeeeeeeeeu
#define APM_GPIO_ERR    0xffffffffu

static vm_offset_t apm_pmu, apm_mbox;

static bool
apm_start(void)
{
	apm_pmu = ml_io_map(APM_PMU_PA, 0x4000);
	apm_mbox = ml_io_map(APM_MBOX_PA, 0x1000);
	if (!apm_pmu || !apm_mbox) {
		printf("HEROLTE APM: mapping failed\n");
		return false;
	}
	uint32_t opt = ht_rd(apm_pmu, APM_OPTION);
	if (!(opt & (1u << 15))) {
		printf("HEROLTE APM: releasing the M3 in 11 s (config=%#x option=%#x)\n",
		    ht_rd(apm_pmu, APM_CONFIG), opt);
		ht_checkpoint();
		/* exynos8890_apm_power_up() then exynos8890_apm_reset_release(). */
		ht_wr(apm_pmu, APM_CONFIG, (ht_rd(apm_pmu, APM_CONFIG) & ~1u) | 1u);
		ht_wr(apm_mbox, MBOX_TX_IRQ0, 0);
		ht_wr(apm_mbox, MBOX_RX_IRQ0, 0);
		ht_wr(apm_pmu, APM_OPTION, ht_rd(apm_pmu, APM_OPTION) | (1u << 15));
		ht_msleep(100);
	}
	printf("HEROLTE APM: running: config=%#x status=%#x option=%#x\n",
	    ht_rd(apm_pmu, APM_CONFIG), ht_rd(apm_pmu, APM_STATUS), ht_rd(apm_pmu, APM_OPTION));
	return true;
}

/* samsung_mbox_send_data() + samsung_mbox_last_tx_done(), polled. */
static bool
apm_message(uint32_t msg0, uint32_t msg1, uint32_t msg2, uint32_t *reply)
{
	unsigned n;
	for (n = 0; n < 10000 && ((ht_rd(apm_mbox, MBOX_RX_IRQ0) & 1u) ||
	    (ht_rd(apm_mbox, MBOX_TX_IRQ0) & 1u)); n++) {
		delay(10);
	}
	if (n == 10000) {
		printf("HEROLTE APM: mailbox busy tx=%#x rx=%#x\n",
		    ht_rd(apm_mbox, MBOX_TX_IRQ0), ht_rd(apm_mbox, MBOX_RX_IRQ0));
		return false;
	}
	ht_wr(apm_mbox, MBOX_TX_MSG(0), msg0);
	ht_wr(apm_mbox, MBOX_TX_MSG(1), msg1);
	ht_wr(apm_mbox, MBOX_TX_MSG(2), msg2);
	ht_wr(apm_mbox, MBOX_TX_MSG(3), 0);
	ht_wr(apm_mbox, MBOX_TX_IRQ0, 1u);
	for (n = 0; n < 2000 && !(ht_rd(apm_mbox, MBOX_RX_IRQ0) & 1u); n++) {
		delay(100);     /* >= 200 ms; Linux allows ~3 ms */
	}
	if (n == 2000) {
		printf("HEROLTE APM: no answer: tx=%#x rxirq=%#x status=%#x\n",
		    ht_rd(apm_mbox, MBOX_TX_IRQ0), ht_rd(apm_mbox, MBOX_RX_IRQ0),
		    ht_rd(apm_pmu, APM_STATUS));
		return false;
	}
	*reply = ht_rd(apm_mbox, MBOX_RX_MSG(1));
	uint32_t r0 = ht_rd(apm_mbox, MBOX_RX_MSG(0));
	ht_wr(apm_mbox, MBOX_RX_IRQ0, ht_rd(apm_mbox, MBOX_RX_IRQ0) & ~0xffu);
	ht_wr(apm_mbox, MBOX_RX_MSG(0), 0);
	static unsigned logged;
	if (logged++ < 16) {
		printf("HEROLTE APM: msg %#x %#x %#x -> rx0=%#x rx1=%#x\n", msg0, msg1, msg2, r0, *reply);
	}
	return *reply != APM_NO_ACK && *reply != APM_GPIO_ERR;
}

static bool
apm_pmic_read(uint8_t reg, uint8_t *value)
{
	uint32_t r = 0;
	if (!apm_message(1u << 27, reg, 0, &r)) {
		return false;
	}
	*value = (uint8_t)r;
	return true;
}

/*
 * Read-only S2MPS16 access for other drivers (herolte_gpu.c), only once
 * the touch thread has started the APM; the mailbox is polled, so callers
 * must not overlap with the touch power-on at boot.
 */
static bool apm_ready;
bool herolte_apm_pmic_read(uint8_t reg, uint8_t *value);
bool
herolte_apm_pmic_read(uint8_t reg, uint8_t *value)
{
	return apm_ready && apm_pmic_read(reg, value);
}

static bool
apm_pmic_write(uint8_t reg, uint8_t value)
{
	uint32_t r = 0;
	return apm_message(0, reg, value, &r) && r == 0xau;
}

/* Switch on tsp_io (LDO32) then tsp_avdd (LDO33) via the APM. */
static bool
apm_touch_power_on(void)
{
	uint8_t id = 0, l32 = 0, l33 = 0, c32 = 0, c33 = 0;

	if (!apm_start()) {
		return false;
	}
	printf("HEROLTE APM: first PMIC read in 11 s\n");
	ht_checkpoint();
	if (!apm_pmic_read(0x00, &id) || !apm_pmic_read(S2MPS16_L32CTRL, &l32) ||
	    !apm_pmic_read(S2MPS16_L33CTRL, &l33)) {
		printf("HEROLTE APM: S2MPS16 read failed\n");
		return false;
	}
	printf("HEROLTE APM: S2MPS16 id=%#x L32CTRL=%#x L33CTRL=%#x\n", id, l32, l33);
	if ((l32 & 0x3fu) != S2MPS16_SEL_1V85 || (l33 & 0x3fu) != S2MPS16_SEL_3V3) {
		printf("HEROLTE APM: unexpected LDO32/33 voltage, leaving them off\n");
		return false;
	}
	printf("HEROLTE APM: enabling LDO32/LDO33 in 11 s\n");
	ht_checkpoint();
	if (!(l32 & S2MPS16_ENABLE) && !apm_pmic_write(S2MPS16_L32CTRL, l32 | S2MPS16_ENABLE)) {
		printf("HEROLTE APM: LDO32 enable failed\n");
		return false;
	}
	delay(1000);
	if (!(l33 & S2MPS16_ENABLE) && !apm_pmic_write(S2MPS16_L33CTRL, l33 | S2MPS16_ENABLE)) {
		printf("HEROLTE APM: LDO33 enable failed\n");
		return false;
	}
	if (!apm_pmic_read(S2MPS16_L32CTRL, &c32) || !apm_pmic_read(S2MPS16_L33CTRL, &c33)) {
		printf("HEROLTE APM: LDO readback failed\n");
		return false;
	}
	printf("HEROLTE APM: LDO32=%#x LDO33=%#x (on = bits 7:6 set)\n", c32, c33);
	apm_ready = true;
	return (c32 & S2MPS16_ENABLE) && (c33 & S2MPS16_ENABLE);
}

/*
 * AUTO mode master, as the A5's Exynos HSI2C PMIC path (a5xelte_touch.c).
 * FS timing tSCL_L = tSCL_H = 120 clocks with CLK_DIV 1: about 200 kHz
 * from a 100 MHz source, never above 400 kHz up to a 200 MHz source.
 */
static void
ht_bus_init(void)
{
	ht_wr(ht_i2c, HSI2C_CTL, 0x80000009u);  /* SW reset, master, I2C mode */
	ht_wr(ht_i2c, HSI2C_CTL, 0x00000009u);
	ht_wr(ht_i2c, HSI2C_TIMEOUT, 0);
	ht_wr(ht_i2c, HSI2C_FS1, 0x78787800u);
	ht_wr(ht_i2c, HSI2C_FS2, 0x3c007878u);
	ht_wr(ht_i2c, HSI2C_FS3, 0x000100f0u);
	ht_wr(ht_i2c, HSI2C_SLA, 0x3cu);
	ht_wr(ht_i2c, HSI2C_TRAILING, 0xfu);
	ht_wr(ht_i2c, HSI2C_CONF, 0x80000000u);  /* AUTO mode */
	ht_wr(ht_i2c, HSI2C_INT_EN, 0);
}

/* One message with STOP; count <= 32. */
static bool
ht_message(bool receive, uint8_t *bytes, unsigned count)
{
	uint32_t last = 0;
	bool xfer_done = false;
	unsigned done = 0;

	if (count == 0 || count > 32) {
		return false;
	}
	ht_wr(ht_i2c, HSI2C_INT_STAT, ht_rd(ht_i2c, HSI2C_INT_STAT));
	ht_wr(ht_i2c, HSI2C_FIFO_CTL, 3u | (receive ? (1u << 4) : (1u << 16)));
	ht_wr(ht_i2c, HSI2C_ADDR, HT_ADDR << 10);
	ht_wr(ht_i2c, HSI2C_CTL, 9u | (receive ? 0x40u : 0x80u));
	ht_wr(ht_i2c, HSI2C_INT_EN, 0xf80u | (receive ? 0x42u : 1u));
	ht_wr(ht_i2c, HSI2C_AUTO_CONF, count | (receive ? 0x10000u : 0) | 0x20000u | 0x80000000u);
	for (unsigned us = 0; us < 50000; us++) {
		uint32_t irq = ht_rd(ht_i2c, HSI2C_INT_STAT);
		if (irq) {
			last |= irq;
			ht_wr(ht_i2c, HSI2C_INT_STAT, irq);
		}
		if (irq & 0xf00u) {
			break;
		}
		if (irq & 0x80u) {
			xfer_done = true;
		}
		if (receive) {
			while (done < count && ((ht_rd(ht_i2c, HSI2C_FIFO_STAT) >> 16) & 0x7fu)) {
				bytes[done++] = (uint8_t)ht_rd(ht_i2c, HSI2C_RX_DATA);
			}
		} else {
			while (done < count && !(ht_rd(ht_i2c, HSI2C_FIFO_STAT) & (1u << 7))) {
				ht_wr(ht_i2c, HSI2C_TX_DATA, bytes[done++]);
			}
		}
		/* Like Linux, a read is complete once every byte arrived. */
		if (done == count && (xfer_done || receive)) {
			ht_wr(ht_i2c, HSI2C_INT_EN, 0);
			for (unsigned n = 0; n < 10000; n++) {
				if (!(ht_rd(ht_i2c, HSI2C_TRANS) & (1u << 17))) {
					return true;
				}
				delay(1);
			}
			break;
		}
		delay(1);
	}
	static unsigned errors_logged;
	if (errors_logged++ < 12) {
		printf("HEROLTE TOUCH: %s %u/%u failed int=%#x trans=%#x fifo=%#x\n",
		    receive ? "rx" : "tx", done, count, last,
		    ht_rd(ht_i2c, HSI2C_TRANS), ht_rd(ht_i2c, HSI2C_FIFO_STAT));
	}
	ht_wr(ht_i2c, HSI2C_INT_EN, 0);
	ht_bus_init();
	return false;
}

static bool
ht_cmd(uint8_t reg)
{
	return ht_message(false, &reg, 1);
}

static unsigned ht_moves, ht_retries, ht_lost;

/*
 * sec_ts sometimes NACKs while it is scanning (int 0x400 on the phone);
 * sec_ts_i2c_read() retries up to 10 times. A lost READ_ONE_EVENT drops
 * finger movement, and iOS then misses the swipe-up gesture.
 */
static bool
ht_read(uint8_t reg, uint8_t *data, unsigned len)
{
	for (unsigned attempt = 0; attempt < 10; attempt++) {
		if (attempt) {
			ht_retries++;
			delay(200);
		}
		if (ht_message(false, &reg, 1) && ht_message(true, data, len)) {
			return true;
		}
	}
	ht_lost++;
	return false;
}

static bool
ht_int_low(void)
{
	return !(ht_rd(ht_alive, 0x24) & 1u);
}

static void
ht_release_all(void)
{
	for (int t = 0; t < HT_FINGERS; t++) {
		if (ht_down[t]) {
			ht_down[t] = false;
			sweet_hid_touch(t, 0, ht_x[t], ht_y[t]);
		}
	}
}

/* Returns false when the event stack is empty. */
static bool
ht_event(unsigned *lines)
{
	uint8_t e[8] = {0};
	if (!ht_read(SEC_TS_READ_ONE_EVENT, e, sizeof(e))) {
		return false;
	}
	unsigned eid = e[0] >> 6;
	if (e[0] == 0) {
		return false;
	}
	if (eid != 1) {
		if (*lines < 64) {
			(*lines)++;
			printf("HEROLTE TOUCH: status %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
		}
		return true;
	}
	unsigned action = e[0] & 7;     /* 1 press, 2 move, 3 release */
	int tid = (int)(e[1] & 0xf) - 1;
	unsigned rx = ((unsigned)e[2] << 4) | (e[4] >> 4);
	unsigned ry = ((unsigned)e[3] << 4) | (e[4] & 0xf);
	if (tid < 0 || tid >= HT_FINGERS) {
		return true;
	}
	/* Firmware coordinates span 0..4095 (DTB sec,max_coords 0xfff). */
	int x = (int)(rx * 1440u / 4096u);
	int y = (int)(ry * 2560u / 4096u);
	if (*lines < 64 && action != 2) {
		(*lines)++;
		printf("HEROLTE TOUCH: id=%d action=%u raw=%u,%u -> %d,%d\n", tid, action, rx, ry, x, y);
	}
	if (action == 3) {
		if (ht_down[tid]) {
			ht_down[tid] = false;
			sweet_hid_touch(tid, 0, ht_x[tid], ht_y[tid]);
		}
	} else if (action == 1 || action == 2) {
		if (action == 2) {
			ht_moves++;
		}
		ht_down[tid] = true;
		ht_x[tid] = x;
		ht_y[tid] = y;
		sweet_hid_touch(tid, 1, x, y);
	}
	return true;
}

static void
ht_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint8_t b[8] = {0};
	unsigned lines = 0;

	/* Let the root mount and the persistent log start first. */
	ht_msleep(3000);
	printf("HEROLTE TOUCH: GPF1 con=%#x pud=%#x dat=%#x; GPA1 con=%#x dat=%#x\n",
	    ht_rd(ht_pins, 0), ht_rd(ht_pins, 8), ht_rd(ht_pins, 4),
	    ht_rd(ht_alive, 0x20), ht_rd(ht_alive, 0x24));
	printf("HEROLTE TOUCH: HSI2C ctl=%#x conf=%#x fs1=%#x fs2=%#x fs3=%#x trans=%#x (as found)\n",
	    ht_rd(ht_i2c, HSI2C_CTL), ht_rd(ht_i2c, HSI2C_CONF), ht_rd(ht_i2c, HSI2C_FS1),
	    ht_rd(ht_i2c, HSI2C_FS2), ht_rd(ht_i2c, HSI2C_FS3), ht_rd(ht_i2c, HSI2C_TRANS));
	/*
	 * 0214 froze the phone on its first batcher program at ~56 s (0212's
	 * at ~1 s did not). 0215 read the APM state: config=0x1 (powered),
	 * option=0 (held), firmware vectors in its SRAM, mailbox idle.
	 */
	int mode = 1;
	PE_parse_boot_argn("herotouch", &mode, sizeof(mode));
	ht_apm_probe();
	/* 1 (default): PMIC through the APM mailbox; 2: the HSI2C batcher. */
	bool powered = mode == 2 ? pm_touch_power_on() : apm_touch_power_on();
	if (!powered) {
		printf("HEROLTE TOUCH: no supply; stopping\n");
		return;
	}
	/* sec_ts_power() 5 ms, then probe waits 60 ms for the IC to boot. */
	ht_msleep(70);
	printf("HEROLTE TOUCH: powered: GPF1 dat=%#x (SDA/SCL should be high) GPA1 dat=%#x\n",
	    ht_rd(ht_pins, 4), ht_rd(ht_alive, 0x24));
	/* GPF1-0/1 to HSI2C (function 2), no pulls, as the DTB hs-i2c7-bus. */
	ht_wr(ht_pins, 8, ht_rd(ht_pins, 8) & ~0xfu);
	ht_wr(ht_pins, 0, (ht_rd(ht_pins, 0) & ~0xffu) | 0x22u);
	ht_bus_init();

	bool ok = ht_read(SEC_TS_READ_BOOT_STATUS, b, 1);
	printf("HEROLTE TOUCH: boot status ok=%d %#x (0x20 = app mode)\n", ok, b[0]);
	if (!ok) {
		printf("HEROLTE TOUCH: no ACK at 0x48: supplies off or bus not clocked; stopping\n");
		return;
	}
	if (ht_read(SEC_TS_READ_DEVICE_ID, b, 5)) {
		printf("HEROLTE TOUCH: device id %02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3], b[4]);
	}
	if (ht_read(SEC_TS_READ_SUB_ID, b, 5)) {
		printf("HEROLTE TOUCH: sub id %02x %02x %02x tx=%u rx=%u\n", b[0], b[1], b[2], b[3], b[4]);
	}
	if (ht_read(SEC_TS_READ_TS_STATUS, b, 4)) {
		printf("HEROLTE TOUCH: ts status %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3]);
	}
	/* Drain pending status events (boot complete etc.), then sense on. */
	for (unsigned n = 0; n < 32 && ht_event(&lines); n++) {
	}
	if (!ht_cmd(SEC_TS_CMD_SENSE_ON)) {
		printf("HEROLTE TOUCH: SENSE_ON failed\n");
		return;
	}
	ht_msleep(20);
	printf("HEROLTE TOUCH: sensing, INT=%d; polling every 5 ms\n", ht_int_low());

	unsigned failures = 0, ticks = 0, last_moves = 0, last_retries = 0, last_lost = 0;
	for (;;) {
		/* Every ~10 s with activity: movement events and I2C recoveries. */
		if (++ticks % 2000 == 0 && (ht_moves != last_moves || ht_retries != last_retries ||
		    ht_lost != last_lost)) {
			printf("HEROLTE TOUCH: moves=%u i2c_retries=%u lost_reads=%u\n",
			    ht_moves, ht_retries, ht_lost);
			last_moves = ht_moves;
			last_retries = ht_retries;
			last_lost = ht_lost;
		}
		if (ht_int_low()) {
			unsigned n = 0;
			while (n++ < 32 && ht_event(&lines)) {
			}
			if (ht_int_low() && ++failures >= 200) {
				printf("HEROLTE TOUCH: INT stuck low; clearing the event stack\n");
				ht_release_all();
				(void)ht_cmd(SEC_TS_CMD_CLEAR_EVENTS);
				failures = 0;
			}
		} else {
			failures = 0;
		}
		ht_msleep(5);
	}
}

void
herolte_touch_probe(void)
{
	int enabled = 1;
	thread_t thread;

	PE_parse_boot_argn("herotouch", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("HEROLTE TOUCH: disabled\n");
		return;
	}
	ht_alive = ml_io_map(HT_ALIVE_PA, 0x1000);
	ht_pins = ml_io_map(HT_PINS_PA, 0x1000);
	ht_i2c = ml_io_map(HT_HSI2C_PA, 0x1000);
	ht_pm = ml_io_map(PM_HSI2C_PA, 0x2000);
	if (!ht_alive || !ht_pins || !ht_i2c || !ht_pm) {
		printf("HEROLTE TOUCH: MMIO mapping failed\n");
		return;
	}
	if (kernel_thread_start(ht_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_HEROLTE */
