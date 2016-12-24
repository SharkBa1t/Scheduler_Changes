#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/delay.h>

#include "wrapper_sched.h"

int data = 0;
int migrate_flag = 0;
extern int switch_job_sched;
static int setup_bfs_done = 0;
struct task_struct *switch_task = NULL;
struct task_struct *setup_bfs = NULL;
struct task_struct *setup_cfs = NULL;
wait_queue_head_t bfs_wq;
wait_queue_head_t cfs_wq;

#define for_each_sched_entity(se) \
	for (; se; se = se->parent)

void deactivate_task_from_cfs(struct rq *rq, struct task_struct *p, int flags)
{
	if (p == NULL)
		return;
	if (task_contributes_to_load(p))
		rq->nr_uninterruptible++;

	dequeue_task_cfs(rq, p, flags);
}

void migrate_tasks_to_cfs(void)
{
	struct rq *rq;
	struct task_struct *edt = NULL;
	struct task_struct *temp = NULL;
	struct sched_entity *se = NULL;
	unsigned long idx = -1;
	int cpu;
	unsigned long flags;
	struct cfs_rq *cfs_rq;

	cpu = smp_processor_id();
	rq = cpu_rq(cpu);
	cfs_rq = &rq->cfs;

	if (switch_job_sched == 0) {
		smp_mb__before_spinlock();
		raw_spin_lock_irq(&rq->lock);
		lockdep_pin_lock(&rq->lock);
		grq_lock_irqsave(&flags);
		activate_task_cfs(rq, current, 0);
		rq->curr = current;
		se = &current->se;
		cfs_rq->curr = se;
		set_next_entity(cfs_rq, se);
		rq->clock_skip_update = 0;

		do {
				struct list_head *queue;
				struct task_struct *p;

				idx = idx_grq(++idx);
				queue = list_head_grq(idx);
				if (list_empty(queue))
					continue;
				if (idx < MAX_RT_PRIO) {
					list_for_each_entry_safe(p, edt, queue, run_list) {
						temp = p;
						setup_tasks_cfs_migrated(p);
						deactivate_task_from_bfs(rq, p);
						activate_task_cfs(rq, p, 0);
					}
					continue;
				}
				if (list_empty(queue))
					continue;
				list_for_each_entry_safe(p, edt, queue, run_list) {
					temp = p;
					setup_tasks_cfs_migrated(p);
					deactivate_task_from_bfs(rq, p);
					activate_task_cfs(rq, p, 0);
				}
		} while (!temp);
		grq_unlock_irqrestore(&flags);
		lockdep_unpin_lock(&rq->lock);
		raw_spin_unlock_irq(&rq->lock);
	}
}

void migrate_tasks_to_bfs(void)
{
	int cpu;
	struct rq *rq;
	struct cfs_rq *cfs_rq;
	struct dl_rq *dl_rq;
	struct rb_node *node;
	struct rb_root *cfs_tree;
	struct rb_root *dl_tree;
	struct task_struct *task;
	struct sched_entity *entity;
	struct sched_dl_entity *dl_entity;
	unsigned long flags;

	cpu = smp_processor_id();
	if (switch_job_sched == 1)
	{
		rq = cpu_rq(cpu);
		cfs_rq=&(rq->cfs);
		cfs_tree=&(cfs_rq->tasks_timeline);

/**
  * The current stuff
  *
  */
		entity = cfs_rq->curr;
		if (entity) {
			task = container_of(entity, struct task_struct, se);
		}

/**
  * Ends here
  */
		entity = rb_entry(cfs_tree->rb_node, struct sched_entity, run_node);
		task = container_of(entity, struct task_struct, se);

		smp_mb__before_spinlock();
		raw_spin_lock_irq(&rq->lock);
		lockdep_pin_lock(&rq->lock);
		grq_lock_irqsave(&flags);
		// CFS Tasks
		for (node = rb_first(cfs_tree); node; node = rb_next(node)) {
			entity = rb_entry(node, struct sched_entity, run_node);
			if (!entity->my_q) {
				task = container_of(entity, struct task_struct, se);
				deactivate_task_from_cfs(rq, task, 0);
				setup_tasks_bfs_migrated(task);
				activate_task_bfs(rq, task);
			}
		}
		current->on_cpu_bfs = true;
		INIT_LIST_HEAD(&current->run_list);
		//DL Tasks
		dl_rq=&(rq->dl);
		dl_tree=&(dl_rq->rb_root);
		for (node = rb_first(dl_tree); node; node = rb_next(node))
		{
			dl_entity = rb_entry(node, struct sched_dl_entity, rb_node);
			task = container_of(dl_entity, struct task_struct, dl);
			if (!entity->my_q) // Sched entity is a task
			{
				task = container_of(dl_entity, struct task_struct, dl);
				deactivate_task_from_cfs(rq, task, 0);
				setup_tasks_bfs_migrated(task);
				activate_task_bfs(rq, task);
			}
		}
		lockdep_unpin_lock(&rq->lock);
		raw_spin_unlock_irq(&rq->lock);
		grq_unlock_irqrestore(&flags);
	}
}

int setup_bfs_function(void *value)
{
	while(!kthread_should_stop()) {
		wait_event_interruptible(bfs_wq, migrate_flag == 1);
		current->migrate_init_sched = 1;
		local_irq_disable();
		preempt_disable();
		migrate_flag = 0;
		if (switch_job_sched == 1) {
			sch_alg = 1;
			if (setup_bfs_done == 0) {
				sched_init_bfs();
				migrate_tasks_to_bfs();
				setup_bfs_done = 1;
			}
			else
				migrate_tasks_to_bfs();
		}
		preempt_enable();
		local_irq_enable();
	}
	return 0;
}

int setup_cfs_function(void *value)
{
	while(!kthread_should_stop()) {
		wait_event_interruptible(cfs_wq, migrate_flag == 1);
		current->migrate_init_sched = 2;
		local_irq_disable();
		preempt_disable();
		migrate_flag = 0;
		if (switch_job_sched == 0)
		{
			sch_alg = 0;
			migrate_tasks_to_cfs();
		}
		preempt_enable();
		local_irq_enable();
	}
	return 0;
}

int switch_thread_function(void *value)
{
	migrate_flag = 0;
	init_waitqueue_head(&bfs_wq);
	init_waitqueue_head(&cfs_wq);
	if (!setup_bfs) {
		setup_bfs = kthread_run(&setup_bfs_function,
				&data, "bfsinit");
	}
	if (!setup_cfs) {
		setup_cfs = kthread_run(&setup_cfs_function,
				&data, "cfsinit");
	}
	while(!kthread_should_stop()) {
		if ((switch_job_sched == 1) && (sch_alg == 0)) {
			if (setup_cfs)
				setup_cfs->on_cpu_bfs = false;
			migrate_flag = 1;
			wake_up_interruptible(&bfs_wq);
		}
		else if ((switch_job_sched == 0) && (sch_alg == 1)) {
			migrate_flag = 1;
			wake_up_interruptible(&cfs_wq);
		}
		else {
			migrate_flag = 0;
			msleep(1000);
		}
	}
	return 0;
}

void switch_task_init(void)
{
	if (!switch_task)
	{
		switch_task = kthread_run(&switch_thread_function,
				&data, "switcher");
	}
}
