/*
 * Early Exynos7580 ZT7538 bring-up probe.  The working native Linux
 * port uses I2C2 at 0x13850000, GPC3-0/1, GPA1-1 and S2MPU03 LDO33.
 * Opt-in touch logging enables only the proven S2MPU03 LDO33 supply and
 * observes ZT7538 contacts before userland. It never programs touch NVM.
 */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void thread_terminate_self(void);
extern void sweet_hid_touch(int finger, int down, int x, int y);
void a5xelte_touch_probe(void);
static bool a5touch_controller_init(void);
static bool a5touch_report_packet(const uint8_t packet[34], unsigned *lines);
static void a5touch_release_contacts(void);

static vm_offset_t a5_touch_bus;
static vm_offset_t a5_touch_alive;
static vm_offset_t a5_touch_hsi;
static int a5_touch_sleep;
static bool a5_touch_active[5];
static uint16_t a5_touch_x[5], a5_touch_y[5];

static uint16_t
a5touch_le16(const uint8_t *bytes)
{
	return (uint16_t)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8));
}

static uint32_t
a5touch_read(vm_offset_t base, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(base + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
a5touch_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* Exynos7580 MIF HSI2C3 AUTO mode, matching Linux's 400 kHz timing. */
static void
a5pmic_bus_init(vm_offset_t hsi)
{
	a5touch_write(hsi, 0x00, 0x80000009u);
	a5touch_write(hsi, 0x00, 0x00000009u);
	a5touch_write(hsi, 0x48, 0); /* Linux disables the hardware timeout. */
	a5touch_write(hsi, 0x60, 0x78787800u);
	a5touch_write(hsi, 0x64, 0x3c007878u);
	a5touch_write(hsi, 0x68, 0x000000f0u);
	a5touch_write(hsi, 0x6c, 0x3cu);
	a5touch_write(hsi, 0x08, 0xfu);
	a5touch_write(hsi, 0x40, 0x80000000u);
	a5touch_write(hsi, 0x20, 0); /* Poll without an unregistered XNU IRQ. */
}

static bool
a5pmic_message(vm_offset_t hsi, bool receive, uint8_t *bytes,
    unsigned count, bool stop)
{
	uint32_t ctl;
	uint32_t cfg;
	uint32_t last_irq = 0;
	bool transfer_done = false;
	unsigned done = 0;
	if (!count || count > 4) {
		return false;
	}
	/* Exynos7580 is EXYNOS7: TRANS_STATUS[3:0] is an FSM state,
	 * not the Exynos5 completion/error bitmap. Linux uses INT_STATUS. */
	a5touch_write(hsi, 0x24, a5touch_read(hsi, 0x24));
	a5touch_write(hsi, 0x04, 3u | (count << (receive ? 4 : 16)));
	ctl = 9u | (receive ? 0x40u : 0x80u);
	a5touch_write(hsi, 0x70, 0x66u << 10);
	a5touch_write(hsi, 0x00, ctl);
	cfg = count | (receive ? 0x10000u : 0) | (stop ? 0x20000u : 0);
	/* GIC SPI for this controller is masked in the A5XELTE platform;
	 * enable the controller's status bits and service them by polling. */
	a5touch_write(hsi, 0x20, 0xf80u | (receive ? 0x42u : 1u));
	a5touch_write(hsi, 0x44, cfg | 0x80000000u);
	for (unsigned us = 0; us < 100000; us++) {
		uint32_t fifo = a5touch_read(hsi, 0x30);
		uint32_t irq = a5touch_read(hsi, 0x24);
		if (irq) {
			last_irq |= irq;
			a5touch_write(hsi, 0x24, irq);
		}
		if (irq & 0xf00u) {
			printf("A5XELTE TOUCH: PMIC HSI2C error rx=%d done=%u/%u irq=%#x state=%#x fifo=%#x\n",
			    receive, done, count, last_irq, a5touch_read(hsi, 0x50), fifo);
			goto fail;
		}
		if (irq & 0x80u) {
			transfer_done = true;
		}
		if (receive) {
			while (done < count && ((a5touch_read(hsi, 0x30) >> 16) & 0x7fu)) {
				bytes[done++] = (uint8_t)a5touch_read(hsi, 0x38);
			}
		} else {
			while (done < count && !(a5touch_read(hsi, 0x30) & (1u << 7))) {
				a5touch_write(hsi, 0x34, bytes[done++]);
			}
			if (done == count) {
				a5touch_write(hsi, 0x20, 0xf80u);
			}
		}
		if (done == count && transfer_done) {
			a5touch_write(hsi, 0x20, 0);
			if (stop) {
				for (unsigned n = 0; n < 100000; n++) {
					if (!(a5touch_read(hsi, 0x50) & (1u << 17))) {
						return true;
					}
					delay(1);
				}
				printf("A5XELTE TOUCH: PMIC STOP busy irq=%#x state=%#x\n",
				    last_irq, a5touch_read(hsi, 0x50));
				goto fail;
			}
			return true;
		}
		delay(1);
	}
	printf("A5XELTE TOUCH: PMIC HSI2C timeout rx=%d done=%u/%u irq=%#x state=%#x fifo=%#x\n",
	    receive, done, count, last_irq | a5touch_read(hsi, 0x24),
	    a5touch_read(hsi, 0x50), a5touch_read(hsi, 0x30));
fail:
	a5touch_write(hsi, 0x20, 0);
	a5pmic_bus_init(hsi);
	return false;
}

static bool
a5pmic_read(vm_offset_t hsi, uint8_t reg, uint8_t *value)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		uint8_t address = reg;
		if (a5pmic_message(hsi, false, &address, 1, false) &&
		    a5pmic_message(hsi, true, value, 1, true)) {
			return true;
		}
		delay(1000);
	}
	return false;
}

static bool
a5pmic_write(vm_offset_t hsi, uint8_t reg, uint8_t value)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		uint8_t data[2] = {reg, value};
		if (a5pmic_message(hsi, false, data, 2, true)) {
			return true;
		}
		delay(1000);
	}
	return false;
}

static bool
a5touch_power_on(vm_offset_t mifpins)
{
	vm_offset_t hsi = ml_io_map(0x104c0000, 0x2000);
	uint32_t old_con;
	uint32_t old_pud;
	uint8_t id = 0xff;
	uint8_t ldo = 0xff;
	uint8_t check = 0xff;
	if (!hsi) {
		printf("A5XELTE TOUCH: PMIC HSI2C mapping failed\n");
		return false;
	}
	old_con = a5touch_read(mifpins, 0);
	old_pud = a5touch_read(mifpins, 8);
	a5touch_write(mifpins, 8, (old_pud & ~0xfu) | 0xfu);
	a5touch_write(mifpins, 0, (old_con & ~0xffu) | 0x22u);
	delay(100);
	a5pmic_bus_init(hsi);
	if (!a5pmic_read(hsi, 0x00, &id) ||
	    !a5pmic_read(hsi, 0x4e, &ldo)) {
		printf("A5XELTE TOUCH: PMIC identity/control read failed\n");
		goto fail;
	}
	printf("A5XELTE TOUCH: S2MPU03 ID=%#x LDO33=%#x\n", id, ldo);
	/* Working Linux device read ID 0x02 and LDO33 0x3c before enable. */
	if (id != 0x02 || (ldo & 0x3fu) != 0x3cu) {
		printf("A5XELTE TOUCH: unexpected PMIC or LDO33 voltage, leave off\n");
		goto fail;
	}
	if (!(ldo & 0xc0u)) {
		if (!a5pmic_write(hsi, 0x4e, ldo | 0xc0u)) {
			printf("A5XELTE TOUCH: LDO33 enable failed\n");
			goto fail;
		}
	}
	if (!a5pmic_read(hsi, 0x4e, &check) || check != (ldo | 0xc0u)) {
		printf("A5XELTE TOUCH: LDO33 readback failed value=%#x\n", check);
		goto fail;
	}
	printf("A5XELTE TOUCH: LDO33 enabled at 3.3 V, readback=%#x\n", check);
	a5_touch_hsi = hsi;
	delay(15000);
	return true;
fail:
	a5touch_write(mifpins, 0, old_con);
	a5touch_write(mifpins, 8, old_pud);
	return false;
}

/* S3C2440 I2C2: one bounded message, with STOP between Zinitix phases. */
static bool
a5touch_wait_irq(vm_offset_t i2c)
{
	for (unsigned n = 0; n < 20000; n++) {
		if (a5touch_read(i2c, 0) & 0x10u) {
			return true;
		}
		delay(1);
	}
	return false;
}

static void
a5touch_clear_irq(vm_offset_t i2c)
{
	a5touch_write(i2c, 0, a5touch_read(i2c, 0) & ~0x10u);
}

static void
a5touch_stop(vm_offset_t i2c)
{
	a5touch_write(i2c, 4, a5touch_read(i2c, 4) & ~0x20u);
	a5touch_clear_irq(i2c);
	for (unsigned n = 0; n < 5000; n++) {
		if (!(a5touch_read(i2c, 4) & 0x20u)) {
			break;
		}
		delay(1);
	}
}

static bool
a5touch_message(vm_offset_t i2c, bool receive, uint8_t *bytes, unsigned count)
{
	const uint32_t mode = receive ? 0x90u : 0xd0u;
	bool ok = false;
	if (!count || count > 34) {
		return false;
	}
	for (unsigned n = 0; n < 5000; n++) {
		if (!(a5touch_read(i2c, 4) & 0x20u)) {
			break;
		}
		if (n == 4999) {
			printf("A5XELTE TOUCH: I2C2 busy before message\n");
			return false;
		}
		delay(1);
	}
	/* Conservative divider; the working Linux controller also sets ACK/IRQ. */
	a5touch_write(i2c, 0, 0xe3u);
	a5touch_write(i2c, 0x10, 7u); /* S3C2440 SDA delay/filter */
	a5touch_write(i2c, 4, mode);
	a5touch_write(i2c, 0xc, 0x40u | (receive ? 1u : 0u));
	delay(1);
	a5touch_write(i2c, 4, mode | 0x20u);
	if (!a5touch_wait_irq(i2c)) {
		printf("A5XELTE TOUCH: I2C2 address timeout stat=%#x con=%#x\n",
		    a5touch_read(i2c, 4), a5touch_read(i2c, 0));
		goto out;
	}
	if (a5touch_read(i2c, 4) & 1u) {
		printf("A5XELTE TOUCH: I2C2 address NACK stat=%#x\n",
		    a5touch_read(i2c, 4));
		goto out;
	}
	for (unsigned n = 0; n < count; n++) {
		if (receive) {
			if (n + 1 == count) {
				a5touch_write(i2c, 0, a5touch_read(i2c, 0) & ~0x80u);
			}
		} else {
			a5touch_write(i2c, 0xc, bytes[n]);
			delay(1);
		}
		a5touch_clear_irq(i2c);
		if (!a5touch_wait_irq(i2c)) {
			printf("A5XELTE TOUCH: I2C2 data timeout byte=%u stat=%#x\n",
			    n, a5touch_read(i2c, 4));
			goto out;
		}
		if (receive) {
			bytes[n] = (uint8_t)a5touch_read(i2c, 0xc);
		} else if (a5touch_read(i2c, 4) & 1u) {
			printf("A5XELTE TOUCH: I2C2 data NACK byte=%u stat=%#x\n",
			    n, a5touch_read(i2c, 4));
			goto out;
		}
	}
	ok = true;
out:
	a5touch_stop(i2c);
	return ok;
}

static void
a5touch_chip_code_trial(vm_offset_t nfc)
{
	vm_offset_t i2c = ml_io_map(0x13850000, 0x1000);
	uint32_t old_con;
	uint32_t old_pud;
	uint8_t reg[2] = {0x00, 0xcc};
	uint8_t code[2] = {0, 0};
	bool sent;
	bool got;
	if (!i2c) {
		printf("A5XELTE TOUCH: I2C2 mapping failed\n");
		return;
	}
	old_con = a5touch_read(nfc, 0);
	old_pud = a5touch_read(nfc, 8);
	/* GPC3-0/1: Linux touch_bus function 2 and PUD 3; preserve other pins. */
	a5touch_write(nfc, 8, (old_pud & ~0xfu) | 0xfu);
	a5touch_write(nfc, 0, (old_con & ~0xffu) | 0x22u);
	delay(100);
	sent = a5touch_message(i2c, false, reg, sizeof(reg));
	if (sent) {
		delay(75); /* ZT7538 requires a STOP and 50 us before receive. */
	}
	got = sent && a5touch_message(i2c, true, code, sizeof(code));
	printf("A5XELTE TOUCH: I2C2 chip-code sent=%d received=%d value=%#x stat=%#x con=%#x\n",
	    sent, got, (unsigned)code[0] | ((unsigned)code[1] << 8),
	    a5touch_read(i2c, 4), a5touch_read(i2c, 0));
	a5touch_write(nfc, 0, old_con);
	a5touch_write(nfc, 8, old_pud);
}

static bool
a5touch_send(uint8_t *data, unsigned length)
{
	for (unsigned attempt = 0; attempt < 8; attempt++) {
		if (a5touch_message(a5_touch_bus, false, data, length)) {
			return true;
		}
		delay(1100);
	}
	return false;
}

static bool
a5touch_cmd(uint16_t reg)
{
	uint8_t data[2] = {(uint8_t)reg, (uint8_t)(reg >> 8)};
	bool ok = a5touch_send(data, 2);
	delay(20);
	return ok;
}

static bool
a5touch_write_reg(uint16_t reg, uint16_t value)
{
	uint8_t data[4] = {(uint8_t)reg, (uint8_t)(reg >> 8),
	    (uint8_t)value, (uint8_t)(value >> 8)};
	bool ok = a5touch_send(data, 4);
	delay(20);
	return ok;
}

static bool
a5touch_read_reg(uint16_t reg, uint8_t *data, unsigned length)
{
	uint8_t address[2] = {(uint8_t)reg, (uint8_t)(reg >> 8)};
	if (!a5touch_send(address, 2)) {
		return false;
	}
	delay(75); /* The ZT7538 requires STOP then at least 50 us. */
	if (!a5touch_message(a5_touch_bus, true, data, length)) {
		return false;
	}
	delay(20);
	return true;
}

static bool
a5touch_read_u16(uint16_t reg, uint16_t *value)
{
	uint8_t data[2] = {0, 0};
	if (!a5touch_read_reg(reg, data, sizeof(data))) {
		return false;
	}
	*value = a5touch_le16(data);
	return true;
}

static void
a5touch_sleep_5ms(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(5, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&a5_touch_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
a5touch_recover(void)
{
	uint8_t ldo = 0;
	uint8_t check = 0;
	a5touch_release_contacts();
	if (!a5_touch_hsi || !a5pmic_read(a5_touch_hsi, 0x4e, &ldo) ||
	    (ldo & 0x3fu) != 0x3cu ||
	    !a5pmic_write(a5_touch_hsi, 0x4e, ldo & ~0xc0u)) {
		printf("A5XELTE TOUCH: LDO33 recovery power-off failed\n");
		return false;
	}
	delay(50000);
	if (!a5pmic_write(a5_touch_hsi, 0x4e, ldo | 0xc0u) ||
	    !a5pmic_read(a5_touch_hsi, 0x4e, &check) || check != (ldo | 0xc0u)) {
		printf("A5XELTE TOUCH: LDO33 recovery power-on failed value=%#x\n", check);
		return false;
	}
	delay(50000);
	return a5touch_controller_init();
}

static void
a5touch_log_thread(void *arg, wait_result_t result)
{
	unsigned lines = 0;
	unsigned errors = 0;
	unsigned idle_ticks = 0;
	unsigned recoveries = 0;
	(void)arg;
	(void)result;
	printf("A5XELTE TOUCH: contact HID worker started (GPA1-1)\n");
	for (;;) {
		/* ZT7538 holds INTN low until CLEAR_INT_STATUS. */
		if (!(a5touch_read(a5_touch_alive, 0x24) & 2u)) {
			uint8_t packet[34] = {0};
			bool read_ok = a5touch_read_reg(0x0080, packet, sizeof(packet));
			bool ack_ok = a5touch_cmd(0x0003);
			idle_ticks = 0;
			if (!read_ok || !ack_ok ||
			    (read_ok && ack_ok && !a5touch_report_packet(packet, &lines))) {
				if (++errors <= 8) {
					printf("A5XELTE TOUCH: packet/ACK failed read=%d ack=%d errors=%u\n",
					    read_ok, ack_ok, errors);
				}
			} else {
				errors = 0;
			}
		} else if (idle_ticks < 600) {
			idle_ticks++;
		}
		if (errors >= 8 || idle_ticks >= 600) {
			printf("A5XELTE TOUCH: recovery reason=%s attempt=%u\n",
			    errors >= 8 ? "transfer" : "health IRQ", recoveries + 1);
			if (!a5touch_recover() && ++recoveries >= 3) {
				printf("A5XELTE TOUCH: worker stopped after recovery failures\n");
				break;
			}
			errors = 0;
			idle_ticks = 0;
		}
		a5touch_sleep_5ms();
	}
	a5touch_release_contacts();
	thread_terminate_self();
}

static void
a5touch_release_contacts(void)
{
	for (unsigned slot = 0; slot < 5; slot++) {
		if (a5_touch_active[slot]) {
			sweet_hid_touch(slot, 0, a5_touch_x[slot], a5_touch_y[slot]);
			a5_touch_active[slot] = false;
		}
	}
}

/* Native Linux mode-0 packet policy, followed by the existing iOS HID
 * digitizer service.  A heartbeat leaves all slots unchanged. */
static bool
a5touch_report_packet(const uint8_t packet[34], unsigned *lines)
{
	uint16_t status = a5touch_le16(packet);
	unsigned count = packet[2];
	if (status == 0) {
		return true;
	}
	if (status == 0xffffu || status == 1u || (status & (1u << 13)) || count > 5) {
		printf("A5XELTE TOUCH: corrupt packet status=%#x count=%u\n", status, count);
		return false;
	}
	for (unsigned slot = 0; slot < 5; slot++) {
		unsigned at = 4 + 6 * slot;
		uint16_t x = a5touch_le16(packet + at);
		uint16_t y = a5touch_le16(packet + at + 2);
		unsigned sub = packet[at + 5];
		bool active = (status & (1u << 11)) && !(status & 0x30u) &&
		    (sub & 1u) && !(sub & 8u) && x <= 1080 && y <= 1920;
		if (active) {
			if (*lines < 256 && (!a5_touch_active[slot] ||
			    a5_touch_x[slot] != x || a5_touch_y[slot] != y)) {
				printf("A5XELTE TOUCH: contact slot=%u x=%u y=%u width=%u flags=%#x status=%#x\n",
				    slot, x, y, packet[at + 4], sub, status);
				(*lines)++;
			}
			a5_touch_x[slot] = x;
			a5_touch_y[slot] = y;
			a5_touch_active[slot] = true;
			sweet_hid_touch(slot, 1, x, y);
		} else if (a5_touch_active[slot]) {
			sweet_hid_touch(slot, 0, a5_touch_x[slot], a5_touch_y[slot]);
			a5_touch_active[slot] = false;
		}
	}
	return true;
}

static bool
a5touch_controller_init(void)
{
	uint16_t chip = 0;
	uint16_t crc = 0;
	uint16_t x = 0;
	uint16_t y = 0;
	/* Native ZT7538 power-on sequence; abort on each failed transfer. */
	if (!a5touch_write_reg(0xc000, 1) ||
	    !a5touch_read_u16(0xcc00, &chip) || chip != 0xe538) {
		printf("A5XELTE TOUCH: chip-code failed value=%#x\n", chip);
		goto fail;
	}
	printf("A5XELTE TOUCH: ZT7538 chip-code=%#x\n", chip);
	if (!a5touch_cmd(0xc004) || !a5touch_write_reg(0xc002, 1)) {
		printf("A5XELTE TOUCH: firmware init sequence failed\n");
		goto fail;
	}
	delay(2000);
	if (!a5touch_write_reg(0xc001, 1)) {
		printf("A5XELTE TOUCH: program start failed\n");
		goto fail;
	}
	delay(150000);
	if (!a5touch_read_u16(0x012c, &crc) || crc != 0x55aau ||
	    !a5touch_read_u16(0x00c0, &x) || !a5touch_read_u16(0x00c1, &y)) {
		printf("A5XELTE TOUCH: firmware CRC/geometry failed crc=%#x x=%u y=%u\n",
		    crc, x, y);
		goto fail;
	}
	printf("A5XELTE TOUCH: firmware CRC=%#x geometry=%ux%u\n", crc, x, y);
	if (!a5touch_cmd(0x0000) || !a5touch_write_reg(0x00f0, 0) ||
	    !a5touch_write_reg(0x0010, 0) ||
	    !a5touch_write_reg(0x00f0, 0x003f)) {
		printf("A5XELTE TOUCH: mode 0 setup failed\n");
		goto fail;
	}
	for (unsigned n = 0; n < 10; n++) {
		if (!a5touch_cmd(0x0003)) {
			printf("A5XELTE TOUCH: interrupt queue ACK failed\n");
			goto fail;
		}
	}
	if (!a5touch_write_reg(0x00f1, 100)) {
		printf("A5XELTE TOUCH: heartbeat setup failed\n");
		goto fail;
	}
	printf("A5XELTE TOUCH: mode 0 ready, INTN=%u\n",
	    (unsigned)!!(a5touch_read(a5_touch_alive, 0x24) & 2u));
	return true;
fail:
	return false;
}

static void
a5touch_start_early(vm_offset_t nfc, vm_offset_t alive)
{
	uint32_t con;
	uint32_t pud;
	thread_t thread;
	a5_touch_bus = ml_io_map(0x13850000, 0x1000);
	a5_touch_alive = alive;
	if (!a5_touch_bus) {
		printf("A5XELTE TOUCH: I2C2 mapping failed\n");
		return;
	}
	con = a5touch_read(nfc, 0);
	pud = a5touch_read(nfc, 8);
	a5touch_write(nfc, 8, (pud & ~0xfu) | 0xfu);
	a5touch_write(nfc, 0, (con & ~0xffu) | 0x22u);
	delay(100);
	if (!a5touch_controller_init()) {
		a5touch_write(nfc, 0, con);
		a5touch_write(nfc, 8, pud);
		return;
	}
	if (kernel_thread_start(a5touch_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("A5XELTE TOUCH: contact thread start failed\n");
	}
}

/*
 * Samsung Galaxy J7 Prime (SM-G610F, Exynos7870), selected by boot-arg j7hw=1.
 *
 * Touch is a Synaptics TD4300 speaking RMI4 over I2C1 (0x13840000, S3C2440
 * register layout, 7-bit address 0x70).  The controller sits behind the same
 * supply as the display, which boot firmware has already powered.  Nothing here
 * touches a PMIC.  The CMU_PERI gate for I2C1 is checked, and set only if it is
 * clear.  The sequence mirrors Linux rmi4 (PDT scan, F01 configure, F12 finger
 * objects) and logs what it finds with the "J7 TOUCH" prefix.
 *
 * Boot-args: j7touch=0 disables, j7touch_clk=0 never writes the clock gate,
 * j7touch_d1=N forces the F12 finger-data offset, j7touch_log=1 prints raw
 * finger packets.
 */
#define J7T_CMU_PERI_PA 0x101f0000u
#define J7T_GATE_REG    0x0810u
#define J7T_GATE_BUS    (1u << 3)    /* BUSP1_PERIC0_HCLK */
#define J7T_GATE_I2C1   (1u << 23)   /* I2C1_PCLK */
#define J7T_PINS_PA     0x139d0000u  /* pinctrl_touch, GPC3 */
#define J7T_I2C_PA      0x13840000u
#define J7T_ADDR        0xe0u        /* 7-bit 0x70 */
#define J7T_FINGERS     5

extern kern_return_t kernel_thread_start_priority(thread_continue_t, void *, int, thread_t *);

struct j7t_fn {
	uint16_t query, cmd, ctrl, data;
	uint8_t number, irq_pos, irq_count;
};

static vm_offset_t j7t_i2c;
static int j7t_sleep;
static int j7t_page = -1;
static unsigned j7t_i2c_errors;
static unsigned j7t_irq_regs = 1;
static struct j7t_fn j7t_f01, j7t_f12;
static uint8_t j7t_f12_mask[4];
static unsigned j7t_d1_offset;
static unsigned j7t_con = 0xe1u;   /* ACK, PCLK/512, IRQ flag, divider 1 */
static unsigned j7t_nobj = 2;      /* finger objects read per attention */
static bool j7t_active[J7T_FINGERS];
static uint16_t j7t_x[J7T_FINGERS], j7t_y[J7T_FINGERS];

static void
j7t_sleep_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&j7t_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* One bounded I2C message to 0x70 (START ... STOP), like a5touch_message. */
static bool
j7t_msg(bool receive, uint8_t *bytes, unsigned count)
{
	vm_offset_t i2c = j7t_i2c;
	const uint32_t mode = receive ? 0x90u : 0xd0u;
	bool ok = false;
	if (!count || count > 128) {
		return false;
	}
	for (unsigned n = 0; n < 5000; n++) {
		if (!(a5touch_read(i2c, 4) & 0x20u)) {
			break;
		}
		if (n == 4999) {
			if (++j7t_i2c_errors <= 8) {
				printf("J7 TOUCH: I2C busy before message\n");
			}
			return false;
		}
		delay(1);
	}
	a5touch_write(i2c, 0, j7t_con);
	a5touch_write(i2c, 0x10, 7u);
	a5touch_write(i2c, 4, mode);
	a5touch_write(i2c, 0xc, J7T_ADDR | (receive ? 1u : 0u));
	delay(1);
	a5touch_write(i2c, 4, mode | 0x20u);
	if (!a5touch_wait_irq(i2c)) {
		if (++j7t_i2c_errors <= 8) {
			printf("J7 TOUCH: I2C address timeout stat=%#x con=%#x\n",
			    a5touch_read(i2c, 4), a5touch_read(i2c, 0));
		}
		goto out;
	}
	if (a5touch_read(i2c, 4) & 1u) {
		if (++j7t_i2c_errors <= 8) {
			printf("J7 TOUCH: I2C address NACK stat=%#x\n", a5touch_read(i2c, 4));
		}
		goto out;
	}
	for (unsigned n = 0; n < count; n++) {
		if (receive) {
			if (n + 1 == count) {
				a5touch_write(i2c, 0, a5touch_read(i2c, 0) & ~0x80u);
			}
		} else {
			a5touch_write(i2c, 0xc, bytes[n]);
			delay(1);
		}
		a5touch_clear_irq(i2c);
		if (!a5touch_wait_irq(i2c)) {
			if (++j7t_i2c_errors <= 8) {
				printf("J7 TOUCH: I2C data timeout byte=%u stat=%#x\n",
				    n, a5touch_read(i2c, 4));
			}
			goto out;
		}
		if (receive) {
			bytes[n] = (uint8_t)a5touch_read(i2c, 0xc);
		} else if (a5touch_read(i2c, 4) & 1u) {
			if (++j7t_i2c_errors <= 8) {
				printf("J7 TOUCH: I2C data NACK byte=%u stat=%#x\n",
				    n, a5touch_read(i2c, 4));
			}
			goto out;
		}
	}
	ok = true;
out:
	a5touch_stop(i2c);
	return ok;
}

static bool
j7t_set_page(unsigned page)
{
	uint8_t cmd[2] = {0xff, (uint8_t)page};
	if (j7t_page == (int)page) {
		return true;
	}
	if (!j7t_msg(false, cmd, 2)) {
		return false;
	}
	j7t_page = (int)page;
	return true;
}

/* RMI4 register read: set page, send the register byte, STOP, receive. */
static bool
j7t_read(uint16_t addr, uint8_t *data, unsigned count)
{
	uint8_t reg = (uint8_t)(addr & 0xffu);
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (j7t_set_page((unsigned)(addr >> 8)) && j7t_msg(false, &reg, 1) &&
		    j7t_msg(true, data, count)) {
			return true;
		}
		j7t_page = -1;
		delay(1000);
	}
	return false;
}

static bool
j7t_write(uint16_t addr, const uint8_t *data, unsigned count)
{
	uint8_t buf[17];
	if (count == 0 || count > 16) {
		return false;
	}
	buf[0] = (uint8_t)(addr & 0xffu);
	for (unsigned i = 0; i < count; i++) {
		buf[1 + i] = data[i];
	}
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (j7t_set_page((unsigned)(addr >> 8)) && j7t_msg(false, buf, count + 1)) {
			return true;
		}
		j7t_page = -1;
		delay(1000);
	}
	return false;
}

/*
 * RMI4 register descriptor: one size byte N, then N bytes holding the
 * structure size (1 byte, or 0 + 2 bytes) and the register presence bitmap.
 * The structure that follows lists, for each present register, its size and
 * a subpacket bitmap.  A parse is trusted only if it consumes the structure
 * exactly.
 */
struct j7t_desc {
	uint8_t has[40];
	uint16_t size[40];
	unsigned hdr_len, struct_size;
	bool valid;
};

static bool
j7t_desc_header(const uint8_t *q, unsigned avail, struct j7t_desc *d)
{
	unsigned n = q[0];
	unsigned presence_from;
	unsigned bit = 0;
	for (unsigned i = 0; i < 40; i++) {
		d->has[i] = 0;
		d->size[i] = 0;
	}
	d->valid = false;
	if (n < 2 || n > 12 || 1 + n > avail) {
		return false;
	}
	if (q[1] == 0) {
		if (n < 4) {
			return false;
		}
		d->struct_size = (unsigned)q[2] | ((unsigned)q[3] << 8);
		presence_from = 3;
	} else {
		d->struct_size = q[1];
		presence_from = 1;
	}
	for (unsigned i = presence_from; i < n; i++) {
		for (unsigned b = 0; b < 8; b++, bit++) {
			if ((q[1 + i] & (1u << b)) && bit < 40) {
				d->has[bit] = 1;
			}
		}
	}
	d->hdr_len = 1 + n;
	return true;
}

static bool
j7t_desc_struct(const uint8_t *s, unsigned avail, struct j7t_desc *d)
{
	unsigned off = 0;
	if (d->struct_size > avail || d->struct_size == 0) {
		return false;
	}
	for (unsigned r = 0; r < 40; r++) {
		unsigned sz;
		if (!d->has[r]) {
			continue;
		}
		if (off >= d->struct_size) {
			return false;
		}
		sz = s[off++];
		if (!sz) {
			if (off + 2 > d->struct_size) {
				return false;
			}
			sz = (unsigned)s[off] | ((unsigned)s[off + 1] << 8);
			off += 2;
		}
		if (!sz) {
			if (off + 4 > d->struct_size) {
				return false;
			}
			sz = (unsigned)s[off] | ((unsigned)s[off + 1] << 8);
			off += 4;
		}
		d->size[r] = (uint16_t)sz;
		for (;;) {
			uint8_t sub;
			if (off >= d->struct_size) {
				return false;
			}
			sub = s[off++];
			if (!(sub & 0x80u)) {
				break;
			}
		}
	}
	d->valid = (off == d->struct_size);
	return d->valid;
}

/*
 * F12 Query 0 bit 0 says register descriptors exist.  They start at Query 1,
 * three of them (query, control, data).  Try the grouped layout (all headers,
 * then all structures) first and the interleaved one second.
 */
static bool
j7t_f12_parse(const uint8_t q[64], struct j7t_desc desc[3])
{
	if (!(q[0] & 1u)) {
		return false;
	}
	for (unsigned variant = 0; variant < 2; variant++) {
		unsigned at = 1;
		bool good = true;
		if (variant == 0) {
			for (unsigned i = 0; i < 3 && good; i++) {
				if (at >= 63 || !j7t_desc_header(q + at, 64 - at, &desc[i])) {
					good = false;
					break;
				}
				at += desc[i].hdr_len;
			}
			for (unsigned i = 0; i < 3 && good; i++) {
				if (at >= 64 || !j7t_desc_struct(q + at, 64 - at, &desc[i])) {
					good = false;
					break;
				}
				at += desc[i].struct_size;
			}
		} else {
			for (unsigned i = 0; i < 3 && good; i++) {
				if (at >= 63 || !j7t_desc_header(q + at, 64 - at, &desc[i])) {
					good = false;
					break;
				}
				at += desc[i].hdr_len;
				if (at >= 64 || !j7t_desc_struct(q + at, 64 - at, &desc[i])) {
					good = false;
					break;
				}
				at += desc[i].struct_size;
			}
		}
		if (good) {
			printf("J7 TOUCH: F12 descriptors parsed (%s layout), data D0=%u D1=%u bytes\n",
			    variant ? "interleaved" : "grouped",
			    desc[2].has[0] ? (unsigned)desc[2].size[0] : 0u,
			    desc[2].has[1] ? (unsigned)desc[2].size[1] : 0u);
			return true;
		}
	}
	return false;
}

static bool
j7t_scan_pdt(void)
{
	unsigned irq_total = 0;
	unsigned empty = 0;
	bool have01 = false, have12 = false;
	for (unsigned page = 0; page < 8 && empty < 2; page++) {
		unsigned found = 0;
		for (int a = 0xe9; a >= 0x05; a -= 6) {
			uint8_t e[6];
			struct j7t_fn fn;
			if (!j7t_read((uint16_t)((page << 8) | (unsigned)a), e, 6)) {
				if (page == 0) {
					return false;
				}
				break;
			}
			if (e[5] == 0 || e[5] == 0xff) {
				break;
			}
			fn.number = e[5];
			fn.query = (uint16_t)((page << 8) | e[0]);
			fn.cmd = (uint16_t)((page << 8) | e[1]);
			fn.ctrl = (uint16_t)((page << 8) | e[2]);
			fn.data = (uint16_t)((page << 8) | e[3]);
			fn.irq_count = (uint8_t)(e[4] & 7u);
			fn.irq_pos = (uint8_t)irq_total;
			irq_total += fn.irq_count;
			found++;
			printf("J7 TOUCH: F%02x page=%u query=%#x ctrl=%#x data=%#x irq=%u@%u\n",
			    fn.number, page, fn.query, fn.ctrl, fn.data, fn.irq_count, fn.irq_pos);
			if (fn.number == 0x01 && !have01) {
				j7t_f01 = fn;
				have01 = true;
			} else if (fn.number == 0x12 && !have12) {
				j7t_f12 = fn;
				have12 = true;
			}
		}
		empty = found ? 0 : empty + 1;
	}
	if (!have01 || !have12) {
		printf("J7 TOUCH: F01/F12 not found\n");
		return false;
	}
	j7t_irq_regs = (irq_total + 7) / 8;
	if (j7t_irq_regs < 1 || j7t_irq_regs > 3) {
		printf("J7 TOUCH: odd interrupt register count %u\n", j7t_irq_regs);
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		j7t_f12_mask[i] = 0;
	}
	for (unsigned b = 0; b < j7t_f12.irq_count; b++) {
		unsigned bit = j7t_f12.irq_pos + b;
		j7t_f12_mask[bit / 8] = (uint8_t)(j7t_f12_mask[bit / 8] | (1u << (bit % 8)));
	}
	return true;
}

/* Linux F01 probe: normal sleep mode, no-sleep, configured, enable F12 IRQ. */
static bool
j7t_configure(void)
{
	uint8_t ctrl0 = 0;
	uint8_t en[3] = {0, 0, 0};
	uint8_t status[4] = {0, 0, 0, 0};
	if (!j7t_read(j7t_f01.ctrl, &ctrl0, 1) ||
	    !j7t_read((uint16_t)(j7t_f01.ctrl + 1), en, j7t_irq_regs)) {
		return false;
	}
	ctrl0 = (uint8_t)((ctrl0 & ~0x03u) | 0x04u | 0x80u);
	for (unsigned i = 0; i < j7t_irq_regs; i++) {
		en[i] = (uint8_t)(en[i] | j7t_f12_mask[i]);
	}
	if (!j7t_write(j7t_f01.ctrl, &ctrl0, 1) ||
	    !j7t_write((uint16_t)(j7t_f01.ctrl + 1), en, j7t_irq_regs)) {
		return false;
	}
	(void)j7t_read(j7t_f01.data, status, 1 + j7t_irq_regs);
	printf("J7 TOUCH: configured ctrl0=%#x irq_enable=%#x,%#x status=%#x irq=%#x,%#x\n",
	    ctrl0, en[0], en[1], status[0], status[1], status[2]);
	return true;
}

static void
j7t_release_contacts(void)
{
	for (unsigned slot = 0; slot < J7T_FINGERS; slot++) {
		if (j7t_active[slot]) {
			sweet_hid_touch((int)slot, 0, j7t_x[slot], j7t_y[slot]);
			j7t_active[slot] = false;
		}
	}
}

static void
j7t_report(const uint8_t *o, int verbose)
{
	static unsigned lines;
	int highest = -1;
	for (unsigned slot = 0; slot < J7T_FINGERS; slot++) {
		const uint8_t *p = o + slot * 8;
		unsigned type = p[0];
		unsigned x = (unsigned)p[1] | ((unsigned)p[2] << 8);
		unsigned y = (unsigned)p[3] | ((unsigned)p[4] << 8);
		bool active = (type == 1 || type == 6) && x <= 1080 && y <= 1920;
		if (active) {
			if ((verbose || lines < 128) && (!j7t_active[slot] ||
			    j7t_x[slot] != x || j7t_y[slot] != y)) {
				printf("J7 TOUCH: contact slot=%u type=%u x=%u y=%u z=%u\n",
				    slot, type, x, y, p[5]);
				lines++;
			}
			j7t_x[slot] = (uint16_t)x;
			j7t_y[slot] = (uint16_t)y;
			j7t_active[slot] = true;
			highest = (int)slot;
			sweet_hid_touch((int)slot, 1, (int)x, (int)y);
		} else if (j7t_active[slot]) {
			sweet_hid_touch((int)slot, 0, j7t_x[slot], j7t_y[slot]);
			j7t_active[slot] = false;
		}
	}
	/* Next time read one slot past the highest finger, at least two. */
	j7t_nobj = (unsigned)(highest + 2);
	if (j7t_nobj < 2) {
		j7t_nobj = 2;
	}
	if (j7t_nobj > J7T_FINGERS) {
		j7t_nobj = J7T_FINGERS;
	}
}

static bool
j7t_hw_init(void)
{
	vm_offset_t cmu = ml_io_map(J7T_CMU_PERI_PA, 0x1000);
	vm_offset_t pins = ml_io_map(J7T_PINS_PA, 0x1000);
	uint32_t need = J7T_GATE_BUS | J7T_GATE_I2C1;
	uint32_t gate;
	uint32_t con, pud;
	int allow_clk = 1;
	uint8_t q[64];
	struct j7t_desc desc[3];
	int forced_d1 = -1;
	int d1;
	if (!cmu || !pins) {
		printf("J7 TOUCH: MMIO mapping failed\n");
		return false;
	}
	(void)PE_parse_boot_argn("j7touch_clk", &allow_clk, sizeof(allow_clk));
	gate = a5touch_read(cmu, J7T_GATE_REG);
	printf("J7 TOUCH: CMU_PERI gate %#x (need %#x)\n", gate, need);
	if ((gate & need) != need) {
		if (!allow_clk) {
			printf("J7 TOUCH: I2C1 clock gate off and j7touch_clk=0; giving up\n");
			return false;
		}
		a5touch_write(cmu, J7T_GATE_REG, gate | need);
		delay(100);
		gate = a5touch_read(cmu, J7T_GATE_REG);
		printf("J7 TOUCH: CMU_PERI gate now %#x\n", gate);
		if ((gate & need) != need) {
			printf("J7 TOUCH: I2C1 clock gate did not enable; giving up\n");
			return false;
		}
	}
	con = a5touch_read(pins, 0);
	pud = a5touch_read(pins, 8);
	printf("J7 TOUCH: GPC3 con=%#x dat=%#x pud=%#x\n", con, a5touch_read(pins, 4), pud);
	/* GPC3-0/1 are SDA/SCL (function 2) with pull-ups, as in Linux i2c1_bus. */
	a5touch_write(pins, 8, (pud & ~0xfu) | 0xfu);
	a5touch_write(pins, 0, (con & ~0xffu) | 0x22u);
	delay(100);
	j7t_i2c = ml_io_map(J7T_I2C_PA, 0x1000);
	if (!j7t_i2c) {
		printf("J7 TOUCH: I2C mapping failed\n");
		goto fail;
	}
	printf("J7 TOUCH: I2C1 con=%#x stat=%#x lc=%#x\n", a5touch_read(j7t_i2c, 0),
	    a5touch_read(j7t_i2c, 4), a5touch_read(j7t_i2c, 0x10));
	if (!j7t_scan_pdt()) {
		printf("J7 TOUCH: RMI4 page descriptor scan failed (no answer from 0x70)\n");
		goto fail;
	}
	if (!j7t_read(j7t_f12.query, q, 64)) {
		printf("J7 TOUCH: F12 query read failed\n");
		goto fail;
	}
	printf("J7 TOUCH: F12 query %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
	printf("J7 TOUCH: F12 query %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    q[12], q[13], q[14], q[15], q[16], q[17], q[18], q[19], q[20], q[21], q[22], q[23]);
	j7t_d1_offset = 0;
	if (j7t_f12_parse(q, desc)) {
		j7t_d1_offset = desc[2].has[0] ? (unsigned)desc[2].size[0] : 0u;
	} else {
		printf("J7 TOUCH: F12 descriptors not understood, assuming finger data at +0\n");
	}
	(void)PE_parse_boot_argn("j7touch_d1", &forced_d1, sizeof(forced_d1));
	d1 = forced_d1;
	if (d1 >= 0 && d1 < 32) {
		j7t_d1_offset = (unsigned)d1;
	}
	printf("J7 TOUCH: F12 finger data at data+%u\n", j7t_d1_offset);
	if (!j7t_configure()) {
		printf("J7 TOUCH: F01 configure failed\n");
		goto fail;
	}
	return true;
fail:
	a5touch_write(pins, 0, con);
	a5touch_write(pins, 8, pud);
	return false;
}

static void
j7t_thread(void *arg, wait_result_t result)
{
	int verbose = 0;
	unsigned bad = 0;
	unsigned reconf = 0;
	unsigned idle = 0;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("j7touch_log", &verbose, sizeof(verbose));
	if (!j7t_hw_init()) {
		thread_terminate_self();
		return;
	}
	printf("J7 TOUCH: worker running\n");
	for (;;) {
		uint8_t st[4] = {0, 0, 0, 0};
		bool attn = false;
		if (!j7t_read(j7t_f01.data, st, 1 + j7t_irq_regs)) {
			if (++bad >= 50) {
				printf("J7 TOUCH: too many I2C failures, worker stopped\n");
				break;
			}
			j7t_sleep_ms(20);
			continue;
		}
		bad = 0;
		if (st[0] & 0x80u) {
			/* Controller says it is unconfigured (it reset): redo F01,
			 * at most four times and not more often than about once a second. */
			if (reconf < 4 && ++idle > 120) {
				printf("J7 TOUCH: controller unconfigured status=%#x, reconfiguring\n", st[0]);
				(void)j7t_configure();
				reconf++;
				idle = 0;
			}
		} else {
			idle = 0;
		}
		for (unsigned r = 0; r < j7t_irq_regs; r++) {
			if (st[1 + r] & j7t_f12_mask[r]) {
				attn = true;
			}
		}
		if (attn) {
			uint8_t o[J7T_FINGERS * 8];
			unsigned nobj = j7t_nobj;
			for (unsigned i = nobj * 8; i < sizeof(o); i++) {
				o[i] = 0;
			}
			if (j7t_read((uint16_t)(j7t_f12.data + j7t_d1_offset), o, nobj * 8)) {
				if (verbose) {
					printf("J7 TOUCH: raw %02x %02x %02x %02x %02x %02x %02x %02x\n",
					    o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
				}
				j7t_report(o, verbose);
			}
			j7t_sleep_ms(4);
		} else {
			j7t_sleep_ms(8);
		}
	}
	j7t_release_contacts();
	thread_terminate_self();
}

static void
j7prime_touch_start(void)
{
	thread_t thread;
	int enabled = 1;
	int pri = 28;
	int div = 1;
	(void)PE_parse_boot_argn("j7touch", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("J7 TOUCH: disabled\n");
		return;
	}
	/* Below the default user priority, so a busy bus never starves SpringBoard. */
	(void)PE_parse_boot_argn("j7touch_pri", &pri, sizeof(pri));
	(void)PE_parse_boot_argn("j7touch_div", &div, sizeof(div));
	if (pri < 4 || pri > 80) {
		pri = 28;
	}
	if (div < 0 || div > 15) {
		div = 1;
	}
	j7t_con = 0xe0u | (unsigned)div;
	printf("J7 TOUCH: thread priority %d, I2C CON %#x\n", pri, j7t_con);
	if (kernel_thread_start_priority(j7t_thread, NULL, pri, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("J7 TOUCH: thread start failed\n");
	}
}


void
a5xelte_touch_probe(void)
{
	int enabled = 1;
	int log_contacts = 0;
	int bus_trial = 0;
	vm_offset_t alive;
	vm_offset_t nfc;
	vm_offset_t mifpins;
	vm_offset_t peri;
	vm_offset_t mif;
	uint32_t pclk;
	uint32_t mifclk;
	{
		int j7hw = 0;
		(void)PE_parse_boot_argn("j7hw", &j7hw, sizeof(j7hw));
		if (j7hw) {
			j7prime_touch_start();
			return;
		}
	}
	(void)PE_parse_boot_argn("a5touchprobe", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("a5touchlog", &log_contacts, sizeof(log_contacts));
	if (!enabled) {
		return;
	}
	alive = ml_io_map(0x139b0000, 0x1000);
	nfc = ml_io_map(0x138c0000, 0x1000);
	mifpins = ml_io_map(0x104e0000, 0x1000);
	peri = ml_io_map(0x139f0000, 0x1000);
	mif = ml_io_map(0x10430000, 0x1000);
	if (!alive || !nfc || !mifpins || !peri || !mif) {
		printf("A5XELTE TOUCH: probe MMIO mapping failed\n");
		return;
	}
	pclk = a5touch_read(peri, 0x904);
	mifclk = a5touch_read(mif, 0x900);
	printf("A5XELTE TOUCH: INT GPA1 con=%#x dat=%#x; GPC3 con=%#x dat=%#x pud=%#x\n",
	    a5touch_read(alive, 0x20), a5touch_read(alive, 0x24),
	    a5touch_read(nfc, 0), a5touch_read(nfc, 4), a5touch_read(nfc, 8));
	printf("A5XELTE TOUCH: I2C2 gate=%d MIF HSI2C3 gate=%d GPM0 con=%#x dat=%#x pud=%#x\n",
	    !!(pclk & (1u << 9)), !!(mifclk & (1u << 27)),
	    a5touch_read(mifpins, 0), a5touch_read(mifpins, 4),
	    a5touch_read(mifpins, 8));
	if (pclk & (1u << 9)) {
		vm_offset_t i2c = ml_io_map(0x13850000, 0x1000);
		if (i2c) {
			printf("A5XELTE TOUCH: I2C2 con=%#x stat=%#x add=%#x lc=%#x (read only)\n",
			    a5touch_read(i2c, 0), a5touch_read(i2c, 4),
			    a5touch_read(i2c, 8), a5touch_read(i2c, 0x10));
		}
		(void)PE_parse_boot_argn("a5touchbus", &bus_trial, sizeof(bus_trial));
		if (log_contacts && (mifclk & (1u << 27))) {
			if (a5touch_power_on(mifpins)) {
				a5touch_start_early(nfc, alive);
			}
		} else if (bus_trial) {
			a5touch_chip_code_trial(nfc);
		}
	}
}
#endif
