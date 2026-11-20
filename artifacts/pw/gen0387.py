#!/usr/bin/env python3
"""Build the new SweetFramebuffer.cpp for 0387 from the post-0386 base."""
import os, shutil, sys

BASE = "artifacts/pw/base/iokit/Kernel/arm/SweetFramebuffer.cpp"
NEWD = "artifacts/pw/new/iokit/Kernel/arm"
NEW = os.path.join(NEWD, "SweetFramebuffer.cpp")

os.makedirs(NEWD, exist_ok=True)
shutil.copy(BASE, NEW)
s = open(NEW).read()
orig = s


def sub(old, new, why):
    global s
    if s.count(old) != 1:
        sys.exit("MATCH %d for %s\n%r" % (s.count(old), why, old[:120]))
    s = s.replace(old, new, 1)


# --- 1. the sampler becomes a column table and a row pointer -------------------
old_at = '''/*
 * The picture is the logical display size and a swap record's layer is whatever the client says,
 * not always the same size: requiring an exact match meant no wallpaper was ever sampled (the
 * log said "took 0 background pixels" on a frame whose dominant colour was black).  Sample the
 * picture at the layer's pixel position instead, nearest neighbour.
 */
static inline uint32_t
sweet_fb_wall_at(int32_t c, int32_t r, int32_t w, int32_t h)
{
\tif (sweet_fb_wall == NULL || sweet_fb_wall_w <= 0 || sweet_fb_wall_h <= 0 || w <= 0 || h <= 0) {
\t\treturn 0;
\t}
\tint64_t sx = (int64_t)c * sweet_fb_wall_w / w;
\tint64_t sy = (int64_t)r * sweet_fb_wall_h / h;
\tif (sx < 0) {
\t\tsx = 0;
\t} else if (sx >= sweet_fb_wall_w) {
\t\tsx = sweet_fb_wall_w - 1;
\t}
\tif (sy < 0) {
\t\tsy = 0;
\t} else if (sy >= sweet_fb_wall_h) {
\t\tsy = sweet_fb_wall_h - 1;
\t}
\treturn sweet_fb_wall[(uint64_t)sy * (uint64_t)sweet_fb_wall_w + (uint64_t)sx];
}
'''
new_at = '''/*
 * The picture is the logical display size and a swap record's layer is whatever the client says,
 * not always the same size, so the picture is sampled at the layer's own pixel position, nearest
 * neighbour.  The row of the picture is the same for every pixel of a logical row and the column
 * is the same for every output pixel of a source column, so the two halves are split: the column
 * table is built once per present, and the caller keeps a pointer to the picture's row.
 */
static inline uint16_t
sweet_fb_wall_col(int32_t c, int32_t w)
{
\tif (sweet_fb_wall == NULL || sweet_fb_wall_w <= 0 || w <= 0) {
\t\treturn 0;
\t}
\tint64_t sx = (int64_t)c * sweet_fb_wall_w / w;
\tif (sx < 0) {
\t\tsx = 0;
\t} else if (sx >= sweet_fb_wall_w) {
\t\tsx = sweet_fb_wall_w - 1;
\t}
\treturn (uint16_t)sx;
}

static inline const uint32_t *
sweet_fb_wall_row(int32_t r, int32_t h)
{
\tif (sweet_fb_wall == NULL || sweet_fb_wall_w <= 0 || sweet_fb_wall_h <= 0 || h <= 0) {
\t\treturn NULL;
\t}
\tint64_t sy = (int64_t)r * sweet_fb_wall_h / h;
\tif (sy < 0) {
\t\tsy = 0;
\t} else if (sy >= sweet_fb_wall_h) {
\t\tsy = sweet_fb_wall_h - 1;
\t}
\treturn sweet_fb_wall + (uint64_t)sy * (uint64_t)sweet_fb_wall_w;
}
'''
sub(old_at, new_at, "wall sampler")

# --- 2. cacheable source row and the wallpaper column table --------------------
sub('''\tstatic uint16_t column[2048];
''',
'''\tstatic uint16_t column[2048];
\t/* The wallpaper column for each output pixel: same lifetime as column[]. */
\tstatic uint16_t wallx[2048];
\t/* The client's surface is not in the cache.  Reading a pixel straight out of it costs a bus
\t * transaction, and the row build measured 65 ms of a 106 ms frame - 45 ns per source pixel,
\t * which is one uncached read each.  Stage the row in cacheable memory once, one transaction
\t * per 64-byte line, and build every output pixel from there instead. */
\tstatic uint32_t srcrow[2048] __attribute__((aligned(64)));
''', "srcrow declarations")

# --- 3. bounds, and the wallpaper column table next to column[] ---------------
sub('''\tif (pw <= 0 || py1 <= py0 || pw > (int32_t)(sizeof(column) / sizeof(column[0]))) {
\t\treturn 0;
\t}
\tfor (int32_t x = 0; x < pw; x++) {
\t\tint32_t c = (px0 + x) * (int32_t)fLogicalW / (int32_t)fWidth - dx;
\t\tcolumn[x] = (uint16_t)(c < 0 ? 0 : (c >= w ? w - 1 : c));
\t}
''',
'''\tif (pw <= 0 || py1 <= py0 || pw > (int32_t)(sizeof(column) / sizeof(column[0])) ||
\t    w <= 0 || w > (int32_t)(sizeof(srcrow) / sizeof(srcrow[0]))) {
\t\treturn 0;
\t}
\tfor (int32_t x = 0; x < pw; x++) {
\t\tint32_t c = (px0 + x) * (int32_t)fLogicalW / (int32_t)fWidth - dx;
\t\tcolumn[x] = (uint16_t)(c < 0 ? 0 : (c >= w ? w - 1 : c));
\t\twallx[x] = sweet_fb_wall_col(column[x], w);
\t}
''', "column loop")

# --- 4. the row is read once, in cacheable memory ----------------------------
sub('''\t\tconst uint32_t *in = (const uint32_t *)(src + (uint64_t)r * srcRowBytes);
''',
'''\t\tconst uint32_t *in = (const uint32_t *)(src + (uint64_t)r * srcRowBytes);
\t\t/* One pass over the client's row, then everything reads the cached copy. */
\t\tmemcpy(srcrow, in, (size_t)w * 4);
\t\tin = srcrow;
''', "row staging")

# --- 5. paint every dark pixel, at the picture's own resolution ----------------
sub('''\t\t\t/* One source pixel becomes one or two output pixels: write the run, not the
\t\t\t * output pixel, so the source is read once per pixel instead of gathered. */
\t\t\tint32_t x = 0;
''',
'''\t\t\t/* The picture's row for this logical row, once per row. */
\t\t\tconst uint32_t *wrow = sweet_fb_wall_row(r, h);
\t\t\t/* One source pixel becomes one or two output pixels: write the run, not the
\t\t\t * output pixel, so the source is read once per pixel instead of gathered. */
\t\t\tint32_t x = 0;
''', "wrow")

sub('''\t\t\t\tif ((v >> 24) == 0xffu && isbg && (end - x) >= 32) {
\t\t\t\t\t/* A wallpaper area is hundreds of pixels across, a glyph stem is one
\t\t\t\t\t * or two, so wide dark runs are background. Paint the image there
\t\t\t\t\t * when one is loaded, the gradient when it is not. */
\t\t\t\t\tif (sweet_fb_wall != NULL) {
\t\t\t\t\t\tv = sweet_fb_wall_at(c, r, w, h);
\t\t\t\t\t\tsweet_fb_wall_px += (uint32_t)(end - x);
\t\t\t\t\t} else {
\t\t\t\t\t\tv = bg;
\t\t\t\t\t}
\t\t\t\t\tsweet_fb_alpha0 += (uint32_t)(end - x);
\t\t\t\t} else if (v == 0u && sweet_fb_wall != NULL) {
\t\t\t\t\t/* The client left this pixel transparent, so nothing else draws
\t\t\t\t\t * there: the picture goes in, and it counts. */
\t\t\t\t\tv = sweet_fb_wall_at(c, r, w, h);
\t\t\t\t\tsweet_fb_wall_px += (uint32_t)(end - x);
\t\t\t\t}
''',
'''\t\t\t\t/* Runs are at most two pixels wide, because one source pixel covers
\t\t\t\t * one or two output pixels at 1.44x, so the old "a background run is 32
\t\t\t\t * pixels" test never matched and no wallpaper was ever painted.  A dark
\t\t\t\t * opaque pixel is background; the wallpaper's pixel for it is a table
\t\t\t\t * lookup because the row and the column of the picture are fixed here. */
\t\t\t\tif ((v >> 24) == 0xffu && isbg) {
\t\t\t\t\tv = wrow != NULL ? wrow[wallx[x]] : bg;
\t\t\t\t\tif (wrow != NULL) {
\t\t\t\t\t\tsweet_fb_wall_px += (uint32_t)(end - x);
\t\t\t\t\t}
\t\t\t\t\tsweet_fb_alpha0 += (uint32_t)(end - x);
\t\t\t\t} else if (v == 0u && wrow != NULL) {
\t\t\t\t\t/* The client left this pixel transparent, so nothing else draws
\t\t\t\t\t * there: the picture goes in, and it counts. */
\t\t\t\t\tv = wrow[wallx[x]];
\t\t\t\t\tsweet_fb_wall_px += (uint32_t)(end - x);
\t\t\t\t}
''', "paint runs")

open(NEW, "w").write(s)
print("wrote", NEW, len(s) - len(orig), "bytes added")
