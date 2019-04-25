// SPDX-License-Identifier: GPL-2.0
/*
 * mm/pcpu-vm.c - vm areas for percpu access
 *
 * Copyright (C) 2019 Gao Xiang <gaoxiang25@huawei.com>
 */
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <linux/pcpu-vm.h>

struct pcpu_vm_set {
	struct list_head list;

	/* mainly used to protect nrpages atomicly for all cpus */
	rwlock_t lock;
	unsigned int nrpages;

	struct pcpu_vm_area *pcpu;
};

struct pcpu_vm_area {
	struct pcpu_vm_set *set;

	/* vm area for mapping object that span pages */
	struct vm_struct *vm;

	/* # of dirty pages */
	unsigned int nrdirties;
};

static LIST_HEAD(pcpu_vm_sets);
static DECLARE_RWSEM(pcpu_vm_rwsem);
static enum cpuhp_state hp_online;

unsigned int lock_pcpu_vm_area(struct pcpu_vm_set *set,
			       unsigned int pageneeded)
{
	struct pcpu_vm_area *area;

	read_lock(&set->lock);
	preempt_disable();
	pagefault_disable();

	area = this_cpu_ptr(set->pcpu);
	if (pageneeded + area->nrdirties > set->nrpages) {
		unmap_kernel_range_local((unsigned long)area->vm->addr,
					 area->nrdirties * PAGE_SIZE);
		area->nrdirties = 0;		
	}
	return set->nrpages - area->nrdirties;
}
EXPORT_SYMBOL_GPL(lock_pcpu_vm_area);

void unlock_pcpu_vm_area(struct pcpu_vm_set *set)
{
	pagefault_enable();
	preempt_enable();
	read_unlock(&set->lock);
}
EXPORT_SYMBOL_GPL(unlock_pcpu_vm_area);

void *map_pcpu_vm_area(struct pcpu_vm_set *set, struct page **pages,
		       unsigned int nrpages)
{
	struct pcpu_vm_area *area;
	unsigned long addr;

	/* set->nrpages should not be changed since vm_area_set is locked */
	if (nrpages > set->nrpages)
		return ERR_PTR(-EINVAL);

	BUG_ON(nrpages > set->nrpages);
	area = get_cpu_ptr(set->pcpu);

	addr = (unsigned long)area->vm->addr;

	/* no enough room to map these pages, flush TLB emergency */
	if (area->nrdirties + nrpages > set->nrpages) {
		unmap_kernel_range(addr, area->nrdirties * PAGE_SIZE);
		area->nrdirties = 0;
	}

	/* map_vm_area cannot be used to map the vm area partially */
	addr += area->nrdirties * PAGE_SIZE;
	map_kernel_range_noflush(addr, nrpages * PAGE_SIZE,
				 PAGE_KERNEL, pages);
	flush_cache_vmap(addr, addr + nrpages * PAGE_SIZE);
	area->nrdirties += nrpages;

	put_cpu_ptr(set->pcpu);
	return (void *)addr;
}
EXPORT_SYMBOL_GPL(map_pcpu_vm_area);

static void __free_pcpu_vm_area(struct pcpu_vm_area *pcpu)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct pcpu_vm_area *area = per_cpu_ptr(pcpu, cpu);

		if (!area->vm)
			continue;
		free_vm_area(area->vm);
		area->vm = NULL;
	}
	free_percpu(pcpu);
}

struct pcpu_vm_set *register_pcpu_vm_area(unsigned int nrpages)
{
	struct pcpu_vm_set *set;
	struct pcpu_vm_area *pcpu;
	int cpu;

	set = kmalloc(sizeof(*set), GFP_KERNEL);
	if (!set)
		goto err_out;

	pcpu = alloc_percpu(struct pcpu_vm_area);
	if (!pcpu)
		goto err_kfree;

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct pcpu_vm_area *area = per_cpu_ptr(pcpu, cpu);
		struct vm_struct *vm;

		vm = alloc_vm_area(nrpages * PAGE_SIZE, NULL);
		if (!vm) {
			__free_pcpu_vm_area(pcpu);
			cpus_read_unlock();
			goto err_kfree;
		}
		area->vm = vm;
	}

	set->pcpu = pcpu;
	set->nrpages = nrpages;
	rwlock_init(&set->lock);

	down_write(&pcpu_vm_rwsem);
	list_add(&set->list, &pcpu_vm_sets);
	up_write(&pcpu_vm_rwsem);
	cpus_read_unlock();
	return set;
err_kfree:
	kfree(set);
err_out:
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL_GPL(register_pcpu_vm_area);

int resize_pcpu_vm_area(struct pcpu_vm_set *set, unsigned nrpages)
{
	struct vm_struct *head = NULL;
	int cpu, err;

	err = 0;
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct vm_struct *cur;

		cur = alloc_vm_area(nrpages * PAGE_SIZE, NULL);
		if (!cur) {
			while (head) {
				cur = head;
				head = head->next;
				free_vm_area(cur);
			}
			err = -ENOMEM;
			break;
		}
		cur->next = head;
		head = cur;
	}
	if (!err) {
		/* lock_pcpu_vm_area cannot fail, let's replace all in atomic */
		write_lock(&set->lock);
		for_each_online_cpu(cpu) {
			struct pcpu_vm_area *area = per_cpu_ptr(set->pcpu, cpu);

			free_vm_area(area->vm);
			area->vm = head;
			area->nrdirties = 0;
			head = head->next;
		}
		write_unlock(&set->lock);
	}
	cpus_read_unlock();
	return err;
}
EXPORT_SYMBOL_GPL(resize_pcpu_vm_area);

void unregister_pcpu_vm_area(struct pcpu_vm_set *set)
{
	cpus_read_lock();

	down_write(&pcpu_vm_rwsem);
	list_del(&set->list);
	up_write(&pcpu_vm_rwsem);

	__free_pcpu_vm_area(set->pcpu);
	cpus_read_unlock();

	kfree(set);
}
EXPORT_SYMBOL_GPL(unregister_pcpu_vm_area);

static int pcpu_vm_cpu_prepare(unsigned int cpu)
{
	struct pcpu_vm_set *cur;
	int err = 0;

	down_write(&pcpu_vm_rwsem);
	list_for_each_entry(cur, &pcpu_vm_sets, list) {
		struct pcpu_vm_area *area = per_cpu_ptr(cur->pcpu, cpu);
		struct vm_struct *vm;

		BUG_ON(area->vm);
		vm = alloc_vm_area(cur->nrpages * PAGE_SIZE, NULL);
		if (!vm) {
			err = -ENOMEM;
			break;
		}
		area->vm = vm;
		area->nrdirties = 0;
	}
	up_write(&pcpu_vm_rwsem);
	return err;
}

static int pcpu_vm_cpu_dead(unsigned int cpu)
{
	struct pcpu_vm_set *cur;

	list_for_each_entry(cur, &pcpu_vm_sets, list) {
		struct pcpu_vm_area *area = per_cpu_ptr(cur->pcpu, cpu);
		struct vm_struct *vm = area->vm;

		if (!vm)
			continue;
		area->vm = NULL;
		area->nrdirties = 0;
		free_vm_area(vm);
	}
	return 0;
}

static int __init pcpu_vm_init(void)
{
	int err = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					    "mm/pcpu-vm:online",
					    pcpu_vm_cpu_prepare,
					    pcpu_vm_cpu_dead);
	if (err < 0)
		return err;

	hp_online = err;
	return 0;
}

static void __exit pcpu_vm_exit(void)
{
	cpuhp_remove_state(hp_online);
}

module_init(pcpu_vm_init);
module_exit(pcpu_vm_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gao Xiang <gaoxiang25@huawei.com");

