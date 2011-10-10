/* 
 * Copyright (C) 2006, Intel Corporation
 * 
 * This file is part of irqbalance
 *
 * This program file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program in a file named COPYING; if not, write to the 
 * Free Software Foundation, Inc., 
 * 51 Franklin Street, Fifth Floor, 
 * Boston, MA 02110-1301 USA
 */

/*
 * This file has the basic functions to manipulate interrupt metadata
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>

#include "types.h"
#include "irqbalance.h"



struct load_balance_info {
	unsigned long long int total_load;
	unsigned long long avg_load;
	int load_sources;
	unsigned long long int deviations;
	long double std_deviation;
};

static void gather_load_stats(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;

	info->total_load += obj->load;
	info->load_sources += 1;
}

static void compute_deviations(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;
	unsigned long long int deviation;

	deviation = (obj->load > info->avg_load) ?
		obj->load - info->avg_load :
		info->avg_load - obj->load;

	info->deviations += (deviation * deviation);
}

static void move_candidate_irqs(struct irq_info *info, void *data)
{
	int *remaining_deviation = (int *)data;

	/* never move an irq that has an afinity hint when 
 	 * hint_policy is HINT_POLICY_EXACT 
 	 */
	if (hint_policy == HINT_POLICY_EXACT)
		if (!cpus_empty(info->affinity_hint))
			return;

	/* Don't rebalance irqs that don't want it */
	if (info->level == BALANCE_NONE)
		return;

	/* Don't move cpus that only have one irq, regardless of load */
	if (g_list_length(info->assigned_obj->interrupts) <= 1)
		return;

	/* Stop rebalancing if we've estimated a full reduction of deviation */
	if (*remaining_deviation <= 0)
		return;

	*remaining_deviation -= info->load;

	migrate_irq(&info->assigned_obj->interrupts, &rebalance_irq_list, info);

	info->assigned_obj = NULL;
}

static void migrate_overloaded_irqs(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;
	int deviation;

	/*
 	 * Don't rebalance irqs on objects whos load is below the average
 	 */
	if (obj->load <= info->avg_load)
		return;

	deviation = obj->load - info->avg_load;

	if ((deviation > info->std_deviation) &&
	    (g_list_length(obj->interrupts) > 1)) {
		/*
 		 * We have a cpu that is overloaded and 
 		 * has irqs that can be moved to fix that
 		 */

		/* order the list from least to greatest workload */
		sort_irq_list(&obj->interrupts);
		/*
 		 * Each irq carries a weighted average amount of load
 		 * we think its responsible for.  Set deviation to be the load
 		 * of the difference between this objects load and the averate,
 		 * and migrate irqs until we only have one left, or until that
 		 * difference reaches zero
 		 */
		for_each_irq(obj->interrupts, move_candidate_irqs, &deviation);
	}

}

#define find_overloaded_objs(name, info) do {\
	int ___load_sources;\
	memset(&(info), 0, sizeof(struct load_balance_info));\
	for_each_##name(NULL, gather_load_stats, &(info));\
	(info).avg_load = (info).total_load / (info).load_sources;\
	for_each_##name(NULL, compute_deviations, &(info));\
	___load_sources = ((info).load_sources == 1) ? 1 : ((info).load_sources - 1);\
	(info).std_deviation = (long double)((info).deviations / ___load_sources);\
	(info).std_deviation = sqrt((info).std_deviation);\
	for_each_##name(NULL, migrate_overloaded_irqs, &(info));\
}while(0)

void update_migration_status(void)
{
	struct load_balance_info info;

	find_overloaded_objs(cpu_core, info);
	find_overloaded_objs(cache_domain, info);
	find_overloaded_objs(package, info);
	find_overloaded_objs(numa_node, info);
}


static void reset_irq_count(struct irq_info *info, void *unused __attribute__((unused)))
{
	info->last_irq_count = info->irq_count;
	info->irq_count = 0;
}

void reset_counts(void)
{
	for_each_irq(NULL, reset_irq_count, NULL);
}


static void dump_workload(struct irq_info *info, void *unused __attribute__((unused)))
{
	printf("Interrupt %i node_num %d (class %s) has workload %lu \n", info->irq, irq_numa_node(info)->number, classes[info->class], (unsigned long)info->load);
}

void dump_workloads(void)
{
	for_each_irq(NULL, dump_workload, NULL);
}

