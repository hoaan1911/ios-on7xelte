#ifndef HT_IOKIT_H
#define HT_IOKIT_H
#include <stdint.h>
#include <stddef.h>
typedef int kern_return_t; typedef unsigned io_object_t; typedef io_object_t io_service_t, io_connect_t; typedef unsigned mach_port_t;
#define KERN_SUCCESS 0
#define kIOMainPortDefault 0
mach_port_t mach_task_self(void);
io_service_t IOServiceGetMatchingService(mach_port_t, void *); void *IOServiceMatching(const char *);
kern_return_t IOServiceOpen(io_service_t, mach_port_t, uint32_t, io_connect_t *); kern_return_t IOServiceClose(io_connect_t); kern_return_t IOObjectRelease(io_object_t);
kern_return_t IOConnectCallScalarMethod(io_connect_t, uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t *);
kern_return_t IOConnectCallStructMethod(io_connect_t, uint32_t, const void *, size_t, void *, size_t *);
#endif
