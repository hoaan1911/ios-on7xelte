#!/bin/bash
set -e
cd /workspace/chats/c57811c4ae693748619586f4

P=xnu/patches
DIFF="diff -u --label a/iokit/Kernel/arm/SweetFramebuffer.cpp --label b/iokit/Kernel/arm/SweetFramebuffer.cpp"

mk() {
  local out=$1 subject=$2 body=$3 old=$4 new=$5
  {
    printf 'From: j7\nSubject: %s\n\n' "$subject"
    printf '%s\n\n' "$body"
    printf 'Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>\n\n'
    $DIFF "$old" "$new" || true
  } > "$out"
  echo "$out $(wc -l < "$out") lines"
}

mk "$P/0384-j7-leave-the-cpu-clock-to-the-bootloader.patch" \
  "j7: leave the CPU clock to the bootloader, and say what it is" \
"The image with 0380 came up at 38 MHz. To reprogram the cluster PLL the cores have to be moved
off it, and the switch gate and divider this tree believed were at CMU_MIF +0x858/+0x458 are not
there: the boot logged \"CPUCL0 did not leave the PLL, left alone\", and the perf window then
reported cpu_mhz=38 - a number j7m_cpu_mhz() measures with a timing loop, not a constant - so a
present took 2.4 s and a touch waited half a minute. A machine at 38 MHz is far worse than the
902 MHz the bootloader leaves, so the clock tree is not written at all; j7_cpu_clock() only reads
PLL_CON0 and the ARMCLK dividers and logs the rate, and the retry at 1586 MHz goes with it." \
  artifacts/cur/SweetFramebuffer.cpp /tmp/gen/s1.cpp

mk "$P/0385-j7-latch-the-scanout-base-the-way-the-loader-does.patch" \
  "j7: latch the scanout base the way the verified loader does" \
"The panel was black from the first flip on. Window 0 is protected by SHADOWCON bit 8, and the
base written with that bit set stayed in the shadow register: the boot dump read ADD0 with the
new address and UPDATE=1 a second later, so the request was never consumed and the controller
kept scanning the buffer the console mute had already cleared. The loader that is verified on
this phone clears the bit first, writes the base, then requests the update; do the same, in one
helper used by both the first scanout and every flip. The first finished frame is also copied to
the loader's buffer once, so a controller that still refuses the new base shows a frame instead
of a black panel." \
  /tmp/gen/s1.cpp /tmp/gen/s2.cpp

mk "$P/0386-j7-size-the-wallpaper-to-the-layer-not-the-screen.patch" \
  "j7: sample the wallpaper at the layer's own size" \
"The wallpaper only replaced pixels when the swap layer was exactly the logical display size, and
on this phone no layer that carries the desktop is: the log reported a frame whose dominant colour
was black with \"took 0 background pixels\", which is what a size check that never matches looks
like. Sample the picture at the layer's own pixel position instead, nearest neighbour, for both
the near-black runs and the transparent pixels, so a wallpaper appears whatever rectangle the
client hands over." \
  /tmp/gen/s2.cpp /tmp/gen/s3.cpp
