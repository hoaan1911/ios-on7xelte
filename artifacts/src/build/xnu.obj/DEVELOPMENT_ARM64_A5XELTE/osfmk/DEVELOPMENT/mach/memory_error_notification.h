#ifndef	_memory_error_notification_user_
#define	_memory_error_notification_user_

/* Module memory_error_notification */

#include <string.h>
#include <mach/ndr.h>
#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/notify.h>
#include <mach/mach_types.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <mach/port.h>
	
/* BEGIN MIG_STRNCPY_ZEROFILL CODE */

#if defined(__has_include)
#if __has_include(<mach/mig_strncpy_zerofill_support.h>)
#ifndef USING_MIG_STRNCPY_ZEROFILL
#define USING_MIG_STRNCPY_ZEROFILL
#endif
#ifndef __MIG_STRNCPY_ZEROFILL_FORWARD_TYPE_DECLS__
#define __MIG_STRNCPY_ZEROFILL_FORWARD_TYPE_DECLS__
#ifdef __cplusplus
extern "C" {
#endif
#ifndef __MIG_STRNCPY_ZEROFILL_FORWARD_TYPE_DECLS_CSTRING_ATTR
#define __MIG_STRNCPY_ZEROFILL_FORWARD_TYPE_DECLS_CSTRING_COUNTEDBY_ATTR(C) __unsafe_indexable
#endif
	extern int mig_strncpy_zerofill(char * dest, const char * src, int len) __attribute__((weak_import));
#ifdef __cplusplus
}
#endif
#endif /* __MIG_STRNCPY_ZEROFILL_FORWARD_TYPE_DECLS__ */
#endif /* __has_include(<mach/mig_strncpy_zerofill_support.h>) */
#endif /* __has_include */
	
/* END MIG_STRNCPY_ZEROFILL CODE */

#if	(__MigKernelSpecificCode) || (_MIG_KERNEL_SPECIFIC_CODE_)
#include <kern/ipc_mig.h>
#endif /* __MigKernelSpecificCode */

#ifdef AUTOTEST
#ifndef FUNCTION_PTR_T
#define FUNCTION_PTR_T
typedef void (*function_ptr_t)(mach_port_t, char *, mach_msg_type_number_t);
typedef struct {
        char            * name;
        function_ptr_t  function;
} function_table_entry;
typedef function_table_entry   *function_table_t;
#endif /* FUNCTION_PTR_T */
#endif /* AUTOTEST */

#ifndef	memory_error_notification_MSG_COUNT
#define	memory_error_notification_MSG_COUNT	2
#endif	/* memory_error_notification_MSG_COUNT */

#include <Availability.h>
#include <mach/std_types.h>
#include <mach/mig.h>
#include <mach/mig.h>
#include <mach/mach_types.h>
#include <kern/ecc.h>

#ifdef __BeforeMigUserHeader
__BeforeMigUserHeader
#endif /* __BeforeMigUserHeader */

#include <sys/cdefs.h>
__BEGIN_DECLS


/* SimpleRoutine memory_error_notification */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t memory_error_notification
(
	mach_port_t memory_error_port,
	uint64_t physical_address,
	uint32_t ecc_flags,
	uint32_t ce_count
);

/* SimpleRoutine mcc_memory_error_notification */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t mcc_memory_error_notification
(
	mach_port_t memory_error_port,
	mcc_ecc_event_t event
);

__END_DECLS

/********************** Caution **************************/
/* The following data types should be used to calculate  */
/* maximum message sizes only. The actual message may be */
/* smaller, and the position of the arguments within the */
/* message layout may vary from what is presented here.  */
/* For example, if any of the arguments are variable-    */
/* sized, and less than the maximum is sent, the data    */
/* will be packed tight in the actual message to reduce  */
/* the presence of holes.                                */
/********************** Caution **************************/

/* typedefs for all requests */

#ifndef __Request__memory_error_notification_subsystem__defined
#define __Request__memory_error_notification_subsystem__defined

#ifdef  __MigPackStructs
#pragma pack(push, 4)
#endif
	typedef struct {
		mach_msg_header_t Head;
		NDR_record_t NDR;
		uint64_t physical_address;
		uint32_t ecc_flags;
		uint32_t ce_count;
	} __Request__memory_error_notification_t __attribute__((unused));
#ifdef  __MigPackStructs
#pragma pack(pop)
#endif

#ifdef  __MigPackStructs
#pragma pack(push, 4)
#endif
	typedef struct {
		mach_msg_header_t Head;
		NDR_record_t NDR;
		mcc_ecc_event_t event;
	} __Request__mcc_memory_error_notification_t __attribute__((unused));
#ifdef  __MigPackStructs
#pragma pack(pop)
#endif
#endif /* !__Request__memory_error_notification_subsystem__defined */

/* union of all requests */

#ifndef __RequestUnion__memory_error_notification_subsystem__defined
#define __RequestUnion__memory_error_notification_subsystem__defined
union __RequestUnion__memory_error_notification_subsystem {
	__Request__memory_error_notification_t Request_memory_error_notification;
	__Request__mcc_memory_error_notification_t Request_mcc_memory_error_notification;
};
#endif /* !__RequestUnion__memory_error_notification_subsystem__defined */
/* typedefs for all replies */

#ifndef __Reply__memory_error_notification_subsystem__defined
#define __Reply__memory_error_notification_subsystem__defined

#ifdef  __MigPackStructs
#pragma pack(push, 4)
#endif
	typedef struct {
		mach_msg_header_t Head;
		NDR_record_t NDR;
		kern_return_t RetCode;
	} __Reply__memory_error_notification_t __attribute__((unused));
#ifdef  __MigPackStructs
#pragma pack(pop)
#endif

#ifdef  __MigPackStructs
#pragma pack(push, 4)
#endif
	typedef struct {
		mach_msg_header_t Head;
		NDR_record_t NDR;
		kern_return_t RetCode;
	} __Reply__mcc_memory_error_notification_t __attribute__((unused));
#ifdef  __MigPackStructs
#pragma pack(pop)
#endif
#endif /* !__Reply__memory_error_notification_subsystem__defined */

/* union of all replies */

#ifndef __ReplyUnion__memory_error_notification_subsystem__defined
#define __ReplyUnion__memory_error_notification_subsystem__defined
union __ReplyUnion__memory_error_notification_subsystem {
	__Reply__memory_error_notification_t Reply_memory_error_notification;
	__Reply__mcc_memory_error_notification_t Reply_mcc_memory_error_notification;
};
#endif /* !__RequestUnion__memory_error_notification_subsystem__defined */

#ifndef subsystem_to_name_map_memory_error_notification
#define subsystem_to_name_map_memory_error_notification \
    { "memory_error_notification", 5900 },\
    { "mcc_memory_error_notification", 5901 }
#endif

#ifdef __AfterMigUserHeader
__AfterMigUserHeader
#endif /* __AfterMigUserHeader */

#endif	 /* _memory_error_notification_user_ */
