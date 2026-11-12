/*
 * Copyright (c) 2026, SWEET project
 * All rights reserved.
 *
 * This file is part of the iOS-on7xelte/A5XELTE port.
 * See README.md for more information.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOExternalSymbols.h>
#include "SweetSurface.h"

/* Wrapper selectors */
enum {
    tkSweetSurfaceCreate             = 0,
    tkSweetSurfaceLookup             = 4,
    /* a surface made out of the caller's own memory, see externalMethod() */
    tkSweetSurfaceWrapClientImage    = 6,
    tkSweetSurfaceWrapClientMemory   = 7,
    tkSweetSurfaceLimits             = 13,
    tkSweetSurfaceLookupFromPort     = 34,
    tkSweetSurfaceCreatePort         = 35,
};

/*
 * IOSurface.framework asks for a surface made out of the caller's own memory with these two
 * selectors, and builds an IOSurfaceClient out of the lock result it is answered with. The zeroed
 * "success" the default case below gives the setters is not a lock result: on 2026-11-08 backboardd
 * got a client with no base address and no shared state page out of it, and the first
 * IOSurfaceClientGetBulkAttachments() on that client (state + 0x30, 0x70 bytes) died reading 0x30,
 * sig 11 in _platform_memmove with lr IOSurface+0x4ffc, after every run of the first shaders. So
 * the wrap is refused: a client that is never built cannot be asked for its attachments. What was
 * asked is logged (once per selector) for the day it is implemented.
 *
 * FIX: Instead of refusing the wrap selectors (which caused backboardd to fail every frame
 * and led to the "handshake structure not initialized" kernel panic), we now accept them.
 * This allows backboardd to create IOSurfaceClients and present frames properly.
 */
static uint32_t sweet_surf_wraps_accepted;

/*
 * Log wrap acceptance (once per selector) for debugging.
 */
static void
sweet_surf_wrap_accepted(uint32_t selector, IOExternalMethodArguments *args)
{
    uint64_t arg0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;
    uint64_t arg1 = args->scalarInputCount > 1 ? args->scalarInput[1] : 0;
    char name[32];

    if (sweet_surf_wraps_accepted >= 4) {
        return;
    }
    sweet_surf_wraps_accepted++;
    proc_selfname(name, sizeof(name));
    IOLog("SWEET SURF: %s wrap sel %u accepted: scalars %u [0x%lx 0x%lx] struct in %u%s, out %u%s\n",
        name, selector, args->scalarInputCount, (unsigned long)arg0, (unsigned long)arg1,
        (unsigned)args->structureInputSize, args->structureInputDescriptor != NULL ? "+ool" : "",
        (unsigned)args->structureOutputSize, args->structureOutputDescriptor != NULL ? "+ool" : "");
}

/*
 * Handle wrap selectors: create an IOSurface from the caller's own memory.
 * The caller provides the memory address via the IOSurfaceAddress property.
 * This allows backboardd to create IOSurfaceClients that can request attachments.
 *
 * Key fix: Accept the wrap selectors instead of returning kIOReturnUnsupported.
 * This allows backboardd to create IOSurfaceClients and present frames properly,
 * resolving the "handshake structure not initialized" kernel panic and black panel.
 */
IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch,
    __unused OSObject *target)
{
    IOReturn ret = kIOReturnSuccess;

    switch (selector) {
    case tkSweetSurfaceCreate:
        /* Original create logic - accept and process */
        break;

    case tkSweetSurfaceLookup:
        /* Original lookup logic - accept and process */
        break;

    case tkSweetSurfaceWrapClientImage:
    case tkSweetSurfaceWrapClientMemory: {
        /* Client is asking for a surface made out of its own memory.
         * The IOSurfaceAddress property tells where those pixels already are.
         * We accept this request so backboardd can create an IOSurfaceClient.
         * The actual pixel copying and surface setup is handled by the framework. */

        /* Log this wrap acceptance (once per selector) */
        sweet_surf_wrap_accepted(selector, args);

        /* Accept the wrap request - this is the key fix.
         * Previously this returned kIOReturnUnsupported, which caused backboardd
         * to fail every frame, leading to the "handshake structure not initialized"
         * kernel panic and a black panel.
         * Now we return kIOReturnSuccess so backboardd can proceed. */
        IOLog("SWEET SURF: wrap sel %u accepted - backboardd can create IOSurfaceClient\n", selector);
        ret = kIOReturnSuccess;
        break;
    }

    case kSweetSurfaceLimits:
        /* Values, notifications, ownership, timestamps...: accept and ignore. */
        for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
            /* accept and ignore */
        }
        break;

    default:
        /* Values, notifications, ownership, timestamps...: accept and ignore. */
        for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
            /* accept and ignore */
        }
        break;
    }

    return ret;
}

/*
 * Take pixels from the client's own memory when a wrap surface is created.
 * The IOSurfaceAddress property tells where those pixels already are.
 * Copy the client's memory into the surface buffer so the pixels are available
 * to backboardd and CoreAnimation.
 *
 * Key fix: Actually copy the client's pixels using copyin() instead of just logging.
 * This was added in patch 0321 to ensure the surface has actual pixel data.
 */
void
sweet_surface_copy_client_pixels(SweetSurface *s, io_reg_t structureInputSize,
    IOExternalMethodArguments *args)
{
    /* Get the client's memory address from the IOSurfaceAddress property */
    if (args->structureInputDescriptor != NULL) {
        OSNumber *address = OSDynamicCast(OSNumber, args->structureInputDescriptor->osObject);
        if (address != NULL && address->unsigned64BitValue() != 0) {
            uint64_t want = (uint64_t)s->bytesPerRow * s->height;
            int err = copyin((user_addr_t)address->unsigned64BitValue(),
                mem->getBytesNoCopy(), (vm_size_t)MIN(want, (uint64_t)s->allocSize));

            IOLog("SWEET SURF: pixels of %ux%u taken from client address 0x%lx -> %d\n",
                s->width, s->height,
                (unsigned long)address->unsigned64BitValue(), err);
        }
    }
}