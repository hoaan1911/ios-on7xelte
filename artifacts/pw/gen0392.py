#!/usr/bin/env python3
"""Generate 0392/0393 from the post-0391 sources in artifacts/pw/p2/s2.

0392  SweetFramebuffer.cpp : stop painting the compiled-in photograph
0393  a5xelte_battery.c   : drive the gauge's HSI2C the way the verified PMIC path does
"""
import os, shutil, subprocess, sys

ROOT = "artifacts/pw"
FB = "iokit/Kernel/arm/SweetFramebuffer.cpp"
FG = "bsd/dev/a5xelte_battery.c"
CO = "Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>"


def load(path):
    return open(path).read()


def sub(s, old, new, why):
    if s.count(old) != 1:
        sys.exit("MATCH %d for %s\n%r" % (s.count(old), why, old[:200]))
    return s.replace(old, new, 1)


def cut(s, start_anchor, end_anchor, new, why):
    if s.count(start_anchor) != 1:
        sys.exit("AMBIGUOUS start for %s" % why)
    if s.count(end_anchor) != 1:
        sys.exit("AMBIGUOUS end for %s" % why)
    i = s.index(start_anchor)
    j = s.index(end_anchor, i) + len(end_anchor)
    return s[:i] + new + s[j:]


s2 = load(ROOT + "/p2/s2/" + FB)
g2 = load(ROOT + "/p2/s2/" + FG)

# ---------------------------------------------------------------- 0392
s3 = s2

s3 = sub(s3,
         '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
         '#include "j7wall_data.h"    /* tools/j7wall.py output: the picture below, RGB565 */\n'
         '#endif\n',
         '',
         "drop the picture header")

s3 = cut(s3,
         '/*\n * Expand the compiled-in picture to the logical display size.',
         '\t\t}\n\t}\n}\n\nstatic void\nsweet_fb_wall_load(int32_t w, int32_t h)',
         'static void\nsweet_fb_wall_load(int32_t w, int32_t h)',
         "drop the embedded picture")

s3 = sub(s3,
         '''	(void)PE_parse_boot_argn("j7wall", path, sizeof(path));
	if (size <= 0) {
		return;
	}
	sweet_fb_wall = (uint32_t *)IOMallocData((vm_size_t)size);
	if (sweet_fb_wall == NULL) {
		IOLog("A5XELTE FB: no memory for a wallpaper\\n");
		return;
	}
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0 || vp == NULL) {
		IOLog("A5XELTE FB: no wallpaper at %s, using the built-in picture\\n", path);
		sweet_fb_wall_embedded(sweet_fb_wall, w, h);
		sweet_fb_wall_w = w;
		sweet_fb_wall_h = h;
		return;
	}
	int error = vn_rdwr(UIO_READ, vp, (caddr_t)sweet_fb_wall, size, 0, UIO_SYSSPACE,
	    IO_NODELOCKED | IO_NOCACHE, kauth_cred_get(), NULL, NULL);
	vnode_close(vp, FREAD, ctx);
	if (error != 0) {
		IOLog("A5XELTE FB: wallpaper read %d, using the built-in picture\\n", error);
		sweet_fb_wall_embedded(sweet_fb_wall, w, h);
		sweet_fb_wall_w = w;
		sweet_fb_wall_h = h;
		return;
	}
''',
         '''	(void)PE_parse_boot_argn("j7wall", path, sizeof(path));
	if (size <= 0) {
		return;
	}
	/* No compiled-in picture any more.  It was one photograph at half resolution in
	 * RGB565, so its dark areas were not the wallpaper the desktop would have shown but
	 * holes in it, and the desktop is better off with its own background.  A file that is
	 * really there is still used, and j7wall=<path> still picks another one. */
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0 || vp == NULL) {
		IOLog("A5XELTE FB: no wallpaper at %s, keeping the plain background\\n", path);
		return;
	}
	sweet_fb_wall = (uint32_t *)IOMallocData((vm_size_t)size);
	if (sweet_fb_wall == NULL) {
		vnode_close(vp, FREAD, ctx);
		IOLog("A5XELTE FB: no memory for a wallpaper\\n");
		return;
	}
	int error = vn_rdwr(UIO_READ, vp, (caddr_t)sweet_fb_wall, size, 0, UIO_SYSSPACE,
	    IO_NODELOCKED | IO_NOCACHE, kauth_cred_get(), NULL, NULL);
	vnode_close(vp, FREAD, ctx);
	if (error != 0) {
		IOFreeData(sweet_fb_wall, (vm_size_t)size);
		sweet_fb_wall = NULL;
		IOLog("A5XELTE FB: wallpaper read %d, keeping the plain background\\n", error);
		return;
	}
''',
         "picture only from a file")

# ---------------------------------------------------------------- 0393
#
# The high-speed transfer below is the one that is device verified on this IP: the A5XELTE
# PMIC path (bsd/dev/a5xelte_touch.c) talks to the S2MPU03 through the same controller on
# the same SoC generation and gets answers.  That code resets the block, programs the byte
# count into the FIFO trigger the way the controller expects, services both FIFOs while the
# transfer runs, and waits for the stop phase to finish in TRANS_STATUS bit 17.
g3 = g2

g3 = sub(g3,
         '''#define HSI2C_CTL_FUNC_I2C  (1u << 0)''',
         '''#define HSI2C_CTL_SW_RST    (1u << 31)
#define HSI2C_FS_TX       0x60u
#define HSI2C_FS_RX       0x64u
#define HSI2C_HS_TX       0x68u
#define HSI2C_HS_RX       0x6cu
#define HSI2C_STOP_PENDING (1u << 17)   /* TRANS_STATUS: the stop phase has not finished */

#define HSI2C_CTL_FUNC_I2C  (1u << 0)''',
         "reset and stop bits")

g3 = cut(g3,
         '''/* Waits for one of `mask` in HSI2C_INT_STAT, bounded; leaves the status in *st. */''',
         '''static bool
fg_hsi2c_word(uint8_t reg, uint16_t *value)
{''',
         '''/*
 * Reset the block and program it the way the verified A5XELTE PMIC path does.  I2C_CTL
 * bit 31 is the software reset: write it with the function bits to assert and release it,
 * then write the plain function byte.  The timing registers keep Linux's 400 kHz values
 * for this controller, because the bootloader's divider is not readable as a rate here and
 * a reset leaves them at their reset values, not at the ones it programmed.
 */
static void
fg_hsi2c_init(void)
{
	fg_wr(HSI2C_CTL, HSI2C_CTL_SW_RST | HSI2C_CTL_FUNC_I2C | HSI2C_CTL_MASTER);
	fg_wr(HSI2C_CTL, HSI2C_CTL_FUNC_I2C | HSI2C_CTL_MASTER);
	fg_wr(HSI2C_TIMEOUT, 0);                /* Linux disables the hardware timeout */
	fg_wr(HSI2C_FS_TX, 0x78787800u);
	fg_wr(HSI2C_FS_RX, 0x3c007878u);
	fg_wr(HSI2C_HS_TX, 0x000000f0u);
	fg_wr(HSI2C_HS_RX, 0x0000003cu);
	fg_wr(HSI2C_FIFO_CTL, 0xfu);
	fg_wr(HSI2C_CONF, HSI2C_CONF_AUTO_MODE);
	fg_wr(HSI2C_INT_EN, 0);
	delay(100);
}

/*
 * One message in the controller's own auto mode.
 *
 * The previous version here put the transmit bytes in the FIFO before the run bit, then
 * waited for the completion bit and expected the receive bytes to still be in the FIFO.
 * It also never waited for the stop phase and never reset the block between probes, which
 * is why every bus after the first logged the same `ts=0x2000c`: that is bit 17, a stop
 * that never finished, left over from the message before.  Do it the way the PMIC path
 * does - start the run, then move bytes through the FIFOs as the controller drains and
 * fills them, clear each status bit as it arrives, and only return once the stop is done.
 */
static bool
fg_hsi2c_msg(bool receive, uint8_t *bytes, unsigned count)
{
	const uint32_t ctl = HSI2C_CTL_FUNC_I2C | HSI2C_CTL_MASTER |
	    (receive ? HSI2C_CTL_RXCHON : HSI2C_CTL_TXCHON);
	uint32_t last_irq = 0;
	unsigned done = 0;
	bool transfer_done = false;

	if (!count || count > 4) {
		return false;
	}
	fg_wr(HSI2C_INT_STAT, fg_rd(HSI2C_INT_STAT));   /* the status bits are write-one-to-clear */
	/* Trigger level 3 in the low nibble and the byte count in the half that belongs to the
	 * direction: the controller runs the FIFO, the driver only keeps it fed and drained. */
	fg_wr(HSI2C_FIFO_CTL, 3u | ((uint32_t)count << (receive ? 4 : 16)));
	/* I2C_ADDR holds the 7-bit target in bits [19:10] (HSI2C_SLV_ADDR_MAS). */
	fg_wr(HSI2C_ADDR, ((uint32_t)fg_addr & 0x3ffu) << 10);
	fg_wr(HSI2C_CTL, ctl);
	/* The controller's interrupt is not routed on this platform, so enable the status bits
	 * and service them here: 0xf80 is done/abort/no-dev/tx-empty/timeout, plus the fifo
	 * bits in use. */
	fg_wr(HSI2C_INT_EN, 0xf80u | (receive ? 0x42u : 1u));
	fg_wr(HSI2C_AUTO_CONF, (uint32_t)count | HSI2C_AUTO_STOP | HSI2C_AUTO_RUN |
	    (receive ? HSI2C_AUTO_READ_WRITE : 0u));
	for (unsigned us = 0; us < 20000; us++) {
		uint32_t fifo = fg_rd(HSI2C_FIFO_STAT);
		uint32_t irq = fg_rd(HSI2C_INT_STAT);

		if (irq != 0) {
			last_irq |= irq;
			fg_wr(HSI2C_INT_STAT, irq);
		}
		if (irq & 0xf00u) {             /* NO_DEV_ACK, NO_DEV, ABORT or TIMEOUT */
			goto out;
		}
		if (irq & HSI2C_INT_TRANS_DONE) {
			transfer_done = true;
		}
		if (receive) {
			while (done < count && ((fifo >> 16) & 0x7fu) != 0) {
				bytes[done++] = (uint8_t)fg_rd(HSI2C_RX_DATA);
				fifo = fg_rd(HSI2C_FIFO_STAT);
			}
		} else {
			while (done < count && (fifo & (1u << 7)) == 0) {
				fg_wr(HSI2C_TX_DATA, bytes[done++]);
				fifo = fg_rd(HSI2C_FIFO_STAT);
			}
		}
		if (done == count && transfer_done) {
			fg_wr(HSI2C_INT_EN, 0);
			for (unsigned n = 0; n < 20000; n++) {
				if ((fg_rd(HSI2C_TRANS_STAT) & HSI2C_STOP_PENDING) == 0) {
					fg_wr(HSI2C_CTL, ctl & ~(HSI2C_CTL_RXCHON | HSI2C_CTL_TXCHON));
					return true;
				}
				delay(1);
			}
			last_irq |= HSI2C_STOP_PENDING;
			goto out;
		}
		delay(1);
	}
out:
	fg_wr(HSI2C_INT_EN, 0);
	/* Leave a clean block behind: without the reset the next bus inherits this stop. */
	fg_hsi2c_init();
	if (++fg_errors <= 6) {
		printf("A5XELTE FG: hsi2c %s of %u byte(s) failed irq=%#x ts=%#x fifo=%#x\\n",
		    receive ? "read" : "write", count, last_irq | (uint32_t)fg_rd(HSI2C_INT_STAT),
		    fg_rd(HSI2C_TRANS_STAT), fg_rd(HSI2C_FIFO_STAT));
	}
	return false;
}

static bool
fg_hsi2c_word(uint8_t reg, uint16_t *value)
{''',
         "verified hsi2c transfer")

g3 = sub(g3,
         '\tstatic const uint8_t addrs[] = { 0x3bu, 0x33u, 0x0du };',
         '\tstatic const uint8_t addrs[] = { 0x3bu, 0x36u, 0x33u };',
         "gauge addresses")

g3 = sub(g3,
         '''	/* Samsung's tree puts the gauge on I2C4 at 0x3b, and this phone answers on none of the
	 * six controllers at that address.  The charger/FG part is an S2MU005 block whose sibling
	 * PMIC answers as 0x66, so ask the three addresses it can have, on the bus Samsung
	 * documents first and then on the rest.  The first pair that ACKs is kept. */''',
         '''	/* Samsung's tree puts the S2MU004 gauge on I2C4 at 0x3b; the MAX77854 part that the
	 * S7 uses keeps its gauge at 0x36 next to the PMIC at 0x66, and both parts appear on
	 * Exynos 7870 boards.  Ask every address a fuel gauge can have on every controller,
	 * the bus Samsung documents first, and keep the first pair that ACKs. */''',
         "scan comment")

# ---------------------------------------------------------------- emit
os.makedirs(ROOT + "/p3", exist_ok=True)
open(ROOT + "/p3/" + FB.replace("/", "_"), "w").write(s3)
open(ROOT + "/p3/" + FG.replace("/", "_"), "w").write(g3)


def diff(old, new, path):
    d = subprocess.run(["diff", "-u", "--label", "a/" + path, "--label", "b/" + path, old, new],
                       capture_output=True, text=True)
    if d.returncode != 1:
        sys.exit("diff %s -> %d %s" % (path, d.returncode, d.stderr))
    return d.stdout


def patch(name, subject, body, hunks):
    out = "From: j7\nSubject: %s\n\n%s\n\n%s\n\n%s" % (subject, body, CO, "".join(hunks))
    open("xnu/patches/%s.patch" % name, "w").write(out)
    print("wrote xnu/patches/%s.patch (%d lines)" % (name, out.count("\n")))


def stage(path, text):
    f = ROOT + "/p3/s0_" + path.replace("/", "_")
    open(f, "w").write(text)
    return f


f0 = stage(FB, s2)
g0 = stage(FG, g2)
f1 = stage(FB, s3) if False else ROOT + "/p3/" + FB.replace("/", "_")
g1 = ROOT + "/p3/" + FG.replace("/", "_")

patch("0392-j7-drop-the-compiled-in-picture-from-the-desktop",
      "j7: drop the compiled-in picture from the desktop",
      "The photograph painted, but every dark part of it was a hole: the desktop is painted on\n"
      "top of it, and the tile seen on the phone had black blocks wherever the picture was\n"
      "dark.  Stop shipping the picture inside the kernel - one half-size RGB565 photograph is\n"
      "not a wallpaper anyway - and paint nothing where the client painted nothing, so the\n"
      "desktop keeps one flat, graded background.\n"
      "\n"
      "A raw picture is still read from /var/mobile/j7wall.raw (or j7wall=<path>) when that\n"
      "file exists, and that path now opens the file before allocating the buffer and frees\n"
      "the buffer when the read fails, instead of leaving four megabytes behind and painting\n"
      "half of it.",
      [diff(f0, f1, FB)])

patch("0393-j7-drive-the-gauge-the-way-the-verified-pmic-path-does",
      "j7: drive the gauge the way the verified PMIC path does",
      "Every high-speed bus still logged nothing, and the last three logged the same\n"
      "`ts=0x2000c` - bit 17 of I2C_TRANS_STATUS, a stop phase that never finished.  That value\n"
      "was inherited: this code never waited for a stop and never reset the block, so one\n"
      "failed message left every bus that followed in the same state.\n"
      "\n"
      "The same controller IP is device verified on this SoC generation in the A5XELTE PMIC\n"
      "path, which reads the S2MPU03 through it.  Do what that code does: reset the block with\n"
      "I2C_CTL bit 31, program Linux's timing registers, put the byte count in the FIFO\n"
      "trigger the controller expects, service the transmit and receive FIFOs while the\n"
      "transfer runs instead of around it, poll INT_STATUS and clear each bit as it arrives,\n"
      "and do not report success until the stop phase has cleared.  A failed message now\n"
      "resets the block, so the next bus starts clean.\n"
      "\n"
      "The scan also asks 0x36, the MAX77854 gauge address the Exynos 7870 boards use next to\n"
      "the PMIC at 0x66, instead of 0x0d, which is not a fuel gauge on any Samsung board.",
      [diff(g0, g1, FG)])

print("stage sizes:", len(s2), len(s3), len(g2), len(g3))
