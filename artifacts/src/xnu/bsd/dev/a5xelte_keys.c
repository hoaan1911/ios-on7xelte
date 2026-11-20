/*
 * Exynos7580 ALIVE GPIO keys for the SM-A510F.
 *
 * The working Linux device tree locates Power/Volume Up/Volume Down/Home at
 * GPA2 pins 7/0/1/5, all active low.  Poll only GPA2DAT; boot firmware owns
 * the pin mux and pull-ups.  No ALIVE or interrupt-controller writes occur.
 *
 * HEROLTE (Galaxy S7, Exynos 8890, from its BOOT DTB gpio_keys): ALIVE at
 * 0x10580000, Power GPA2 pin 4, Volume Up/Down and Home GPA0 pins 3/4/5,
 * active low.  S-Boot reads them too, so their mux is already set; the two
 * DAT registers are sampled together (bits 0-7 GPA0, 8-15 GPA2).
 */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_hid_key(int key, int down);

#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#define A5_ALIVE_PA 0x10580000u
#define A5_GPA0_CON 0x00u
#define A5_GPA0_DAT 0x04u
#define A5_GPA0_PUD 0x08u
#else
#define A5_ALIVE_PA 0x139b0000u
#endif
#define A5_GPA2_CON 0x40u
#define A5_GPA2_DAT 0x44u
#define A5_GPA2_PUD 0x48u

static vm_offset_t a5_alive;
/* Last Power press (mach_absolute_time); SweetFramebuffer.cpp reads it. */
extern uint64_t a5xelte_power_pressed_at;
uint64_t a5xelte_power_pressed_at;
static int a5_keys_sleep;
static bool a5_keys_started;

static uint32_t
a5_keys_read(uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(a5_alive + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
a5_keys_wait(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(10, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&a5_keys_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Key order of sweet_hid_key(): power, volume up, volume down, home. */
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
static uint16_t
a5_keys_sample(void)
{
	return (uint16_t)((a5_keys_read(A5_GPA0_DAT) & 0xff) |
	       ((a5_keys_read(A5_GPA2_DAT) & 0xff) << 8));
}
static const uint8_t a5_key_pins[4] = {8 + 4, 3, 4, 5};
#else
static uint16_t
a5_keys_sample(void)
{
	return (uint16_t)(a5_keys_read(A5_GPA2_DAT) & 0xff);
}
static const uint8_t a5_key_pins[4] = {7, 0, 1, 5};
#endif

static void
a5_keys_thread(void *arg, wait_result_t result)
{
	uint16_t stable = a5_keys_sample();
	uint16_t previous = stable;
	unsigned repeated = 0;
	(void)arg;
	(void)result;
	for (;;) {
		uint16_t sample = a5_keys_sample();
		if (sample == previous) {
			if (repeated < 2) {
				repeated++;
			}
		} else {
			previous = sample;
			repeated = 0;
		}
		if (repeated == 2 && stable != sample) {
			for (unsigned key = 0; key < 4; key++) {
				uint16_t bit = (uint16_t)(1u << a5_key_pins[key]);
				if ((stable ^ sample) & bit) {
					int down = !(sample & bit);
					printf("A5XELTE KEYS: key=%u down=%d gpio=%#x\n",
					    key, down, sample);
					if (key == 0 && down) {
						a5xelte_power_pressed_at = mach_absolute_time();
					}
					sweet_hid_key((int)key, down);
				}
			}
			stable = sample;
		}
		a5_keys_wait();
	}
}

/*
 * Samsung Galaxy J7 Prime (SM-G610F, Exynos7870), selected by boot-arg j7hw=1.
 *
 * The mainline on7xelte device tree puts the keys on the ALIVE pin controller
 * (0x139f0000): Power GPA0-0, Home GPA1-7, Volume Up GPA2-0 and Volume Down
 * GPA2-1, all active low.  The bank offsets inside that block are not the same
 * as on the Exynos7580, so the two plausible layouts are scored against the
 * live registers (every key pin must be an input/EINT pin that idles high)
 * and only a layout that matches is used.  Everything is read only, and
 * boot-arg j7keys_layout=1|2 forces a layout.
 */
#define J7K_ALIVE_PA 0x139f0000u

extern kern_return_t kernel_thread_start_priority(thread_continue_t, void *, int, thread_t *);

static uint32_t j7k_bank[3];    /* register offsets of GPA0, GPA1, GPA2 */
/* Key order of sweet_hid_key(): power, volume up, volume down, home. */
static const uint8_t j7k_pins[4] = {0, 16, 17, 15};

static uint32_t
j7k_sample(void)
{
	return (a5_keys_read(j7k_bank[0] + 4) & 0xffu) |
	       ((a5_keys_read(j7k_bank[1] + 4) & 0xffu) << 8) |
	       ((a5_keys_read(j7k_bank[2] + 4) & 0xffu) << 16);
}

static bool
j7k_layout_matches(const uint32_t bank[3])
{
	/* GPA0-0 power, GPA2-0 volume up, GPA2-1 volume down, GPA1-7 home */
	static const uint8_t which[4] = {0, 2, 2, 1};
	static const uint8_t pin[4] = {0, 0, 1, 7};
	for (unsigned i = 0; i < 4; i++) {
		uint32_t con = (a5_keys_read(bank[which[i]]) >> (4u * pin[i])) & 0xfu;
		uint32_t dat = a5_keys_read(bank[which[i]] + 4);
		if ((con != 0x0u && con != 0xfu) || !(dat & (1u << pin[i]))) {
			return false;
		}
	}
	return true;
}

static void
j7k_thread(void *arg, wait_result_t result)
{
	uint32_t stable = j7k_sample();
	uint32_t previous = stable;
	unsigned repeated = 0;
	unsigned lines = 0;
	(void)arg;
	(void)result;
	for (;;) {
		uint32_t sample = j7k_sample();
		if (sample == previous) {
			if (repeated < 2) {
				repeated++;
			}
		} else {
			previous = sample;
			repeated = 0;
		}
		if (repeated == 2 && stable != sample) {
			for (unsigned key = 0; key < 4; key++) {
				uint32_t bit = 1u << j7k_pins[key];
				if ((stable ^ sample) & bit) {
					int down = !(sample & bit);
					if (lines < 200) {
						lines++;
						printf("J7 KEYS: key=%u down=%d gpio=%#x\n", key, down, sample);
					}
					if (key == 0 && down) {
						a5xelte_power_pressed_at = mach_absolute_time();
					}
					sweet_hid_key((int)key, down);
				}
			}
			stable = sample;
		}
		a5_keys_wait();
	}
}

static void
j7prime_keys_init(void)
{
	static const uint32_t layout1[3] = {0x40, 0x60, 0x80}; /* etc0/etc1 first */
	static const uint32_t layout2[3] = {0x00, 0x20, 0x40}; /* Exynos7580 style */
	const uint32_t *chosen = NULL;
	int enabled = 1;
	int forced = 0;
	thread_t thread;
	(void)PE_parse_boot_argn("j7keys", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("j7keys_layout", &forced, sizeof(forced));
	if (!enabled) {
		printf("J7 KEYS: disabled\n");
		return;
	}
	a5_alive = ml_io_map(J7K_ALIVE_PA, 0x1000);
	if (!a5_alive) {
		printf("J7 KEYS: ALIVE mapping failed\n");
		return;
	}
	for (unsigned off = 0; off < 0xc0; off += 0x20) {
		printf("J7 KEYS: ALIVE +%#x con=%#x dat=%#x\n", off,
		    a5_keys_read(off), a5_keys_read(off + 4));
	}
	if (forced == 1) {
		chosen = layout1;
	} else if (forced == 2) {
		chosen = layout2;
	} else if (j7k_layout_matches(layout1)) {
		chosen = layout1;
	} else if (j7k_layout_matches(layout2)) {
		chosen = layout2;
	}
	if (!chosen) {
		printf("J7 KEYS: no ALIVE layout matched (see dump above); keys disabled\n");
		return;
	}
	for (unsigned i = 0; i < 3; i++) {
		j7k_bank[i] = chosen[i];
	}
	printf("J7 KEYS: using GPA0=%#x GPA1=%#x GPA2=%#x sample=%#x\n",
	    j7k_bank[0], j7k_bank[1], j7k_bank[2], j7k_sample());
	if (kernel_thread_start_priority(j7k_thread, NULL, 28, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("J7 KEYS: thread start failed\n");
	}
}


void
a5xelte_keys_init(void)
{
	thread_t thread;
	int enabled = 1;
	if (a5_keys_started) {
		return;
	}
	a5_keys_started = true;
	{
		int j7hw = 0;
		(void)PE_parse_boot_argn("j7hw", &j7hw, sizeof(j7hw));
		if (j7hw) {
			j7prime_keys_init();
			return;
		}
	}
	(void)PE_parse_boot_argn("a5keys", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("A5XELTE KEYS: disabled\n");
		return;
	}
	a5_alive = ml_io_map(A5_ALIVE_PA, 0x1000);
	if (!a5_alive) {
		printf("A5XELTE KEYS: ALIVE mapping failed\n");
		return;
	}
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	printf("A5XELTE KEYS: GPA0 con=%#x dat=%#x pud=%#x (read only)\n",
	    a5_keys_read(A5_GPA0_CON), a5_keys_read(A5_GPA0_DAT),
	    a5_keys_read(A5_GPA0_PUD));
#endif
	printf("A5XELTE KEYS: GPA2 con=%#x dat=%#x pud=%#x (read only)\n",
	    a5_keys_read(A5_GPA2_CON), a5_keys_read(A5_GPA2_DAT),
	    a5_keys_read(A5_GPA2_PUD));
	kern_return_t kr = kernel_thread_start(a5_keys_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("A5XELTE KEYS: thread start failed %d\n", kr);
	}
}
#endif
