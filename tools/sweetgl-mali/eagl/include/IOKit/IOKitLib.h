// SPDX-License-Identifier: MIT
// Galaxy S7 port: the few IOKit declarations HakiOS's src/eagl.c uses (display_waits()). The iOS SDK ships
// IOKit.framework without its headers.
#ifndef HERO_IOKITLIB_H
#define HERO_IOKITLIB_H
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_registry_entry_t;
typedef UInt32 IOOptionBits;

CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator,
                                          IOOptionBits options);
kern_return_t IOObjectRelease(io_object_t object);
#endif
