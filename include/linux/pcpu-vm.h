// SPDX-License-Identifier: GPL-2.0
#ifndef LINUX_PCPU_VM_H
#define LINUX_PCPU_VM_H

#include <linux/mm.h>

struct pcpu_vm_set;

unsigned int lock_pcpu_vm_area(struct pcpu_vm_set *set,
			       unsigned int pageneeded);
void unlock_pcpu_vm_area(struct pcpu_vm_set *set);
void *map_pcpu_vm_area(struct pcpu_vm_set *set, struct page **pages,
		       unsigned int nrpages);

struct pcpu_vm_set *register_pcpu_vm_area(unsigned int nrpages);
int resize_pcpu_vm_area(struct pcpu_vm_set *set, unsigned nrpages);
void unregister_pcpu_vm_area(struct pcpu_vm_set *set);

#endif

