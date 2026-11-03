// SPDX-License-Identifier: MIT
// J7 Prime port: CoreAnimation settings for backboardd when the bridge is in it. With a GLES context
// available, iOS 16's render server picks a deep, two-plane display format ('b3a8': 10-bit colour plus an
// alpha plane) that neither this GPU path nor the display driver writes, and the panel stays black. As
// Chlorophyll's backboardd injector does (ry-diffusion/Chlorophyll, inject/, MIT), deep and compressed
// framebuffers and IOSurfaces are turned off before QuartzCore reads its preferences, so the display
// stays 8-bit BGRA. From the Galaxy S7 port's hero_backboardd.c (ios-a5xelte-herolte).
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void sweet_backboardd_init(void) {
    if (strcmp(getprogname(), "backboardd") != 0) return;
    setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
    CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
}
