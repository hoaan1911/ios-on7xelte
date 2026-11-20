/*
 * Copyright (c) 2022 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#ifndef _SYS_CODE_SIGNING_H_
#define _SYS_CODE_SIGNING_H_

#include <sys/cdefs.h>
__BEGIN_DECLS

typedef uint32_t code_signing_monitor_type_t;
enum {
	CS_MONITOR_TYPE_NONE = 0,
	CS_MONITOR_TYPE_PPL = 1,

};

typedef uint32_t code_signing_config_t;
enum {
	CS_CONFIG_UNRESTRICTED_DEBUGGING = (1 << 0),
	CS_CONFIG_ALLOW_ANY_SIGNATURE = (1 << 1),
	CS_CONFIG_ENFORCEMENT_DISABLED = (1 << 2),
	CS_CONFIG_GET_OUT_OF_MY_WAY = (1 << 3),
	CS_CONFIG_CSM_ENABLED = (1 << 31),
};


__END_DECLS
#endif /* _SYS_CODE_SIGNING_H_ */
