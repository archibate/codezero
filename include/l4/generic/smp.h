/*
 * Copyright 2010 B Labs Ltd.
 *
 * Author: Prem Mallappa <prem.mallappa@b-labs.co.uk>
 */

#ifndef __GENERIC_SMP_H__
#define __GENERIC_SMP_H__

#include INC_SUBARCH(cpu.h)

/* IPIs, we define more as we go */
/* we have limited IPI's on ARM, exactly 15 */
#define IPI_TLB_FLUSH	0x00000001
#define IPI_SCHEDULE	0x00000002
#define IPI_CACH_FLUSH	0x00000003

#if !defined (CONFIG_NCPU)
#define CONFIG_NCPU	1
#define smp_get_cpuid()		0
#endif

#endif	/* __GENERIC_SMP_H__ */
