/*
 * Copyright (c) 2000-2017 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifndef _PTHREAD_PRIORITY_PRIVATE_H_
#define _PTHREAD_PRIORITY_PRIVATE_H_

#include <TargetConditionals.h>
#if TARGET_OS_SIMULATOR
#define PTHREAD_EXPOSE_LAYOUT 0
#else
#define PTHREAD_EXPOSE_LAYOUT 1
#endif /* TARGET_OS_SIMULATOR */

/*!
 * @typedef pthread_priority_t
 *
 * @abstract
 * pthread_priority_t is an on opaque integer that is guaranteed to be ordered
 * such that combations of QoS classes and relative priorities are ordered
 * numerically, according to their combined priority.
 *
 * <b>xnu, pthread & libdispatch flags</b>
 *
 * @const _PTHREAD_PRIORITY_OVERCOMMIT_FLAG
 * The thread this priority is applied to is overcommit (affects the workqueue
 * creation policy for this priority).
 *
 * @const _PTHREAD_PRIORITY_COOPERATIVE_FLAG
 * Used to convey that a thread is part of the cooperative pool. This is used
 * both outgoing form kernel and incoming into kernel
 *
 * @const _PTHREAD_PRIORITY_THREAD_TYPE_MASK
 * The set of bits that encode information about the thread type - whether it is
 * overcommit, non-overcommit or cooperative
 *
 * @const _PTHREAD_PRIORITY_FALLBACK_FLAG
 * Indicates that this priority is is used only when incoming events have no
 * priority at all. It is merely used as a fallback (hence the name) instead of
 * a floor.
 *
 * This is usually used with QOS_CLASS_DEFAULT and a 0 relative priority.
 *
 * @const _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG
 * The event manager flag indicates that this thread/request is for a event
 * manager thread.  There can only ever be one event manager thread at a time
 * and it is brought up at the highest of all event manager priorities pthread
 * knows about.
 *
 * @const _PTHREAD_PRIORITY_OVERRIDE_QOS_FLAG
 * This flag indicates that the bits extracted using
 * _PTHREAD_PRIORITY_QOS_CLASS_MASK represent { QoS override, req QoS } instead
 * of just req QoS. This is only currently only used as input to the kernel as
 * part of pthread_set_properties_self(). The override field here represents the
 * dispatch workqueue override.
 *
 * @const _PTHREAD_PRIORITY_SCHED_PRI_FLAG
 * @const _PTHREAD_PRIORITY_SCHED_PRI_MASK
 * This flag indicates that the bits extracted using
 * _PTHREAD_PRIORITY_SCHED_PRI_MASK represent a scheduler priority instead of
 * a {qos, relative priority} pair.
 *
 * This flag is used by the pthread kext to indicate to libdispatch that the
 * event manager queue priority is a scheduling priority and not a QoS.  When
 * the manager thread's priority is updated due to creation of pthread root
 * queues, libdispatch passed a pthread_priority_t in to kernel with this flag
 * to specify the new sched pri of manager. This flag is never used as an input
 * by anything else and is why it can perform a double duty with
 * _PTHREAD_PRIORITY_ROOTQUEUE_FLAG.
 *
 * @const _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG
 * This flag is used for the priority of event delivery threads to indicate
 * to libdispatch that this thread is bound to a kqueue.
 *
 * <b>dispatch only flags</b>
 *
 * @const _PTHREAD_PRIORITY_INHERIT_FLAG
 * This flag is meaningful to libdispatch only and has no meaning for the
 * kernel and/or pthread.
 *
 * @const _PTHREAD_PRIORITY_ROOTQUEUE_FLAG
 * This flag is meaningful to libdispatch only and has no meaning for the
 * kernel and/or pthread.
 *
 * @const _PTHREAD_PRIORITY_ENFORCE_FLAG
 * This flag is used to indicate that this priority should be prefered for work
 * submited asynchronously over the intrinsic priority of the queue/thread the
 * work is submitted to.
 *
 *
 * pthread_priority_t encoding - outgoing from kernel:
 *
 * Regular:
 *                flags                    req QoS class           Rel pri
 * |---------------------------------|--------------------|--------------------|
 *              22 - 31                      8-21                 0-7
 *
 * With _PTHREAD_PRIORITY_SCHED_PRI_FLAG:
 *
 *               flags                  unused         sched priority
 * |---------------------------------|----------|------------------------------|
 *              22 - 31                 16-21             0-15
 *
 * pthread_priority_t encoding - incoming to kernel via various syscalls:
 *
 * Regular:
 *
 *                flags                    req QoS class           Rel pri
 * |---------------------------------|--------------------|--------------------|
 *              22 - 31                      8-21                 0-7
 *
 * With _PTHREAD_PRIORITY_OVERRIDE_QOS_FLAG:
 *
 *              flags                  QoS ovr   QoS class       Rel pri
 * |---------------------------------|---------|----------|--------------------|
 *              22 - 31                 14-21      8-13            0-7
 *
 * With _PTHREAD_PRIORITY_SCHED_PRI_FLAG:
 *
 *               flags                  unused         sched priority
 * |---------------------------------|----------|------------------------------|
 *              22 - 31                 16-21             0-15
 */
typedef unsigned long pthread_priority_t;

#define _PTHREAD_PRIORITY_OVERCOMMIT_FLAG               0x80000000u
#define _PTHREAD_PRIORITY_INHERIT_FLAG                  0x40000000u /* dispatch only */
#define _PTHREAD_PRIORITY_ROOTQUEUE_FLAG                0x20000000u /* dispatch only */
#define _PTHREAD_PRIORITY_SCHED_PRI_FLAG                0x20000000u
#define _PTHREAD_PRIORITY_ENFORCE_FLAG                  0x10000000u /* dispatch only */
#define _PTHREAD_PRIORITY_FALLBACK_FLAG                 0x04000000u
#define _PTHREAD_PRIORITY_COOPERATIVE_FLAG              0x08000000u
#define _PTHREAD_PRIORITY_EVENT_MANAGER_FLAG            0x02000000u
#define _PTHREAD_PRIORITY_NEEDS_UNBIND_FLAG             0x01000000u
#define _PTHREAD_PRIORITY_DEFAULTQUEUE_FLAG             _PTHREAD_PRIORITY_FALLBACK_FLAG // compat
#define _PTHREAD_PRIORITY_OVERRIDE_QOS_FLAG             0x00800000u

#define _PTHREAD_PRIORITY_THREAD_TYPE_MASK              (_PTHREAD_PRIORITY_COOPERATIVE_FLAG | _PTHREAD_PRIORITY_OVERCOMMIT_FLAG)

#if PTHREAD_EXPOSE_LAYOUT || defined(__PTHREAD_EXPOSE_INTERNALS__)
// Masks for encoding of pthread priority
#define _PTHREAD_PRIORITY_FLAGS_MASK                    0xff000000u

#define _PTHREAD_PRIORITY_SCHED_PRI_MASK                0x0000ffffu

#define _PTHREAD_PRIORITY_QOS_CLASS_MASK                0x003fff00u
#define _PTHREAD_PRIORITY_QOS_CLASS_SHIFT               (8ull)
#define _PTHREAD_PRIORITY_VALID_QOS_CLASS_MASK          0x00003f00u
#define _PTHREAD_PRIORITY_VALID_OVERRIDE_QOS_MASK       0x003fc000u
#define _PTHREAD_PRIORITY_QOS_OVERRIDE_SHIFT            (14ull)

#define _PTHREAD_PRIORITY_PRIORITY_MASK                 0x000000ffu
#define _PTHREAD_PRIORITY_PRIORITY_SHIFT                (0)
#endif /* PTHREAD_EXPOSE_LAYOUT */

#endif // _PTHREAD_PRIORITY_PRIVATE_H_
