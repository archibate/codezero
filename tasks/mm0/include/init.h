/*
 * Data that comes from the kernel, and other init data.
 *
 * Copyright (C) 2007 Bahadir Balban
 */
#ifndef __MM_INIT_H__
#define __MM_INIT_H__

#include <l4/macros.h>
#include <l4/config.h>
#include <l4/types.h>
#include <l4/generic/physmem.h>
#include INC_PLAT(offsets.h)
#include INC_GLUE(memory.h)
#include INC_GLUE(memlayout.h)
#include INC_ARCH(bootdesc.h)
#include <vm_area.h>

struct initdata {
	struct bootdesc *bootdesc;
	struct page_bitmap page_map;
};

extern struct initdata initdata;

int request_initdata(struct initdata *i);

void initialise(void);

#endif /* __MM_INIT_H__ */
