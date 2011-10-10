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
 * This file contains the code to construct and manipulate a hierarchy of processors,
 * cache domains and processor cores.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

#include <glib.h>

#include "irqbalance.h"


GList *cpus;
GList *cache_domains;
GList *packages;

int package_count;
int cache_domain_count;
int core_count;

/* Users want to be able to keep interrupts away from some cpus; store these in a cpumask_t */
cpumask_t banned_cpus;

cpumask_t cpu_possible_map;

/* 
   it's convenient to have the complement of banned_cpus available so that 
   the AND operator can be used to mask out unwanted cpus
*/
static cpumask_t unbanned_cpus;

static struct topo_obj* add_cache_domain_to_package(struct topo_obj *cache, 
						    cpumask_t package_mask)
{
	GList *entry;
	struct topo_obj *package;
	struct topo_obj *lcache; 

	entry = g_list_first(packages);

	while (entry) {
		package = entry->data;
		if (cpus_equal(package_mask, package->mask))
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {
		package = calloc(sizeof(struct topo_obj), 1);
		if (!package)
			return NULL;
		package->mask = package_mask;
		packages = g_list_append(packages, package);
		package_count++;
	}

	entry = g_list_first(package->children);
	while (entry) {
		lcache = entry->data;
		if (lcache == cache)
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {
		package->children = g_list_append(package->children, cache);
		cache->parent = package;
	}

	return package;
}
static struct topo_obj* add_cpu_to_cache_domain(struct topo_obj *cpu,
						    cpumask_t cache_mask)
{
	GList *entry;
	struct topo_obj *cache;
	struct topo_obj *lcpu;

	entry = g_list_first(cache_domains);

	while (entry) {
		cache = entry->data;
		if (cpus_equal(cache_mask, cache->mask))
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {
		cache = calloc(sizeof(struct topo_obj), 1);
		if (!cache)
			return NULL;
		cache->mask = cache_mask;
		cache->number = cache_domain_count;
		cache_domains = g_list_append(cache_domains, cache);
		cache_domain_count++;
	}

	entry = g_list_first(cache->children);
	while (entry) {
		lcpu = entry->data;
		if (lcpu == cpu)
			break;
		entry = g_list_next(entry);
	}

	if (!entry) {
		cache->children = g_list_append(cache->children, cpu);
		cpu->parent = (struct topo_obj *)cache;
	}

	return cache;
}
 
static void do_one_cpu(char *path)
{
	struct topo_obj *cpu;
	FILE *file;
	char new_path[PATH_MAX];
	cpumask_t cache_mask, package_mask;
	struct topo_obj *cache;
	struct topo_obj *package;
	DIR *dir;
	struct dirent *entry;
	int nodeid;

	/* skip offline cpus */
	snprintf(new_path, PATH_MAX, "%s/online", path);
	file = fopen(new_path, "r");
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file)==0)
			return;
		fclose(file);
		if (line && line[0]=='0') {
			free(line);
			return;
		}
		free(line);
	}

	cpu = calloc(sizeof(struct topo_obj), 1);
	if (!cpu)
		return;

	cpu->number = strtoul(&path[27], NULL, 10);

	cpu_set(cpu->number, cpu_possible_map);
	
	cpu_set(cpu->number, cpu->mask);

	/* if the cpu is on the banned list, just don't add it */
	if (cpus_intersects(cpu->mask, banned_cpus)) {
		free(cpu);
		/* even though we don't use the cpu we do need to count it */
		core_count++;
		return;
	}


	/* try to read the package mask; if it doesn't exist assume solitary */
	snprintf(new_path, PATH_MAX, "%s/topology/core_siblings", path);
	file = fopen(new_path, "r");
	cpu_set(cpu->number, package_mask);
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file)) 
			cpumask_parse_user(line, strlen(line), package_mask);
		fclose(file);
		free(line);
	}

	/* try to read the cache mask; if it doesn't exist assume solitary */
	/* We want the deepest cache level available so try index1 first, then index2 */
	cpu_set(cpu->number, cache_mask);
	snprintf(new_path, PATH_MAX, "%s/cache/index1/shared_cpu_map", path);
	file = fopen(new_path, "r");
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file)) 
			cpumask_parse_user(line, strlen(line), cache_mask);
		fclose(file);
		free(line);
	}
	snprintf(new_path, PATH_MAX, "%s/cache/index2/shared_cpu_map", path);
	file = fopen(new_path, "r");
	if (file) {
		char *line = NULL;
		size_t size = 0;
		if (getline(&line, &size, file)) 
			cpumask_parse_user(line, strlen(line), cache_mask);
		fclose(file);
		free(line);
	}

	nodeid=0;
	dir = opendir(path);
	do {
		entry = readdir(dir);
		if (!entry)
			break;
		if (strstr(entry->d_name, "node")) {
			nodeid = strtoul(&entry->d_name[4], NULL, 10);
			break;
		}
	} while (entry);
	closedir(dir);

	cache = add_cpu_to_cache_domain(cpu, cache_mask);
	package = add_cache_domain_to_package(cache, package_mask);
	add_package_to_node(package, nodeid);	
 
	/* 
	   blank out the banned cpus from the various masks so that interrupts
	   will never be told to go there
	 */
	cpus_and(cpu_cache_domain(cpu)->mask, cpu_cache_domain(cpu)->mask, unbanned_cpus);
	cpus_and(cpu_package(cpu)->mask, cpu_package(cpu)->mask, unbanned_cpus);
	cpus_and(cpu->mask, cpu->mask, unbanned_cpus);

	cpus = g_list_append(cpus, cpu);
	core_count++;
}

static void dump_irq(struct irq_info *info, void *data)
{
	int spaces = (long int)data;
	int i;
	for (i=0; i<spaces; i++) printf(" ");
	printf("Interrupt %i node_num is %d (%s/%u) \n", info->irq, irq_numa_node(info)->number, classes[info->class], (unsigned int)info->load);
}

static void dump_topo_obj(struct topo_obj *d, void *data __attribute__((unused)))
{
	struct topo_obj *c = (struct topo_obj *)d;
	printf("                CPU number %i  numa_node is %d (load %lu)\n", c->number, cpu_numa_node(c)->number , (unsigned long)c->load);
	if (c->interrupts)
		for_each_irq(c->interrupts, dump_irq, (void *)18);
}

static void dump_cache_domain(struct topo_obj *d, void *data)
{
	char *buffer = data;
	cpumask_scnprintf(buffer, 4095, d->mask);
	printf("        Cache domain %i:  numa_node is %d cpu mask is %s  (load %lu) \n", d->number, cache_domain_numa_node(d)->number, buffer, (unsigned long)d->load);
	if (d->children)
		for_each_cpu_core(d->children, dump_topo_obj, NULL);
	if (d->interrupts)
		for_each_irq(d->interrupts, dump_irq, (void *)10);
}

static void dump_package(struct topo_obj *d, void *data)
{
	char *buffer = data;
	cpumask_scnprintf(buffer, 4096, d->mask);
	printf("Package %i:  numa_node is %d cpu mask is %s (load %lu)\n", d->number, package_numa_node(d)->number, buffer, (unsigned long)d->load);
	if (d->children)
		for_each_cache_domain(d->children, dump_cache_domain, buffer);
	if (d->interrupts)
		for_each_irq(d->interrupts, dump_irq, (void *)2);
}

void dump_tree(void)
{
	char buffer[4096];
	for_each_package(NULL, dump_package, buffer);
}

static void clear_cpu_stats(struct topo_obj *d, void *data __attribute__((unused)))
{
	struct topo_obj *c = (struct topo_obj *)d;
	c->load = 0;
}

static void clear_cd_stats(struct topo_obj *d, void *data __attribute__((unused)))
{
	d->load = 0;
	for_each_cpu_core(d->children, clear_cpu_stats, NULL);
}

static void clear_package_stats(struct topo_obj *d, void *data __attribute__((unused)))
{
	d->load = 0;
	for_each_cache_domain(d->children, clear_cd_stats, NULL);
}

static void clear_node_stats(struct topo_obj *d, void *data __attribute__((unused)))
{
	d->load = 0;
	for_each_package(d->children, clear_package_stats, NULL);
}

static void clear_irq_stats(struct irq_info *info, void *data __attribute__((unused)))
{
	info->load = 0;
}

/*
 * this function removes previous state from the cpu tree, such as
 * which level does how much work and the actual lists of interrupts 
 * assigned to each component
 */
void clear_work_stats(void)
{
	for_each_numa_node(NULL, clear_node_stats, NULL);
	for_each_irq(NULL, clear_irq_stats, NULL);
}


void parse_cpu_tree(void)
{
	DIR *dir;
	struct dirent *entry;

	cpus_complement(unbanned_cpus, banned_cpus);

	dir = opendir("/sys/devices/system/cpu");
	if (!dir)
		return;
	do {
		entry = readdir(dir);
                if (entry && strlen(entry->d_name)>3 && strstr(entry->d_name,"cpu")) {
			char new_path[PATH_MAX];
			/*
 			 * We only want to count real cpus, not cpufreq and
 			 * cpuidle
 			 */
			if ((entry->d_name[3] < 0x30) | (entry->d_name[3] > 0x39))
				continue;
			sprintf(new_path, "/sys/devices/system/cpu/%s", entry->d_name);
			do_one_cpu(new_path);
		}
	} while (entry);
	closedir(dir);  

	if (debug_mode)
		dump_tree();

}


/*
 * This function frees all memory related to a cpu tree so that a new tree
 * can be read
 */
void clear_cpu_tree(void)
{
	GList *item;
	struct topo_obj *cpu;
	struct topo_obj *cache_domain;
	struct topo_obj *package;

	while (packages) {
		item = g_list_first(packages);
		package = item->data;
		g_list_free(package->children);
		g_list_free(package->interrupts);
		free(package);
		packages = g_list_delete_link(packages, item);
	}
	package_count = 0;

	while (cache_domains) {
		item = g_list_first(cache_domains);
		cache_domain = item->data;
		g_list_free(cache_domain->children);
		g_list_free(cache_domain->interrupts);
		free(cache_domain);
		cache_domains = g_list_delete_link(cache_domains, item);
	}
	cache_domain_count = 0;


	while (cpus) {
		item = g_list_first(cpus);
		cpu = item->data;
		g_list_free(cpu->interrupts);
		free(cpu);
		cpus = g_list_delete_link(cpus, item);
	}
	core_count = 0;

}


void for_each_package(GList *list, void (*cb)(struct topo_obj *p, void *data), void *data)
{
	GList *entry = g_list_first(list ? list : packages);
	GList *next;

	while (entry) {
		next = g_list_next(entry);
		cb(entry->data, data);
		entry = next;
	}
}

void for_each_cache_domain(GList *list, void (*cb)(struct topo_obj *c, void *data), void *data)
{
	GList *entry = g_list_first(list ? list : cache_domains);
	GList *next;

	while (entry) {
		next = g_list_next(entry);
		cb(entry->data, data);
		entry = next;
	}
}

void for_each_cpu_core(GList *list, void (*cb)(struct topo_obj *c, void *data), void *data)
{
	GList *entry = g_list_first(list ? list : cpus);
	GList *next;

	while (entry) {
		next = g_list_next(entry);
		cb(entry->data, data);
		entry = next;
	}
}

static gint compare_cpus(gconstpointer a, gconstpointer b)
{
	const struct topo_obj *ai = a;
	const struct topo_obj *bi = b;

	return ai->number - bi->number;	
}

struct topo_obj *find_cpu_core(int cpunr)
{
	GList *entry;
	struct topo_obj find;

	find.number = cpunr;
	entry = g_list_find_custom(cpus, &find, compare_cpus);

	return entry ? entry->data : NULL;
}	

int get_cpu_count(void)
{
	return g_list_length(cpus);
}

