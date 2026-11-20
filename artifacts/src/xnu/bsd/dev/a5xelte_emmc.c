/*
 * Exynos7580 DesignWare eMMC bring-up.
 *
 * The Samsung bootloader has already selected the eMMC user area and set up
 * its clocks and pads. Do not reset that state. Root writes require
 * emmcroot=USERDATA and emmcwrite=1. An opt-in console log uses only the
 * first 4 MiB + 512 bytes of the GPT-verified SYSTEM partition.
 * The host register map and FIFO width rules follow the working downstream
 * dw_mmc / dw_mmc-exynos driver for the SM-A510F.
 */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <machine/atomic.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int hz;

#define A5_LOG_SIZE   (4u << 20)
#define A5_LOG_SECTORS (1u + A5_LOG_SIZE / 512u)

#define EMMC_CTRL       0x000u
#define EMMC_PWREN      0x004u
#define EMMC_CLKENA     0x010u
#define EMMC_TMOUT      0x014u
#define EMMC_BLKSIZ     0x01cu
#define EMMC_BYTCNT     0x020u
#define EMMC_INTMASK    0x024u
#define EMMC_CMDARG     0x028u
#define EMMC_CMD        0x02cu
#define EMMC_RINTSTS    0x044u
#define EMMC_STATUS     0x048u
#define EMMC_TCBCNT     0x05cu
#define EMMC_TBBCNT     0x060u
#define EMMC_VERID      0x06cu
#define EMMC_HCON       0x070u
#define EMMC_FIFO_DEPTH 64u /* a5xelte downstream DT fifo-depth = <0x40> */
#define EMMC_CLKSEL     0x0a8u /* Exynos7580 64-bit IDMAC register map */

#define CTRL_INT_ENABLE (1u << 4)
#define CTRL_DMA_ENABLE (1u << 5)
#define CTRL_USE_IDMAC  (1u << 25)
#define CTRL_FIFO_RESET (1u << 1)
#define CMD_START       (1u << 31)
#define CMD_HOLD        (1u << 29)
#define CMD_WAIT_DATA   (1u << 13)
#define CMD_DATA        (1u << 9)
#define CMD_WRITE       (1u << 10)
#define CMD_RESP_CRC    (1u << 8)
#define CMD_RESP       (1u << 6)
#define INT_DATA_OVER   (1u << 3)
#define INT_CMD_DONE    (1u << 2)
#define INT_RXDR        (1u << 5)
#define INT_TXDR        (1u << 4)
/* HTO (data starvation by host timeout): the controller stopped the card clock because this
 * polled loop was slow to service the FIFO (it got preempted). Nothing is lost; keep going. */
#define INT_HTO         (1u << 10)
#define INT_ERRORS      ((1u << 12) | (1u << 11) | (1u << 9) | \
                         (1u << 8) | (1u << 7) | (1u << 6) | (1u << 1))

/* Opt-in fast path (boot-arg emmcfast=1): CMD18 multi-block reads with the controller's auto-stop. */
#define CMD_SEND_STOP   (1u << 12) /* controller sends CMD12 itself at the end of the data phase */
#define CMD_STOP_ABORT  (1u << 14) /* this command is a stop/abort (CMD12) */
#define STATUS_BUSY     (1u << 9)  /* card data busy (DAT0 low) */
#define EMMC_MAX_RUN    32u        /* sectors per CMD18: 16 KiB, bounds the polled time under the lock */

static vm_offset_t emmc_regs;
static uint64_t emmc_first_lba, emmc_nsectors;
static char emmc_partition_name[16] = "SYSTEM";
static dev_t emmc_bdev;
static bool emmc_ready;
static bool emmc_writable;
static uint64_t emmc_log_first_lba, emmc_log_nsectors;
static bool emmc_log_enabled;
static char emmc_log_ring[A5_LOG_SIZE];
static uint64_t _Atomic emmc_log_total;
static int emmc_log_chan;
static LCK_GRP_DECLARE(a5xelte_emmc_grp, "a5xelte_emmc");
static LCK_MTX_DECLARE(a5xelte_emmc_lock, &a5xelte_emmc_grp);

static inline uint32_t
emmc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(emmc_regs + off);
}

static inline void
emmc_wr(uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(emmc_regs + off) = value;
}

static int
emmc_read_sector_once(uint32_t lba, uint8_t *sector)
{
	uint64_t data[64] = { 0 };
	uint32_t hcon = emmc_rd(EMMC_HCON);
	uint32_t width = (hcon >> 7) & 7u;
	uint32_t ver = emmc_rd(EMMC_VERID) & 0xffffu;
	uint32_t data_off = ver < 0x240au ? 0x100u : 0x200u;
	uint32_t unit, words, read_words = 0;
	uint32_t ctrl = emmc_rd(EMMC_CTRL);
	uint32_t ints = emmc_rd(EMMC_INTMASK);
	int error = 0;

	if (width > 2 || ver == 0 || ver == 0xffff ||
	    !(emmc_rd(EMMC_PWREN) & 1u) || !(emmc_rd(EMMC_CLKENA) & 1u)) {
		printf("A5XELTE eMMC: host unavailable (ver=%x hcon=%x clk=%x)\n",
		    ver, hcon, emmc_rd(EMMC_CLKENA));
		return ENXIO;
	}
	unit = 2u << width;
	words = 512u / unit;
	if (emmc_rd(EMMC_STATUS) & (1u << 9)) {
		printf("A5XELTE eMMC: data path busy; skipping probe\n");
		return EBUSY;
	}

	/* Polled PIO, preserving the bootloader's clock, pads and bus timing. */
	emmc_wr(EMMC_INTMASK, 0);
	emmc_wr(EMMC_CTRL, (ctrl & ~(CTRL_INT_ENABLE | CTRL_DMA_ENABLE |
	    CTRL_USE_IDMAC)) | CTRL_FIFO_RESET);
	for (unsigned i = 0; i < 1000000; i++) {
		if (!(emmc_rd(EMMC_CTRL) & CTRL_FIFO_RESET)) {
			break;
		}
		if (i == 999999) {
			error = ETIMEDOUT;
			goto out;
		}
	}
	/* Known-good Linux attempt23 encoding: counter [31:11], extension [10:8]. */
	emmc_wr(EMMC_TMOUT, 0x573177ffu);
	emmc_wr(EMMC_BLKSIZ, 512);
	emmc_wr(EMMC_BYTCNT, 512);
	emmc_wr(EMMC_RINTSTS, 0xffffffffu);
	emmc_wr(EMMC_CMDARG, lba); /* high-capacity eMMC sector address */
	uint32_t command = CMD_START | CMD_WAIT_DATA | CMD_DATA |
	    CMD_RESP_CRC | CMD_RESP | 17u;
	if ((emmc_rd(EMMC_CLKSEL) >> 16) & 7u) {
		command |= CMD_HOLD;
	}
	emmc_wr(EMMC_CMD, command);
	for (unsigned i = 0; i < 4000000; i++) {
		uint32_t pending = emmc_rd(EMMC_RINTSTS);
		if (pending & INT_ERRORS) {
			printf("A5XELTE eMMC: CMD17 error rint=%x status=%x words=%u/%u first=%llx tcbc=%u tbbc=%u\n",
			    pending, emmc_rd(EMMC_STATUS), read_words, words, data[0],
			    emmc_rd(EMMC_TCBCNT), emmc_rd(EMMC_TBBCNT));
			error = EIO;
			goto out;
		}
		if (pending & INT_HTO) {
			emmc_wr(EMMC_RINTSTS, INT_HTO);
		}
		uint32_t fifo = (emmc_rd(EMMC_STATUS) >> 17) & 0x1fffu;
		while (fifo && read_words < words) {
			if (unit == 8) {
				/* Exynos7580 requires paired 32-bit SFR accesses for a 64-bit FIFO unit. */
				uint32_t lo = emmc_rd(data_off);
				uint32_t hi = emmc_rd(data_off + 4);
				data[read_words] = (uint64_t)lo | ((uint64_t)hi << 32);
			} else if (unit == 4) {
				((uint32_t *)data)[read_words] = emmc_rd(data_off);
			} else {
				((uint16_t *)data)[read_words] =
				    *(volatile uint16_t *)(emmc_regs + data_off);
			}
			read_words++;
			fifo--;
		}
		/* The downstream PIO handler acknowledges RXDR after each FIFO drain. */
		if (pending & INT_RXDR) {
			emmc_wr(EMMC_RINTSTS, INT_RXDR);
		}
		if (read_words == words &&
		    (pending & INT_DATA_OVER) && (pending & INT_CMD_DONE)) {
			break;
		}
		if (i == 3999999) {
			printf("A5XELTE eMMC: CMD17 timeout rint=%x words=%u/%u first=%llx tcbc=%u tbbc=%u\n",
			    pending, read_words, words, data[0],
			    emmc_rd(EMMC_TCBCNT), emmc_rd(EMMC_TBBCNT));
			error = ETIMEDOUT;
			goto out;
		}
	}
	memcpy(sector, data, 512);
out:
	emmc_wr(EMMC_RINTSTS, 0xffffffffu);
	emmc_wr(EMMC_INTMASK, ints);
	/* Keep DMA disabled: this probe does not own the bootloader's descriptors. */
	return error;
}

/* A failed block read fails the whole pagein, which kills the process with SIGBUS: retry first. */
static int
emmc_read_sector(uint32_t lba, uint8_t *sector)
{
	int error = 0;

	for (unsigned attempt = 0; attempt < 4; attempt++) {
		error = emmc_read_sector_once(lba, sector);
		if (error != EIO && error != ETIMEDOUT) {
			break;
		}
	}
	return error;
}

static bool emmc_fast;

/* Wait for the card to leave busy (auto-stop's CMD12 is an R1b). */
static int
emmc_wait_not_busy(void)
{
	for (unsigned i = 0; i < 4000000; i++) {
		if (!(emmc_rd(EMMC_STATUS) & STATUS_BUSY)) {
			return 0;
		}
	}
	return ETIMEDOUT;
}

/* Get the card and the FIFO back to a known state after a failed CMD18. */
static void
emmc_abort_transfer(void)
{
	uint32_t command = CMD_START | CMD_STOP_ABORT | CMD_RESP_CRC | CMD_RESP | 12u;

	emmc_wr(EMMC_RINTSTS, 0xffffffffu);
	emmc_wr(EMMC_CMDARG, 0);
	if ((emmc_rd(EMMC_CLKSEL) >> 16) & 7u) {
		command |= CMD_HOLD;
	}
	emmc_wr(EMMC_CMD, command);
	for (unsigned i = 0; i < 1000000; i++) {
		if (emmc_rd(EMMC_RINTSTS) & (INT_CMD_DONE | INT_ERRORS)) {
			break;
		}
	}
	(void)emmc_wait_not_busy();
	emmc_wr(EMMC_CTRL, emmc_rd(EMMC_CTRL) | CTRL_FIFO_RESET);
	for (unsigned i = 0; i < 1000000; i++) {
		if (!(emmc_rd(EMMC_CTRL) & CTRL_FIFO_RESET)) {
			break;
		}
	}
	emmc_wr(EMMC_RINTSTS, 0xffffffffu);
}

static int
emmc_read_sectors_once(uint32_t lba, uint32_t nsect, uint8_t *out)
{
	uint32_t hcon = emmc_rd(EMMC_HCON);
	uint32_t width = (hcon >> 7) & 7u;
	uint32_t ver = emmc_rd(EMMC_VERID) & 0xffffu;
	uint32_t data_off = ver < 0x240au ? 0x100u : 0x200u;
	uint32_t unit, words, read_words = 0;
	uint32_t ctrl = emmc_rd(EMMC_CTRL);
	uint32_t ints = emmc_rd(EMMC_INTMASK);
	uint32_t limit;
	int error = 0;

	if (width > 2 || ver == 0 || ver == 0xffff || nsect == 0 || nsect > EMMC_MAX_RUN ||
	    !(emmc_rd(EMMC_PWREN) & 1u) || !(emmc_rd(EMMC_CLKENA) & 1u)) {
		return ENXIO;
	}
	unit = 2u << width;
	words = nsect * 512u / unit;
	limit = 2000000u + 500000u * nsect;
	if (emmc_rd(EMMC_STATUS) & STATUS_BUSY) {
		return EBUSY;
	}

	emmc_wr(EMMC_INTMASK, 0);
	emmc_wr(EMMC_CTRL, (ctrl & ~(CTRL_INT_ENABLE | CTRL_DMA_ENABLE |
	    CTRL_USE_IDMAC)) | CTRL_FIFO_RESET);
	for (unsigned i = 0; i < 1000000; i++) {
		if (!(emmc_rd(EMMC_CTRL) & CTRL_FIFO_RESET)) {
			break;
		}
		if (i == 999999) {
			error = ETIMEDOUT;
			goto out;
		}
	}
	emmc_wr(EMMC_TMOUT, 0x573177ffu);
	emmc_wr(EMMC_BLKSIZ, 512);
	emmc_wr(EMMC_BYTCNT, nsect * 512u);
	emmc_wr(EMMC_RINTSTS, 0xffffffffu);
	emmc_wr(EMMC_CMDARG, lba);
	uint32_t command = CMD_START | CMD_WAIT_DATA | CMD_DATA | CMD_RESP_CRC |
	    CMD_RESP | CMD_SEND_STOP | 18u;
	if ((emmc_rd(EMMC_CLKSEL) >> 16) & 7u) {
		command |= CMD_HOLD;
	}
	emmc_wr(EMMC_CMD, command);
	for (unsigned i = 0; i < limit; i++) {
		uint32_t pending = emmc_rd(EMMC_RINTSTS);
		if (pending & INT_ERRORS) {
			printf("A5XELTE eMMC: CMD18 error lba=%u n=%u rint=%x status=%x words=%u/%u\n",
			    lba, nsect, pending, emmc_rd(EMMC_STATUS), read_words, words);
			error = EIO;
			goto out;
		}
		if (pending & INT_HTO) {
			emmc_wr(EMMC_RINTSTS, INT_HTO);
		}
		uint32_t fifo = (emmc_rd(EMMC_STATUS) >> 17) & 0x1fffu;
		while (fifo && read_words < words) {
			if (unit == 8) {
				uint32_t lo = emmc_rd(data_off);
				uint32_t hi = emmc_rd(data_off + 4);
				uint64_t v = (uint64_t)lo | ((uint64_t)hi << 32);
				memcpy(out + (size_t)read_words * 8u, &v, 8);
			} else if (unit == 4) {
				uint32_t v = emmc_rd(data_off);
				memcpy(out + (size_t)read_words * 4u, &v, 4);
			} else {
				uint16_t v = *(volatile uint16_t *)(emmc_regs + data_off);
				memcpy(out + (size_t)read_words * 2u, &v, 2);
			}
			read_words++;
			fifo--;
		}
		if (pending & INT_RXDR) {
			emmc_wr(EMMC_RINTSTS, INT_RXDR);
		}
		if (read_words == words && (pending & INT_DATA_OVER) && (pending & INT_CMD_DONE)) {
			/* The auto-stop CMD12 is still in flight (R1b): let it finish. */
			error = emmc_wait_not_busy();
			goto out;
		}
		if (i == limit - 1) {
			printf("A5XELTE eMMC: CMD18 timeout lba=%u n=%u rint=%x words=%u/%u\n",
			    lba, nsect, pending, read_words, words);
			error = ETIMEDOUT;
		}
	}
	if (!error && read_words != words) {
		error = ETIMEDOUT;
	}
out:
	emmc_wr(EMMC_RINTSTS, 0xffffffffu);
	emmc_wr(EMMC_INTMASK, ints);
	return error;
}

/* Read nsect (<= EMMC_MAX_RUN) consecutive sectors. Never worse than the single-sector path:
 * with emmcfast off, or after any CMD18 failure, it falls back to it sector by sector. */
static int
emmc_read_sectors(uint32_t lba, uint32_t nsect, uint8_t *out)
{
	int error;

	if (emmc_fast && nsect > 1) {
		for (unsigned attempt = 0; attempt < 2; attempt++) {
			error = emmc_read_sectors_once(lba, nsect, out);
			if (!error) {
				return 0;
			}
			emmc_abort_transfer();
		}
	}
	for (uint32_t i = 0; i < nsect; i++) {
		error = emmc_read_sector(lba + i, out + (size_t)i * 512u);
		if (error) {
			return error;
		}
	}
	return 0;
}

static int
emmc_write_sector(uint32_t lba, const uint8_t *sector)
{
	uint64_t data[64];
	uint32_t hcon = emmc_rd(EMMC_HCON);
	uint32_t width = (hcon >> 7) & 7u;
	uint32_t ver = emmc_rd(EMMC_VERID) & 0xffffu;
	uint32_t data_off = ver < 0x240au ? 0x100u : 0x200u;
	uint32_t unit, words, written = 0;
	uint32_t ctrl = emmc_rd(EMMC_CTRL);
	uint32_t ints = emmc_rd(EMMC_INTMASK);
	int error = 0;

	uint64_t address = lba;
	bool root_write = emmc_writable && address >= emmc_first_lba &&
	    address - emmc_first_lba < emmc_nsectors;
	bool log_write = emmc_log_enabled && address >= emmc_log_first_lba &&
	    address - emmc_log_first_lba < A5_LOG_SECTORS;
	if (!root_write && !log_write) {
		return EROFS;
	}
	if (width > 2 || ver == 0 || ver == 0xffff ||
	    !(emmc_rd(EMMC_PWREN) & 1u) || !(emmc_rd(EMMC_CLKENA) & 1u)) {
		return ENXIO;
	}
	unit = 2u << width;
	words = 512u / unit;
	if (emmc_rd(EMMC_STATUS) & (1u << 9)) {
		return EBUSY;
	}
	memcpy(data, sector, 512);
	emmc_wr(EMMC_INTMASK, 0);
	emmc_wr(EMMC_CTRL, (ctrl & ~(CTRL_INT_ENABLE | CTRL_DMA_ENABLE |
	    CTRL_USE_IDMAC)) | CTRL_FIFO_RESET);
	for (unsigned i = 0; i < 1000000; i++) {
		if (!(emmc_rd(EMMC_CTRL) & CTRL_FIFO_RESET)) {
			break;
		}
		if (i == 999999) {
			error = ETIMEDOUT;
			goto out;
		}
	}
	emmc_wr(EMMC_TMOUT, 0x573177ffu);
	emmc_wr(EMMC_BLKSIZ, 512);
	emmc_wr(EMMC_BYTCNT, 512);
	emmc_wr(EMMC_RINTSTS, 0xffffffffu);
	emmc_wr(EMMC_CMDARG, lba);
	uint32_t command = CMD_START | CMD_WAIT_DATA | CMD_DATA | CMD_WRITE |
	    CMD_RESP_CRC | CMD_RESP | 24u;
	if ((emmc_rd(EMMC_CLKSEL) >> 16) & 7u) {
		command |= CMD_HOLD;
	}
	emmc_wr(EMMC_CMD, command);
	for (unsigned i = 0; i < 4000000; i++) {
		uint32_t pending = emmc_rd(EMMC_RINTSTS);
		if (pending & INT_ERRORS) {
			printf("A5XELTE eMMC: CMD24 error lba=%u rint=%x status=%x words=%u/%u\n",
			    lba, pending, emmc_rd(EMMC_STATUS), written, words);
			error = EIO;
			goto out;
		}
		if (pending & INT_HTO) {
			emmc_wr(EMMC_RINTSTS, INT_HTO);
		}
		uint32_t fifo = (emmc_rd(EMMC_STATUS) >> 17) & 0x1fffu;
		while (fifo < EMMC_FIFO_DEPTH && written < words) {
			if (unit == 8) {
				/* Downstream CONFIG_MMC_DW_FORCE_32BIT_SFR_RW. */
				emmc_wr(data_off, (uint32_t)data[written]);
				emmc_wr(data_off + 4, (uint32_t)(data[written] >> 32));
			} else if (unit == 4) {
				emmc_wr(data_off, ((uint32_t *)data)[written]);
			} else {
				*(volatile uint16_t *)(emmc_regs + data_off) =
				    ((uint16_t *)data)[written];
			}
			written++;
			fifo++;
		}
		if (pending & INT_TXDR) {
			emmc_wr(EMMC_RINTSTS, INT_TXDR);
		}
		if (written == words && (pending & INT_DATA_OVER) &&
		    (pending & INT_CMD_DONE)) {
			break;
		}
		if (i == 3999999) {
			printf("A5XELTE eMMC: CMD24 timeout lba=%u rint=%x words=%u/%u tcbc=%u\n",
			    lba, pending, written, words, emmc_rd(EMMC_TCBCNT));
			error = ETIMEDOUT;
			goto out;
		}
	}
	for (unsigned i = 0; i < 4000000; i++) {
		if (!(emmc_rd(EMMC_STATUS) & (1u << 9))) {
			break;
		}
		if (i == 3999999) {
			error = ETIMEDOUT;
			printf("A5XELTE eMMC: CMD24 card busy after data lba=%u\n", lba);
		}
	}
out:
	emmc_wr(EMMC_RINTSTS, 0xffffffffu);
	emmc_wr(EMMC_INTMASK, ints);
	return error;
}

static uint32_t
emmc_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
emmc_le64(const uint8_t *p)
{
	return emmc_le32(p) | (uint64_t)emmc_le32(p + 4) << 32;
}

static uint32_t
emmc_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
		}
	}
	return crc;
}

static int
emmc_find_root_partition(void)
{
	uint8_t hdr[512], entry[512];
	(void)PE_parse_boot_argn("emmcroot", emmc_partition_name,
	    sizeof(emmc_partition_name));
	emmc_partition_name[sizeof(emmc_partition_name) - 1] = '\0';
	if (strcmp(emmc_partition_name, "SYSTEM") &&
	    strcmp(emmc_partition_name, "USERDATA")) {
		printf("A5XELTE eMMC: emmcroot must be SYSTEM or USERDATA\n");
		return EINVAL;
	}
	emmc_first_lba = 0;
	emmc_nsectors = 0;
	emmc_log_first_lba = 0;
	emmc_log_nsectors = 0;
	int error = emmc_read_sector(1, hdr);
	if (error) {
		return error;
	}
	uint32_t header_size = emmc_le32(hdr + 12);
	if (memcmp(hdr, "EFI PART", 8) ||
	    (header_size != 92 && header_size != 512) ||
	    emmc_le64(hdr + 24) != 1) {
		printf("A5XELTE eMMC: invalid GPT header\n");
		return ENXIO;
	}
	uint32_t expected_header_crc = emmc_le32(hdr + 16);
	memset(hdr + 16, 0, 4);
	if ((emmc_crc32(~0u, hdr, header_size) ^ ~0u) != expected_header_crc) {
		printf("A5XELTE eMMC: GPT header CRC mismatch\n");
		return EIO;
	}
	uint64_t first_usable = emmc_le64(hdr + 40);
	uint64_t last_usable = emmc_le64(hdr + 48);
	uint64_t entry_lba = emmc_le64(hdr + 72);
	uint32_t nentries = emmc_le32(hdr + 80);
	uint32_t entry_size = emmc_le32(hdr + 84);
	if (entry_lba < 2 || entry_lba > 1024 || nentries == 0 ||
	    nentries > 128 || (nentries & 3u) || entry_size != 128 ||
	    first_usable > last_usable || last_usable > UINT32_MAX) {
		printf("A5XELTE eMMC: unsupported GPT layout\n");
		return ENXIO;
	}
	uint32_t crc = ~0u;
	for (uint32_t s = 0; s < nentries / 4; s++) {
		error = emmc_read_sector((uint32_t)entry_lba + s, entry);
		if (error) {
			return error;
		}
		crc = emmc_crc32(crc, entry, 512);
		for (uint32_t j = 0; j < 4; j++) {
			const uint8_t *p = entry + j * 128;
			bool match = true;
			bool system_match = true;
			static const char system_name[] = "SYSTEM";
			for (unsigned k = 0; k <= strlen(emmc_partition_name); k++) {
				if (p[56 + k * 2] != (uint8_t)emmc_partition_name[k] ||
				    p[57 + k * 2] != 0) {
					match = false;
					break;
				}
			}
			for (unsigned k = 0; k < sizeof(system_name); k++) {
				if (p[56 + k * 2] != (uint8_t)system_name[k] ||
				    p[57 + k * 2] != 0) {
					system_match = false;
					break;
				}
			}
			if (match || system_match) {
				uint64_t first = emmc_le64(p + 32);
				uint64_t last = emmc_le64(p + 40);
				if (first < first_usable || last > last_usable || first > last) {
					return EINVAL;
				}
				if (match) {
					emmc_first_lba = first;
					emmc_nsectors = last - first + 1;
				}
				if (system_match) {
					emmc_log_first_lba = first;
					emmc_log_nsectors = last - first + 1;
				}
			}
		}
	}
	if ((crc ^ ~0u) != emmc_le32(hdr + 88)) {
		printf("A5XELTE eMMC: GPT entries CRC mismatch\n");
		return EIO;
	}
	if (emmc_log_first_lba == emmc_first_lba && emmc_log_nsectors &&
	    strcmp(emmc_partition_name, "SYSTEM") == 0) {
		/* A log writer must never overlap the selected root. */
		emmc_log_nsectors = 0;
	}
	if (!emmc_nsectors) {
		printf("A5XELTE eMMC: %s partition not found\n", emmc_partition_name);
		return ENXIO;
	}
	int enable_write = 0;
	(void)PE_parse_boot_argn("emmcwrite", &enable_write, sizeof(enable_write));
	emmc_writable = !strcmp(emmc_partition_name, "USERDATA") && enable_write == 1;
	printf("A5XELTE eMMC: GPT %s first=%llu sectors=%llu %s\n",
	    emmc_partition_name, emmc_first_lba, emmc_nsectors,
	    emmc_writable ? "writable" : "read-only");
	return 0;
}

static int
emmc_map_host(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;
	if (emmc_regs) {
		return 0;
	}
	if (SecureDTLookupEntry(NULL, "/a5xelte-emmc", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
	    size < 2 * sizeof(uintptr_t) || reg[0] != 0x13540000u || reg[1] < 0x2000u) {
		printf("A5XELTE eMMC: missing or unexpected ADT register window\n");
		return ENXIO;
	}
	emmc_regs = ml_io_map(reg[0], reg[1]);
	if (!emmc_regs) {
		printf("A5XELTE eMMC: MMIO map failed\n");
		return ENOMEM;
	}
	printf("A5XELTE eMMC: host ctrl=%x status=%x clk=%x hcon=%x\n",
	    emmc_rd(EMMC_CTRL), emmc_rd(EMMC_STATUS),
	    emmc_rd(EMMC_CLKENA), emmc_rd(EMMC_HCON));
	return 0;
}

void
a5xelte_emmc_probe(void)
{
	uint32_t fast = 0;

	if (PE_parse_boot_argn("emmcfast", &fast, sizeof(fast)) && fast) {
		emmc_fast = true;
	}
	if (!emmc_map_host()) {
		(void)emmc_find_root_partition();
	}
}

static int
emmc_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	return !emmc_ready ? ENXIO : ((flags & FWRITE) && !emmc_writable ? EROFS : 0);
}

static int
emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static uint32_t emmc_secsize = 512;
static uint8_t emmc_bounce[512];

static void
emmc_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint32_t count = buf_count(bp);
	uint64_t bytes = emmc_nsectors * 512u;
	uint64_t off = 0;
	buf_setresid(bp, count);
	if (!emmc_ready) {
		error = ENXIO;
	} else if (!(buf_flags(bp) & B_READ) && !emmc_writable) {
		error = EROFS;
	} else if (buf_blkno(bp) < 0) {
		error = EINVAL;
	} else {
		off = (uint64_t)buf_blkno(bp) * emmc_secsize;
		if (off > bytes || count > bytes - off) {
			error = EINVAL;
		}
	}
	if (error || count == 0) {
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		error = EIO;
		goto done;
	}
	lck_mtx_lock(&a5xelte_emmc_lock);
	for (uint32_t n = 0; n < count;) {
		uint64_t pos = off + n;
		uint32_t skip = pos & 511u;
		uint32_t take = MIN(count - n, 512u - skip);
		uint32_t lba = (uint32_t)(emmc_first_lba + pos / 512u);
		if (emmc_fast && (buf_flags(bp) & B_READ) && skip == 0 && count - n >= 1024u) {
			uint32_t run = MIN((count - n) / 512u, EMMC_MAX_RUN);
			error = emmc_read_sectors(lba, run, (uint8_t *)vaddr + n);
			if (error) {
				break;
			}
			n += run * 512u;
			buf_setresid(bp, count - n);
			continue;
		}
		if (buf_flags(bp) & B_READ) {
			error = emmc_read_sector(lba, emmc_bounce);
			if (!error) {
				memcpy(vaddr + n, emmc_bounce + skip, take);
			}
		} else {
			if (skip || take != 512u) {
				error = emmc_read_sector(lba, emmc_bounce);
			}
			if (!error) {
				memcpy(emmc_bounce + skip, vaddr + n, take);
				error = emmc_write_sector(lba, emmc_bounce);
			}
		}
		if (error) {
			break;
		}
		n += take;
		buf_setresid(bp, count - n);
	}
	lck_mtx_unlock(&a5xelte_emmc_lock);
	buf_unmap(bp);
done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
emmc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = emmc_secsize;
		return 0;
	case DKIOCSETBLOCKSIZE: {
		uint32_t requested = *(uint32_t *)data;
		if (requested < 512 || requested > 4096 || (requested & (requested - 1))) {
			return EINVAL;
		}
		emmc_secsize = requested;
		return 0;
	}
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = emmc_nsectors * 512u / emmc_secsize;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = emmc_writable ? 1 : 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = 1;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = emmc_writable ? 1 : 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*(uint64_t *)data = 1;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE:
		return 0;
	default:
		return ENOTTY;
	}
}

static int
emmc_psize(__unused dev_t dev)
{
	return emmc_ready ? (int)MIN(emmc_nsectors * 512u / emmc_secsize, INT_MAX) : -1;
}

static int
emmc_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(emmc_strategy, NULL, dev, B_READ, minphys, uio, (int)emmc_secsize);
}

static int
emmc_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return emmc_writable ? physio(emmc_strategy, NULL, dev, B_WRITE,
	    minphys, uio, (int)emmc_secsize) : EROFS;
}

static const struct bdevsw emmc_bdevsw = {
	.d_open = emmc_open, .d_close = emmc_close, .d_strategy = emmc_strategy,
	.d_ioctl = emmc_ioctl, .d_dump = eno_dump, .d_psize = emmc_psize,
	.d_type = D_DISK,
};

static const struct cdevsw emmc_cdevsw = {
	.d_open = emmc_open, .d_close = emmc_close, .d_read = emmc_read,
	.d_write = emmc_write, .d_ioctl = emmc_ioctl, .d_stop = eno_stop,
	.d_reset = eno_reset, .d_ttys = NULL, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc, .d_type = D_DISK,
};

/* _cnputs may execute in any context. Keep this path free of locks and I/O. */
void a5xelte_log_append(const char *c, int size);
void
a5xelte_log_append(const char *c, int size)
{
	if (size <= 0) {
		return;
	}
	uint64_t at = os_atomic_add_orig(&emmc_log_total, (uint64_t)size, relaxed);
	for (int i = 0; i < size; i++) {
		emmc_log_ring[(at + (uint64_t)i) % A5_LOG_SIZE] = c[i];
	}
}

/* The console ring for other storage drivers (HEROLTE writes it to UFS). */
void a5xelte_log_snapshot(const char **ring, uint32_t *size, uint64_t *total);
void
a5xelte_log_snapshot(const char **ring, uint32_t *size, uint64_t *total)
{
	*ring = emmc_log_ring;
	*size = A5_LOG_SIZE;
	*total = os_atomic_load(&emmc_log_total, relaxed);
}

struct a5_log_header {
	char magic[8];
	uint32_t version, size;
	uint64_t total, seq;
};

static void
emmc_log_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t flushed = 0, seq = 0;
	uint8_t sector[512];
	for (;;) {
		(void)tsleep(&emmc_log_chan, PZERO, "a5log", hz * 10);
		uint64_t total = os_atomic_load(&emmc_log_total, relaxed);
		if (total == flushed) {
			continue;
		}
		uint64_t pos = total - flushed >= A5_LOG_SIZE ? total - A5_LOG_SIZE : flushed;
		int error = 0;
		lck_mtx_lock(&a5xelte_emmc_lock);
		while (pos < total && !error) {
			uint32_t block = (uint32_t)((pos % A5_LOG_SIZE) / 512u);
			uint64_t lba = emmc_log_first_lba + 1u + block;
			if (lba >= emmc_log_first_lba + A5_LOG_SECTORS || lba > UINT32_MAX) {
				error = EINVAL;
				break;
			}
			memcpy(sector, emmc_log_ring + (size_t)block * 512u, 512u);
			error = emmc_write_sector((uint32_t)lba, sector);
			pos += 512u - (pos & 511u);
		}
		if (!error) {
			struct a5_log_header h = {
				.magic = { 'A', '5', 'X', 'L', 'O', 'G', '1', '\0' },
				.version = 1, .size = A5_LOG_SIZE, .total = total, .seq = ++seq,
			};
			memset(sector, 0, sizeof(sector));
			memcpy(sector, &h, sizeof(h));
			error = emmc_write_sector((uint32_t)emmc_log_first_lba, sector);
		}
		lck_mtx_unlock(&a5xelte_emmc_lock);
		if (error) {
			printf("A5XELTE LOG: write failed (%d), stopping\n", error);
			return;
		}
		flushed = total;
	}
}

static void
emmc_log_start(void)
{
	int enabled = 0;
	thread_t thread;
	(void)PE_parse_boot_argn("a5log", &enabled, sizeof(enabled));
	if (enabled != 1 || !emmc_ready || !emmc_writable ||
	    strcmp(emmc_partition_name, "USERDATA") ||
	    emmc_log_nsectors < A5_LOG_SECTORS ||
	    emmc_log_first_lba == 0 ||
	    (emmc_log_first_lba < emmc_first_lba + emmc_nsectors &&
	    emmc_first_lba < emmc_log_first_lba + A5_LOG_SECTORS) ||
	    emmc_log_first_lba + A5_LOG_SECTORS - 1 > UINT32_MAX) {
		return;
	}
	emmc_log_enabled = true;
	printf("A5XELTE LOG: SYSTEM GPT first=%llu, %u sectors reserved\n",
	    emmc_log_first_lba, A5_LOG_SECTORS);
	if (kernel_thread_start(emmc_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		emmc_log_enabled = false;
	}
}

int
a5xelte_emmc_rootdev(dev_t *dev)
{
	if (!emmc_ready) {
		int error = emmc_map_host();
		if (error) {
			return error;
		}
		error = emmc_find_root_partition();
		if (error) {
			return error;
		}
		int bmajor = bdevsw_add(-1, &emmc_bdevsw);
		if (bmajor < 0) {
			return ENXIO;
		}
		int cmajor = cdevsw_add_with_bdev(-1, &emmc_cdevsw, bmajor);
		if (cmajor < 0) {
			return ENXIO;
		}
		emmc_bdev = makedev(bmajor, 0);
		devfs_make_node(emmc_bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmcroot0");
		devfs_make_node(makedev(cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmcroot0");
		emmc_ready = true;
		printf("A5XELTE eMMC: /dev/emmcroot0 ready, GPT %s %s\n",
		    emmc_partition_name, emmc_writable ? "writable" : "read-only");
		emmc_log_start();
	}
	*dev = emmc_bdev;
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_A5XELTE */
