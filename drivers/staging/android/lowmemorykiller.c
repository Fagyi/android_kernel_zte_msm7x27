/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_adj values will get killed. Specify
 * the minimum oom_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/swap.h>

#ifdef CONFIG_HIGHMEM
#define _ZONE ZONE_HIGHMEM
#else
#define _ZONE ZONE_NORMAL
#endif

#define KSWAPD_ZONE_BALANCE_GAP_RATIO 100
#define OOM_SCORE_ADJ_MAX       1000

static uint32_t lowmem_debug_level = 1;
static int lowmem_adj[6] = {
	0,
	1,
	6,
	12,
	15,
};
static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;
static int lmk_fast_run = 1;

static unsigned long lowmem_deathpending_timeout;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			printk(x);			\
	} while (0)

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	} while_each_thread(p, t);

	return 0;
}

/* from 3.4 oom_kill.c -> */
/*
 * The process p may have detached its own ->mm while exiting or through
 * use_mm(), but one or more of its subthreads may still have a valid
 * pointer.  Return p, or any of its subthreads with a valid ->mm, with
 * task_lock() held.
 */
struct task_struct *find_lock_task_mm(struct task_struct *p)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (likely(t->mm))
			return t;
		task_unlock(t);
	} while_each_thread(p, t);

	return NULL;
}

static DEFINE_MUTEX(scan_mutex);

void tune_lmk_zone_param(struct zonelist *zonelist, int classzone_idx,
					int *other_free, int *other_file)
{
	struct zone *zone;
	struct zoneref *zoneref;
	int zone_idx;

	for_each_zone_zonelist(zone, zoneref, zonelist, MAX_NR_ZONES) {
		zone_idx = zonelist_zone_idx(zoneref);
		if (zone_idx == ZONE_MOVABLE) {
			lowmem_print(1, "FIXME: msm7x27 lowmem_shrink should "
			  "not encounter a ZONE_MOVABLE\n");
			continue;
		}

		if (zone_idx > classzone_idx) {
			if (other_free != NULL)
				*other_free -= zone_page_state(zone,
							       NR_FREE_PAGES);
			if (other_file != NULL)
				*other_file -= zone_page_state(zone,
							       NR_FILE_PAGES)
					      - zone_page_state(zone, NR_SHMEM);
		} else if (zone_idx < classzone_idx) {
			if (zone_watermark_ok(zone, 0, 0, classzone_idx, 0)) {
				*other_free -=
					  zone->lowmem_reserve[classzone_idx];
			} else {
				*other_free -=
					   zone_page_state(zone, NR_FREE_PAGES);
			}
		}
	}
}

static struct task_struct *pick_next_from_adj_tree(struct task_struct *task);
static struct task_struct *pick_first_task(void);
static struct task_struct *pick_last_task(void);

void tune_lmk_param(int *other_free, int *other_file, gfp_t gfp_mask)
{
	struct zone *preferred_zone;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx, classzone_idx;
	unsigned long balance_gap;

	zonelist = node_zonelist(0, gfp_mask);
	high_zoneidx = gfp_zone(gfp_mask);
	first_zones_zonelist(zonelist, high_zoneidx, NULL, &preferred_zone);
	classzone_idx = zone_idx(preferred_zone);

	balance_gap = min(low_wmark_pages(preferred_zone),
			  (preferred_zone->present_pages +
			   KSWAPD_ZONE_BALANCE_GAP_RATIO-1) /
			   KSWAPD_ZONE_BALANCE_GAP_RATIO);

	if (likely(current_is_kswapd() && zone_watermark_ok(preferred_zone, 0,
			  high_wmark_pages(preferred_zone) + SWAP_CLUSTER_MAX +
			  balance_gap, 0, 0))) {
		if (lmk_fast_run)
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       other_file);
		else
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       NULL);

		if (zone_watermark_ok(preferred_zone, 0, 0, _ZONE, 0)) {
			*other_free -=
				  preferred_zone->lowmem_reserve[_ZONE];
		} else {
			*other_free -= zone_page_state(preferred_zone,
						      NR_FREE_PAGES);
		}

		lowmem_print(4, "lowmem_shrink of kswapd tuning for highmem "
			     "ofree %d, %d\n", *other_free, *other_file);
	} else {
		tune_lmk_zone_param(zonelist, classzone_idx, other_free,
			       other_file);

		lowmem_print(4, "lowmem_shrink tuning for others ofree %d, "
			     "%d\n", *other_free, *other_file);
	}
}

static int lowmem_shrink(struct shrinker *s, int nr_to_scan, gfp_t gfp_mask)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	int rem = 0;
	int tasksize;
	int i;
	int min_adj = OOM_SCORE_ADJ_MAX + 1;
	int selected_tasksize = 0;
	int selected_oom_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	
	tsk = current->group_leader;
	if ((tsk->flags & PF_EXITING) && test_task_flag(tsk, TIF_MEMDIE)) {
		set_tsk_thread_flag(current, TIF_MEMDIE);
		return 0;
	}
	
	if (nr_to_scan > 0) {
		if (mutex_lock_interruptible(&scan_mutex) < 0)
			return 0;
	}

	other_free = global_page_state(NR_FREE_PAGES);
	other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM);

	tune_lmk_param(&other_free, &other_file, gfp_mask);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		if (other_free < lowmem_minfree[i] &&
		    other_file < lowmem_minfree[i]) {
			min_adj = lowmem_adj[i];
			break;
		}
	}
	if (nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %d, %x, ofree %d %d, ma %d\n",
				nr_to_scan, gfp_mask, other_free,
				other_file, min_adj);
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (nr_to_scan <= 0 || min_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %d, %x, return %d\n",
			     nr_to_scan, gfp_mask, rem);

		if (nr_to_scan > 0)
			mutex_unlock(&scan_mutex);

		return rem;
	}
	selected_oom_adj = min_adj;

	rcu_read_lock();
	for (tsk = pick_first_task();
		tsk != pick_last_task();
		tsk = pick_next_from_adj_tree(tsk)) {
		struct task_struct *p;
		int oom_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		if (time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			if (test_task_flag(tsk, TIF_MEMDIE)) {
				rcu_read_unlock();
				/* give the system time to free up the memory */
				if (!same_thread_group(current, tsk))
					msleep_interruptible(20);
				else
					set_tsk_thread_flag(current, TIF_MEMDIE);
				mutex_unlock(&scan_mutex);
				return 0;
			}
		}

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_adj = p->signal->oom_adj;
		if (oom_adj < min_adj) {
			task_unlock(p);
			break;
		}
		
		
		if (fatal_signal_pending(p) ||
			((p->flags & PF_EXITING) &&
			test_tsk_thread_flag(p, TIF_MEMDIE))) {
			lowmem_print(2, "skip slow dying process %d\n", p->pid);
			task_unlock(p);
			continue;
		}
		
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_adj < selected_oom_adj)
				break;
			if (oom_adj == selected_oom_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_adj = oom_adj;
		lowmem_print(2, "select %d (%s), adj %d, size %d, to kill\n",
			     p->pid, p->comm, oom_adj, tasksize);
	}
	if (selected) {
		lowmem_print(1, "send sigkill to %d (%s), adj %d, size %d\n",
			     selected->pid, selected->comm,
			     selected_oom_adj, selected_tasksize);
		lowmem_deathpending_timeout = jiffies + HZ;
		send_sig(SIGKILL, selected, 0);
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		rem -= selected_tasksize;
		rcu_read_unlock();
		/* give the system time to free up the memory */
		msleep_interruptible(20);
	} else
		rcu_read_unlock();

	lowmem_print(4, "lowmem_shrink %d, %x, return %d\n",
		     nr_to_scan, gfp_mask, rem);
	mutex_unlock(&scan_mutex);
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

static int __init lowmem_init(void)
{
	register_shrinker(&lowmem_shrinker);
	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

DEFINE_SPINLOCK(lmk_lock);
struct rb_root tasksadj = RB_ROOT;
void add_2_adj_tree(struct task_struct *task)
{		struct rb_node **link = &tasksadj.rb_node;
struct rb_node *parent = NULL;
struct task_struct *task_entry;
s64 key = task->signal->oom_adj;
/*
 * Find the right place in the rbtree:
 */
spin_lock(&lmk_lock);
while (*link) {
	parent = *link;
	task_entry = rb_entry(parent, struct task_struct, adj_node);
	if (key < task_entry->signal->oom_adj)
		link = &parent->rb_right;
	else
		link = &parent->rb_left;
}

rb_link_node(&task->adj_node, parent, link);
rb_insert_color(&task->adj_node, &tasksadj);
spin_unlock(&lmk_lock);
}

void delete_from_adj_tree(struct task_struct *task)
{
	spin_lock(&lmk_lock);
	rb_erase(&task->adj_node, &tasksadj);
	spin_unlock(&lmk_lock);
}


static struct task_struct *pick_next_from_adj_tree(struct task_struct *task)
{
	struct rb_node *next;
	
	spin_lock(&lmk_lock);
	next = rb_next(&task->adj_node);
	spin_unlock(&lmk_lock);
	
	if (!next)
		return NULL;
	
	return rb_entry(next, struct task_struct, adj_node);
}

static struct task_struct *pick_first_task(void)
{
	struct rb_node *left;
	
	spin_lock(&lmk_lock);
	left = rb_first(&tasksadj);
	spin_unlock(&lmk_lock);
	
	if (!left)
		return NULL;
	
	return rb_entry(left, struct task_struct, adj_node);
}

static struct task_struct *pick_last_task(void)
{
	struct rb_node *right;
	
	spin_lock(&lmk_lock);
	right = rb_last(&tasksadj);
	spin_unlock(&lmk_lock);
	
	if (!right)
		return NULL;
	
	return rb_entry(right, struct task_struct, adj_node);
}

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
module_param_array_named(adj, lowmem_adj, int, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmk_fast_run, lmk_fast_run, int, S_IRUGO | S_IWUSR);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");
