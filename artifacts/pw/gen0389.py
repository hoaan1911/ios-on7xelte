#!/usr/bin/env python3
"""Generate 0389/0390/0391 from the post-0387 base files.

0389  SweetFramebuffer.cpp : two pixels per bus transaction, paint once per source pixel
0390  SweetFramebuffer.cpp : paint only after SpringBoard starts
      kern_exec.c         : set that flag when SpringBoard is executed
0391  a5xelte_battery.c   : 7-bit addresses, 5 ms completion wait, fifo before run
"""
import os, shutil, subprocess, sys

ROOT = "artifacts/pw"
FB = "iokit/Kernel/arm/SweetFramebuffer.cpp"
FG = "bsd/dev/a5xelte_battery.c"
KX = "kern_exec.c"
CO = "Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>"


def load(path):
    return open(path).read()


def sub(s, old, new, why):
    if s.count(old) != 1:
        sys.exit("MATCH %d for %s\n%r" % (s.count(old), why, old[:160]))
    return s.replace(old, new, 1)


def cut(s, start_anchor, end_anchor, new, why):
    i = s.index(start_anchor)
    j = s.index(end_anchor, i) + len(end_anchor)
    if s.count(start_anchor) != 1:
        sys.exit("AMBIGUOUS start for %s" % why)
    if s.count(end_anchor) != 1:
        sys.exit("AMBIGUOUS end for %s" % why)
    return s[:i] + new + s[j:]


# ---------------------------------------------------------------- stage 0
os.makedirs(ROOT + "/p2", exist_ok=True)
s0 = load(ROOT + "/new/" + FB)
k0 = load("artifacts/cur/" + KX)
g0 = load(ROOT + "/new/" + FG)

# ---------------------------------------------------------------- 0389
s1 = s0

s1 = sub(s1,
         "static uint64_t sweet_fb_build_ns, sweet_fb_store_ns;",
         "static uint64_t sweet_fb_build_ns, sweet_fb_store_ns, sweet_fb_src_ns;",
         "src timer")

s1 = sub(s1,
         '\t\t\tIOLog("A5XELTE FB: copy split build_us=%llu store_us=%llu\\n",\n'
         '\t\t\t    sweet_fb_build_ns / 1000, sweet_fb_store_ns / 1000);\n'
         '\t\t\tsweet_fb_build_ns = sweet_fb_store_ns = 0;',
         '\t\t\tIOLog("A5XELTE FB: copy split build_us=%llu src_us=%llu store_us=%llu\\n",\n'
         '\t\t\t    sweet_fb_build_ns / 1000, sweet_fb_src_ns / 1000, sweet_fb_store_ns / 1000);\n'
         '\t\t\tsweet_fb_build_ns = sweet_fb_src_ns = sweet_fb_store_ns = 0;',
         "copy split line")

# the wide row copy, next to the scale comment
s1 = sub(s1,
         "/*\n"
         " * sweetfb_scale: the logical rect (dx,dy,w,h) at src goes to panel pixels\n",
         "/*\n"
         " * The client's surface arrives cache-inhibited, where every 32-bit load is its own bus\n"
         " * transaction: one 3000-byte row cost 47 us, which was most of the frame.  Load two pixels\n"
         " * per transaction instead, four loads at a time so the controller can pipeline them, and\n"
         " * fall back to memcpy only for a row that is not 8-byte aligned.\n"
         " */\n"
         "static void\n"
         "sweet_fb_row_copy(uint32_t *dst, const uint32_t *src, int32_t n)\n"
         "{\n"
         "\tif ((((uintptr_t)src | (uintptr_t)dst) & 7u) != 0) {\n"
         "\t\tmemcpy(dst, src, (size_t)n * 4);\n"
         "\t\treturn;\n"
         "\t}\n"
         "\tconst uint64_t *s = (const uint64_t *)src;\n"
         "\tuint64_t *d = (uint64_t *)dst;\n"
         "\tconst int32_t pairs = n >> 1;\n"
         "\tint32_t i = 0;\n"
         "\tfor (; i + 3 < pairs; i += 4) {\n"
         "\t\tconst uint64_t a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];\n"
         "\t\td[i] = a;\n"
         "\t\td[i + 1] = b;\n"
         "\t\td[i + 2] = c;\n"
         "\t\td[i + 3] = e;\n"
         "\t}\n"
         "\tfor (; i < pairs; i++) {\n"
         "\t\td[i] = s[i];\n"
         "\t}\n"
         "\tif (n & 1) {\n"
         "\t\tdst[n - 1] = src[n - 1];\n"
         "\t}\n"
         "}\n"
         "\n"
         "/*\n"
         " * sweetfb_scale: the logical rect (dx,dy,w,h) at src goes to panel pixels\n",
         "row copy helper")

s1 = sub(s1,
         "\t/* The wallpaper column for each output pixel: same lifetime as column[]. */\n"
         "\tstatic uint16_t wallx[2048];",
         "\t/* The picture's column for each source pixel: same lifetime as column[]. */\n"
         "\tstatic uint16_t wallc[2048];",
         "wallc table")

s1 = sub(s1,
         "\tfor (int32_t x = 0; x < pw; x++) {\n"
         "\t\tint32_t c = (px0 + x) * (int32_t)fLogicalW / (int32_t)fWidth - dx;\n"
         "\t\tcolumn[x] = (uint16_t)(c < 0 ? 0 : (c >= w ? w - 1 : c));\n"
         "\t\twallx[x] = sweet_fb_wall_col(column[x], w);\n"
         "\t}",
         "\tfor (int32_t c = 0; c < w; c++) {\n"
         "\t\twallc[c] = sweet_fb_wall_col(c, w);\n"
         "\t}\n"
         "\tfor (int32_t x = 0; x < pw; x++) {\n"
         "\t\tint32_t c = (px0 + x) * (int32_t)fLogicalW / (int32_t)fWidth - dx;\n"
         "\t\tcolumn[x] = (uint16_t)(c < 0 ? 0 : (c >= w ? w - 1 : c));\n"
         "\t}",
         "column loop")

s1 = sub(s1,
         "\t\t/* One pass over the client's row, then everything reads the cached copy. */\n"
         "\t\tmemcpy(srcrow, in, (size_t)w * 4);\n"
         "\t\tin = srcrow;",
         "\t\t/* One pass over the client's row, two pixels per bus transaction. */\n"
         "\t\t{\n"
         "\t\t\tconst uint64_t copyStart = mach_absolute_time();\n"
         "\t\t\tsweet_fb_row_copy(srcrow, in, w);\n"
         "\t\t\tuint64_t ns = 0;\n"
         "\t\t\tabsolutetime_to_nanoseconds(mach_absolute_time() - copyStart, &ns);\n"
         "\t\t\tsweet_fb_src_ns += ns;\n"
         "\t\t}\n"
         "\t\tin = srcrow;",
         "row staging")

NEW_REGION = """\t\t\t/* The picture's row for this logical row, once per row. */
\t\t\tconst uint32_t *wrow = sweet_fb_wall_row(r, h);
\t\t\t/* Decide picture or background once per source pixel, not once per output
\t\t\t * pixel: at 1.44x one source pixel covers one or two output pixels, so this is
\t\t\t * 750 decisions a row instead of 1080 and every one of them reads the cached
\t\t\t * copy.  The expansion below then only copies a value. */
\t\t\tif (wrow != NULL) {
\t\t\t\tfor (int32_t c = 0; c < w; c++) {
\t\t\t\t\tconst uint32_t v = srcrow[c];
\t\t\t\t\tif (v == 0u) {
\t\t\t\t\t\t/* The client left this pixel transparent, so nothing
\t\t\t\t\t\t * else draws there: the picture goes in, and it counts. */
\t\t\t\t\t\tsrcrow[c] = wrow[wallc[c]];
\t\t\t\t\t\tsweet_fb_wall_px++;
\t\t\t\t\t\tcontinue;
\t\t\t\t\t}
\t\t\t\t\tif ((v >> 24) != 0xffu) {
\t\t\t\t\t\tcontinue;
\t\t\t\t\t}
\t\t\t\t\tconst uint32_t cr = (v >> 16) & 0xffu, cg = (v >> 8) & 0xffu;
\t\t\t\t\tconst uint32_t cb = v & 0xffu;
\t\t\t\t\tconst uint32_t cmax = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
\t\t\t\t\t/* 0x14 was too tight: the lock screen and the home screen paint
\t\t\t\t\t * 0xff1c1c1e and friends, which stayed on screen as black. */
\t\t\t\t\tbool isbg = cmax <= 0x28u;
\t\t\t\t\t/* The exact and near-black tests both found nothing on this
\t\t\t\t\t * phone: each screen paints its own dark colour.  Anything close
\t\t\t\t\t * to the frame's own dominant colour is background too. */
\t\t\t\t\tif (!isbg && sweet_fb_bg != 0u) {
\t\t\t\t\t\tconst int32_t dr = (int32_t)cr - (int32_t)((sweet_fb_bg >> 16) & 0xffu);
\t\t\t\t\t\tconst int32_t dg = (int32_t)cg - (int32_t)((sweet_fb_bg >> 8) & 0xffu);
\t\t\t\t\t\tconst int32_t db = (int32_t)cb - (int32_t)(sweet_fb_bg & 0xffu);
\t\t\t\t\t\tisbg = dr <= 12 && dr >= -12 && dg <= 12 && dg >= -12 &&
\t\t\t\t\t\t    db <= 12 && db >= -12;
\t\t\t\t\t}
\t\t\t\t\tif (isbg) {
\t\t\t\t\t\tsrcrow[c] = wrow[wallc[c]];
\t\t\t\t\t\tsweet_fb_wall_px++;
\t\t\t\t\t\tsweet_fb_alpha0++;
\t\t\t\t\t}
\t\t\t\t}
\t\t\t} else {
\t\t\t\t/* No picture at all: keep the plain gradient for a dark background. */
\t\t\t\tfor (int32_t c = 0; c < w; c++) {
\t\t\t\t\tconst uint32_t v = srcrow[c];
\t\t\t\t\tconst uint32_t cr = (v >> 16) & 0xffu, cg = (v >> 8) & 0xffu;
\t\t\t\t\tconst uint32_t cb = v & 0xffu;
\t\t\t\t\tconst uint32_t cmax = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
\t\t\t\t\tif ((v >> 24) == 0xffu && cmax <= 0x28u) {
\t\t\t\t\t\tsrcrow[c] = bg;
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t\t/* One source pixel becomes one or two output pixels: write the run, not the
\t\t\t * output pixel, so the source is read once per pixel instead of gathered. */
\t\t\tint32_t x = 0;
\t\t\twhile (x < pw) {
\t\t\t\tint32_t c = column[x];
\t\t\t\tint32_t end = x + 1;
\t\t\t\twhile (end < pw && column[end] == c) {
\t\t\t\t\tend++;
\t\t\t\t}
\t\t\t\tconst uint32_t v = in[c];
\t\t\t\tfor (; x < end; x++) {
\t\t\t\t\trow[x] = v;
\t\t\t\t}
\t\t\t}
"""

s1 = cut(s1,
         "\t\t\t/* The picture's row for this logical row, once per row. */\n"
         "\t\t\tconst uint32_t *wrow = sweet_fb_wall_row(r, h);\n",
         "\t\t\t\tfor (; x < end; x++) {\n"
         "\t\t\t\t\trow[x] = v;\n"
         "\t\t\t\t}\n"
         "\t\t\t}\n",
         NEW_REGION, "paint region")

# ---------------------------------------------------------------- 0390
s2 = sub(s1,
         "/* this frame's own background colour, 0 when it has none: see sweet_fb_dominant() */\n"
         "static uint32_t sweet_fb_bg;\n"
         "static int sweet_fb_bg_wanted;",
         "/* this frame's own background colour, 0 when it has none: see sweet_fb_dominant() */\n"
         "static uint32_t sweet_fb_bg;\n"
         "static int sweet_fb_bg_wanted;\n"
         "/* Set by the exec path when SpringBoard is executed (kern_exec.c).  Until then the panel\n"
         " * shows the boot logo drawn on black, and painting the picture under it looked like the\n"
         " * wallpaper arriving before the system did. */\n"
         "int sweet_fb_wall_ready;",
         "ready flag")

s2 = sub(s2,
         "\t\t\tif (wrow != NULL) {\n"
         "\t\t\t\tfor (int32_t c = 0; c < w; c++) {\n"
         "\t\t\t\t\tconst uint32_t v = srcrow[c];",
         "\t\t\tif (sweet_fb_wall_ready && wrow != NULL) {\n"
         "\t\t\t\tfor (int32_t c = 0; c < w; c++) {\n"
         "\t\t\t\t\tconst uint32_t v = srcrow[c];",
         "paint gate")

s2 = sub(s2,
         "\t\t\t} else {\n"
         "\t\t\t\t/* No picture at all: keep the plain gradient for a dark background. */",
         "\t\t\t} else if (wrow == NULL) {\n"
         "\t\t\t\t/* No picture at all: keep the plain gradient for a dark background.  The\n"
         "\t\t\t\t * boot logo screen has a picture but is not ready yet, and stays black. */",
         "gradient branch")

k1 = sub(k0,
         '#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
         '\t\t\t/* A short line keeps the persistent console ring useful. */\n'
         '\t\t\tprintf("exec %s\\n", p->p_name);\n',
         '#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
         '\t\t\t/* A short line keeps the persistent console ring useful. */\n'
         '\t\t\tprintf("exec %s\\n", p->p_name);\n'
         '#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n'
         '\t\t\t/* SpringBoard is the last step before the lock screen, and the boot logo\n'
         '\t\t\t * screen before it is drawn on black: let the framebuffer paint its\n'
         '\t\t\t * wallpaper from here on, so it does not appear under the Apple logo. */\n'
         '\t\t\tif (p->p_name != NULL && strcmp(p->p_name, "SpringBoard") == 0) {\n'
         '\t\t\t\textern int sweet_fb_wall_ready;\n'
         '\t\t\t\tsweet_fb_wall_ready = 1;\n'
         '\t\t\t}\n'
         '#endif\n',
         "springboard hook")

# ---------------------------------------------------------------- 0391
g1 = g0

g1 = sub(g1,
         " * The high-speed I2C blocks (0x13860000 and up) are a different controller from the\n",
         " * All addresses below are the 7-bit form the device tree uses; the S3C controller takes\n"
         " * (addr << 1) and the HSI2C block takes the address in bits [19:10].\n"
         " *\n"
         " * The high-speed I2C blocks (0x13860000 and up) are a different controller from the\n",
         "address note")

g1 = sub(g1,
         "\tfg_wr(FG_DS, (uint32_t)fg_addr | (receive ? 1u : 0u));",
         "\tfg_wr(FG_DS, ((uint32_t)fg_addr << 1) | (receive ? 1u : 0u));",
         "s3c address")

g1 = sub(g1,
         "\t/* The target address lives in the top half, the controller's own id in the bottom. */\n"
         "\tfg_wr(HSI2C_ADDR, ((uint32_t)fg_addr & 0x3feu) << 9);",
         "\t/* I2C_ADDR holds the 7-bit target in bits [19:10] (HSI2C_SLV_ADDR_MAS). */\n"
         "\tfg_wr(HSI2C_ADDR, ((uint32_t)fg_addr & 0x3ffu) << 10);",
         "hsi2c address")

g1 = sub(g1,
         "static bool\n"
         "fg_hsi2c_wait(uint32_t mask, uint32_t *st)\n"
         "{\n"
         "\tfor (unsigned n = 0; n < 100; n++) {",
         "/* The vendor driver waits 100 ms for a message to finish, and a 2-byte read at 100 kHz\n"
         " * takes about 300 us, so a 100 us bound gave up on transfers that were still running:\n"
         " * the scan reported istat=0 on every bus.  5 ms is enough for one message at either\n"
         " * the bootloader's clock or the slowest standard rate. */\n"
         "static bool\n"
         "fg_hsi2c_wait(uint32_t mask, uint32_t *st)\n"
         "{\n"
         "\tfor (unsigned n = 0; n < 5000; n++) {",
         "wait bound")

g1 = sub(g1,
         "\tfg_wr(HSI2C_AUTO_CONF, (uint32_t)count | HSI2C_AUTO_STOP | HSI2C_AUTO_RUN |\n"
         "\t    (receive ? HSI2C_AUTO_READ_WRITE : 0u));\n"
         "\tif (!receive) {\n"
         "\t\tfor (unsigned n = 0; n < count; n++) {\n"
         "\t\t\tfg_wr(HSI2C_TX_DATA, bytes[n]);\n"
         "\t\t}\n"
         "\t}",
         "\t/* The bytes go in before the run bit: a transmit FIFO that is empty when the\n"
         "\t * controller starts reads as an underrun and aborts the transfer. */\n"
         "\tif (!receive) {\n"
         "\t\tfor (unsigned n = 0; n < count; n++) {\n"
         "\t\t\tfg_wr(HSI2C_TX_DATA, bytes[n]);\n"
         "\t\t}\n"
         "\t}\n"
         "\tfg_wr(HSI2C_AUTO_CONF, (uint32_t)count | HSI2C_AUTO_STOP | HSI2C_AUTO_RUN |\n"
         "\t    (receive ? HSI2C_AUTO_READ_WRITE : 0u));",
         "fifo before run")

g1 = sub(g1,
         "out:\n"
         "\tfg_wr(HSI2C_INT_EN, 0);\n"
         "\tfg_wr(HSI2C_CTL, ctl);",
         "out:\n"
         "\tfg_wr(HSI2C_INT_EN, 0);\n"
         "\tif (!ok) {\n"
         "\t\tfg_wr(HSI2C_AUTO_CONF, 0);   /* drop a transfer that never completed */\n"
         "\t}\n"
         "\tfg_wr(HSI2C_CTL, ctl);",
         "clear run")

g1 = sub(g1,
         "\tstatic const uint8_t addrs[] = { 0x76u, 0x66u, 0x1bu };",
         "\tstatic const uint8_t addrs[] = { 0x3bu, 0x33u, 0x0du };",
         "address list")

g1 = sub(g1,
         "\t\t\tprintf(\"A5XELTE FG: nothing on 0x%08x (hsi2c), istat=%#x ctl=%#x\\n\",\n"
         "\t\t\t    buses[i].pa, fg_rd(HSI2C_INT_STAT), fg_rd(HSI2C_CTL));",
         "\t\t\tprintf(\"A5XELTE FG: nothing on 0x%08x (hsi2c), istat=%#x ts=%#x ctl=%#x\\n\",\n"
         "\t\t\t    buses[i].pa, fg_rd(HSI2C_INT_STAT), fg_rd(HSI2C_TRANS_STAT),\n"
         "\t\t\t    fg_rd(HSI2C_CTL));",
         "probe line")

# ---------------------------------------------------------------- write stages
os.makedirs(ROOT + "/p2/s1/iokit/Kernel/arm", exist_ok=True)
os.makedirs(ROOT + "/p2/s2/iokit/Kernel/arm", exist_ok=True)
os.makedirs(ROOT + "/p2/s2/bsd/dev", exist_ok=True)
open(ROOT + "/p2/s1/" + FB, "w").write(s1)
open(ROOT + "/p2/s2/" + FB, "w").write(s2)
open(ROOT + "/p2/s2/" + KX, "w").write(k1)
open(ROOT + "/p2/s2/" + FG, "w").write(g1)
open(ROOT + "/p2/s0_" + FB.replace("/", "_"), "w").write(s0)
open(ROOT + "/p2/s0_" + KX, "w").write(k0)
open(ROOT + "/p2/s0_" + FG.replace("/", "_"), "w").write(g0)


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


s0f = ROOT + "/p2/s0_" + FB.replace("/", "_")
k0f = ROOT + "/p2/s0_" + KX
g0f = ROOT + "/p2/s0_" + FG.replace("/", "_")

patch("0389-j7-two-pixels-per-bus-read-and-paint-per-source-pixel",
      "j7: two pixels per bus read, and paint once per source pixel",
      "The wallpaper paints now (1.4 M pixels in one frame) and the build is still 70 ms of a 78 ms\n"
      "present, so the cost is not the paint: the client's surface is cache-inhibited and a\n"
      "3000-byte row cost 47 us, which is one bus transaction per 32-bit load.  Load two pixels at a\n"
      "time, four loads in flight, and keep memcpy only for a row that is not 8-byte aligned.\n"
      "\n"
      "The picture decision also ran per output pixel with the picture's column looked up in a\n"
      "750-entry table per 1080-pixel row.  Do it once per source pixel in the staged row instead,\n"
      "where the column of the picture is a table built once per present, and let the expansion\n"
      "loop only copy values.  A new src_us in the copy split line says how much of build_us is the\n"
      "row read.",
      [diff(s0f, ROOT + "/p2/s1/" + FB, FB)])

patch("0390-j7-hold-the-wallpaper-until-springboard-runs",
      "j7: hold the wallpaper until SpringBoard runs",
      "The picture was painted on the boot logo screen, so the Apple appeared over the wallpaper\n"
      "instead of over black.  The loginwindow-side solution is not available here, so use the one\n"
      "the kernel already sees: set a flag when kern_exec.c executes SpringBoard, which is the last\n"
      "step before the lock screen, and paint only after that.  Before it the panel gets exactly\n"
      "what the boot screen drew, and nothing of the picture.",
      [diff(ROOT + "/p2/s1/" + FB, ROOT + "/p2/s2/" + FB, FB),
       diff(k0f, ROOT + "/p2/s2/" + KX, KX)])

patch("0391-j7-five-milliseconds-for-the-gauge-not-a-hundred-microseconds",
      "j7: five milliseconds for the gauge, not a hundred microseconds",
      "Every high-speed bus still logged nothing, and the transfer was never given a chance: the\n"
      "completion poll gave up after 100 us, while drivers/i2c/busses/i2c-exynos5.c waits 100 ms and\n"
      "a two-byte read at 100 kHz takes about 300 us.  Wait 5 ms, put the command bytes in the\n"
      "transmit FIFO before the run bit instead of after it, and drop I2C_AUTO_CONF when a message\n"
      "fails so the next one is not started from a half-run block.\n"
      "\n"
      "The addresses were also written in two inconsistent forms: the scan moved a byte-form address\n"
      "into I2C_ADDR (shifted by 9, which happens to land in the right field only for 0x76) and into\n"
      "IICDS unshifted (which is a different device on the S3C controller).  Keep every address in\n"
      "the 7-bit form the device tree uses and shift each controller's way.",
      [diff(g0f, ROOT + "/p2/s2/" + FG, FG)])

# ---------------------------------------------------------------- verify
print("stage sizes:", len(s0), len(s1), len(s2), len(k0), len(k1), len(g0), len(g1))
