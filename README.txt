J7 Prime XNU boot-image packaging fix

Files in this overlay are intended to be copied over the matching paths in the repository:
- .github/workflows/j7-boot.yml
- shim/j7prime/xnu_boot.S
- shim/j7prime/xnu_boot.c
- tools/mkembeddedkernel.py (new)

The change moves the SWEETXNU payload out of Android's ramdisk field and embeds it in the ARM64
kernel image at offset 0x2000. The ramdisk field is then the small 0x77-byte placeholder already
used by tools/mkj7img.py. This targets the reported S-Boot error "Invalid RAMDISK LENG...".

This is an experimental packaging fix, not a confirmed boot fix. It assumes S-Boot loads the kernel
bytes described by the Android boot header and honors/does not reject the adjusted ARM64 Image
image_size. If the device still rejects it, do not flash repeatedly; capture the exact red error
and share the new artifact's component-size log. Do not write the recovery partition.
