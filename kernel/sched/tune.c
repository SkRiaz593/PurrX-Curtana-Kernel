#include <linux/cgroup.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#include <trace/events/sched.h>

#include "sched.h"
#include "tune.h"

bool schedtune_initialized = false;
extern struct reciprocal_value schedtune_spc_rdiv;

/* We hold schedtune boost in effect for at least this long (50ms = ~3 display frames) */
#define SCHEDTUNE_BOOST_HOLD_NS 50000000ULL

/*
 * EAS scheduler tunables for task groups.
 */

/* SchdTune tunables for a group of tasks */
struct schedtune {
	/* SchedTune CGroup subsystem */
	struct cgroup_subsys_state css;

	/* Boost group allocated ID */
	int idx;

	/* Boost value for tasks on that SchedTune CGroup */
	int boost;

#ifdef CONFIG_SCHED_WALT
	/* Toggle ability to override sched boost enabled */
	bool sched_boost_no_override;

	/*
	 * Controls whether a cgroup is eligible for sched boost or not. This
	 * can temporariliy be disabled by the kernel based on the no_override
	 * flag above.
	 */
	bool sched_boost_enabled;

	/*
	 * Controls whether tasks of this cgroup should be colocated with each
	 * other and tasks of other cgroups that have the same flag turned on.
	 */
	bool colocate;

	/* Controls whether further updates are allowed to the colocate flag */
	bool colocate_update_disabled;
#endif /* CONFIG_SCHED_WALT */

	/* Hint to bias scheduling of tasks on that SchedTune CGroup
	 * towards idle CPUs */
	int prefer_idle;
};

static inline struct schedtune *css_st(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct schedtune, css) : NULL;
}

static inline struct schedtune *task_schedtune(struct task_struct *tsk)
{
	return css_st(task_css(tsk, schedtune_cgrp_id));
}

static inline struct schedtune *parent_st(struct schedtune *st)
{
	return css_st(st->css.parent);
}

/*
 * SchedTune root control group
 */
static struct schedtune
root_schedtune = {
	.boost	= 0,
#ifdef CONFIG_SCHED_WALT
	.sched_boost_no_override = false,
	.sched_boost_enabled = true,
	.colocate = false,
	.colocate_update_disabled = false,
#endif
	.prefer_idle = 0,
};

#define BOOSTGROUPS_COUNT 6

/* Array of configured boostgroups */
static struct schedtune *allocated_group[BOOSTGROUPS_COUNT] = {
	&root_schedtune,
	NULL,
};

struct boost_groups {
	/* Maximum boost value for all RUNNABLE tasks on a CPU */
	bool idle;
	int boost_max;
	u64 boost_ts;
	struct {
		/* The boost for tasks on that boost group */
		int boost;
		/* Count of RUNNABLE tasks on that boost group */
		unsigned tasks;
		/* Timestamp of boost activation */
		u64 ts;
	} group[BOOSTGROUPS_COUNT];
	/* CPU's boost group locking */
	raw_spinlock_t lock;
};

/* Boost groups affecting each CPU in the system */
DEFINE_PER_CPU(struct boost_groups, cpu_boost_groups);

#ifdef CONFIG_SCHED_WALT
static inline void init_sched_boost(struct schedtune *st)
{
	st->sched_boost_no_override = false;
	st->sched_boost_enabled = true;
	/* Esports: Default colocate to true so game threads stay on Big cluster together */
	st->colocate = true;
	st->colocate_update_disabled = false;
}

void update_cgroup_boost_settings(void)
{
	int i;

	for (i = 0; i < BOOSTGROUPS_COUNT; i++) {
		if (!allocated_group[i])
			break;

		if (allocated_group[i]->sched_boost_no_override)
			continue;

		allocated_group[i]->sched_boost_enabled = false;
	}
}

void restore_cgroup_boost_settings(void)
{
	int i;

	for (i = 0; i < BOOSTGROUPS_COUNT; i++) {
		if (!allocated_group[i])
			break;

		allocated_group[i]->sched_boost_enabled = true;
	}
}

bool task_sched_boost(struct task_struct *p)
{
	struct schedtune *st;
	bool enabled;

	if (unlikely(!schedtune_initialized))
		return false;

	rcu_read_lock();
	st = task_schedtune(p);
	enabled = st->sched_boost_enabled;
	rcu_read_unlock();

	return enabled;
}

static u64
sched_boost_override_read(struct cgroup_subsys_state *css,
			struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->sched_boost_no_override;
}

static int sched_boost_override_write(struct cgroup_subsys_state *css,
			struct cftype *cft, u64 override)
{
	struct schedtune *st = css_st(css);

	st->sched_boost_no_override = !!override;

	return 0;
}

#endif /* CONFIG_SCHED_WALT */

static inline bool schedtune_boost_timeout(u64 now, u64 ts)
{
	return ((now - ts) > SCHEDTUNE_BOOST_HOLD_NS);
}

static inline bool
schedtune_boost_group_active(int idx, struct boost_groups* bg, u64 now)
{
	if (bg->group[idx].tasks)
		return true;

	return !schedtune_boost_timeout(now, bg->group[idx].ts);
}

static void
schedtune_cpu_update(int cpu, u64 now)
{
	struct boost_groups *bg = &per_cpu(cpu_boost_groups, cpu);
	int boost_max;
	u64 boost_ts;
	int idx;

	boost_max = bg->group[0].boost;
	boost_ts = now;
	for (idx = 1; idx < BOOSTGROUPS_COUNT; ++idx) {
		if (!schedtune_boost_group_active(idx, bg, now))
			continue;

		if (boost_max > bg->group[idx].boost)
			continue;

		boost_max = bg->group[idx].boost;
		boost_ts =  bg->group[idx].ts;
	}
	boost_max = max(boost_max, 0);
	bg->boost_max = boost_max;
	bg->boost_ts = boost_ts;
}

static int
schedtune_boostgroup_update(int idx, int boost)
{
	struct boost_groups *bg;
	int cur_boost_max;
	int old_boost;
	int cpu;
	u64 now;

	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);

		cur_boost_max = bg->boost_max;
		old_boost = bg->group[idx].boost;

		bg->group[idx].boost = boost;

		now = sched_clock_cpu(cpu);
		if (boost > cur_boost_max &&
			schedtune_boost_group_active(idx, bg, now)) {
			bg->boost_max = boost;
			bg->boost_ts = bg->group[idx].ts;

			trace_sched_tune_boostgroup_update(cpu, 1, bg->boost_max);
			continue;
		}

		if (cur_boost_max == old_boost && old_boost > boost) {
			schedtune_cpu_update(cpu, now);
			trace_sched_tune_boostgroup_update(cpu, -1, bg->boost_max);
			continue;
		}

		trace_sched_tune_boostgroup_update(cpu, 0, bg->boost_max);
	}

	return 0;
}

#define ENQUEUE_TASK  1
#define DEQUEUE_TASK -1

static inline bool
schedtune_update_timestamp(struct task_struct *p)
{
	if (sched_feat(SCHEDTUNE_BOOST_HOLD_ALL))
		return true;

	return task_has_rt_policy(p);
}

static inline void
schedtune_tasks_update(struct task_struct *p, int cpu, int idx, int task_count)
{
	struct boost_groups *bg = &per_cpu(cpu_boost_groups, cpu);
	int tasks = bg->group[idx].tasks + task_count;

	bg->group[idx].tasks = max(0, tasks);

	if (task_count > 0) {
		u64 now = sched_clock_cpu(cpu);

		if (schedtune_update_timestamp(p))
			bg->group[idx].ts = now;

		if (bg->group[idx].tasks == 1)
			schedtune_cpu_update(cpu, now);
	}

	trace_sched_tune_tasks_update(p, cpu, tasks, idx,
			bg->group[idx].boost, bg->boost_max,
			bg->group[idx].ts);
}

void schedtune_enqueue_task(struct task_struct *p, int cpu)
{
	struct boost_groups *bg = &per_cpu(cpu_boost_groups, cpu);
	unsigned long irq_flags;
	struct schedtune *st;
	int idx;

	if (unlikely(!schedtune_initialized))
		return;

	raw_spin_lock_irqsave(&bg->lock, irq_flags);
	rcu_read_lock();

	st = task_schedtune(p);
	idx = st->idx;

	schedtune_tasks_update(p, cpu, idx, ENQUEUE_TASK);

	rcu_read_unlock();
	raw_spin_unlock_irqrestore(&bg->lock, irq_flags);
}

int schedtune_can_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;
	struct boost_groups *bg;
	struct rq_flags rq_flags;
	unsigned int cpu;
	struct rq *rq;
	int src_bg;
	int dst_bg;
	int tasks;
	u64 now;

	if (unlikely(!schedtune_initialized))
		return 0;

	cgroup_taskset_for_each(task, css, tset) {
		rq = task_rq_lock(task, &rq_flags);

		if (!task->on_rq) {
			task_rq_unlock(rq, task, &rq_flags);
			continue;
		}

		cpu = cpu_of(rq);
		bg = &per_cpu(cpu_boost_groups, cpu);
		raw_spin_lock(&bg->lock);

		dst_bg = css_st(css)->idx;
		src_bg = task_schedtune(task)->idx;

		if (unlikely(dst_bg == src_bg)) {
			raw_spin_unlock(&bg->lock);
			task_rq_unlock(rq, task, &rq_flags);
			continue;
		}

		tasks = bg->group[src_bg].tasks - 1;
		bg->group[src_bg].tasks = max(0, tasks);
		bg->group[dst_bg].tasks += 1;

		now = sched_clock_cpu(cpu);
		bg->group[dst_bg].ts = now;

		bg->boost_ts = now - SCHEDTUNE_BOOST_HOLD_NS;

		raw_spin_unlock(&bg->lock);
		task_rq_unlock(rq, task, &rq_flags);
	}

	return 0;
}

#ifdef CONFIG_SCHED_WALT
static u64 sched_colocate_read(struct cgroup_subsys_state *css,
			struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->colocate;
}

static int sched_colocate_write(struct cgroup_subsys_state *css,
			struct cftype *cft, u64 colocate)
{
	struct schedtune *st = css_st(css);

	if (st->colocate_update_disabled)
		return -EPERM;

	st->colocate = !!colocate;
	st->colocate_update_disabled = true;
	return 0;
}

bool schedtune_task_colocated(struct task_struct *p)
{
	struct schedtune *st;
	bool colocated;

	if (unlikely(!schedtune_initialized))
		return false;

	rcu_read_lock();
	st = task_schedtune(p);
	colocated = st->colocate;
	rcu_read_unlock();

	return colocated;
}

#else /* CONFIG_SCHED_WALT */

static inline void init_sched_boost(struct schedtune *st) { }

#endif /* CONFIG_SCHED_WALT */

void schedtune_cancel_attach(struct cgroup_taskset *tset)
{
	WARN(1, "SchedTune cancel attach not implemented");
}

void schedtune_dequeue_task(struct task_struct *p, int cpu)
{
	struct boost_groups *bg = &per_cpu(cpu_boost_groups, cpu);
	unsigned long irq_flags;
	struct schedtune *st;
	int idx;

	if (unlikely(!schedtune_initialized))
		return;

	raw_spin_lock_irqsave(&bg->lock, irq_flags);
	rcu_read_lock();

	st = task_schedtune(p);
	idx = st->idx;

	schedtune_tasks_update(p, cpu, idx, DEQUEUE_TASK);

	rcu_read_unlock();
	raw_spin_unlock_irqrestore(&bg->lock, irq_flags);
}

int schedtune_cpu_boost(int cpu)
{
	struct boost_groups *bg;
	u64 now;

	bg = &per_cpu(cpu_boost_groups, cpu);
	now = sched_clock_cpu(cpu);

	if (schedtune_boost_timeout(now, bg->boost_ts))
		schedtune_cpu_update(cpu, now);

	return bg->boost_max;
}

int schedtune_task_boost(struct task_struct *p)
{
	struct schedtune *st;
	int task_boost;

	if (unlikely(!schedtune_initialized))
		return 0;

	rcu_read_lock();
	st = task_schedtune(p);
	task_boost = st->boost;
	rcu_read_unlock();

	return task_boost;
}

int schedtune_prefer_idle(struct task_struct *p)
{
	struct schedtune *st;
	int prefer_idle;

	if (unlikely(!schedtune_initialized))
		return 0;

	rcu_read_lock();
	st = task_schedtune(p);
	prefer_idle = st->prefer_idle;

	/*
	 * Esports Optimization:
	 * If the task has positive boost, unconditionally force prefer_idle = 1
	 * so Qualcomm WALT pins the active render thread to Big Kryo Gold cores.
	 */
	if (st->boost > 0)
		prefer_idle = 1;

	rcu_read_unlock();

	return prefer_idle;
}

static u64
prefer_idle_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->prefer_idle;
}

static int
prefer_idle_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 prefer_idle)
{
	struct schedtune *st = css_st(css);
	st->prefer_idle = !!prefer_idle;

	return 0;
}

static s64
boost_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->boost;
}

#ifdef CONFIG_SCHED_WALT
static void schedtune_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;
	struct schedtune *st;
	bool colocate;

	cgroup_taskset_first(tset, &css);
	st = css_st(css);

	colocate = st->colocate;

	cgroup_taskset_for_each(task, css, tset)
		sync_cgroup_colocation(task, colocate);

}
#else
static void schedtune_attach(struct cgroup_taskset *tset)
{
}
#endif

static int
boost_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    s64 boost)
{
	struct schedtune *st = css_st(css);

	if (boost < 0 || boost > 100)
		return -EINVAL;

	st->boost = boost;

	schedtune_boostgroup_update(st->idx, st->boost);

	return 0;
}

static struct cftype files[] = {
#ifdef CONFIG_SCHED_WALT
	{
		.name = "sched_boost_no_override",
		.read_u64 = sched_boost_override_read,
		.write_u64 = sched_boost_override_write,
	},
	{
		.name = "colocate",
		.read_u64 = sched_colocate_read,
		.write_u64 = sched_colocate_write,
	},
#endif
	{
		.name = "boost",
		.read_s64 = boost_read,
		.write_s64 = boost_write,
	},
	{
		.name = "prefer_idle",
		.read_u64 = prefer_idle_read,
		.write_u64 = prefer_idle_write,
	},
	{ }	/* terminate */
};

static int
schedtune_boostgroup_init(struct schedtune *st)
{
	struct boost_groups *bg;
	int cpu;

	allocated_group[st->idx] = st;

	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);
		bg->group[st->idx].boost = 0;
		bg->group[st->idx].tasks = 0;
		bg->group[st->idx].ts = 0;
	}

	return 0;
}

static struct cgroup_subsys_state *
schedtune_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct schedtune *st;
	int idx;

	if (!parent_css)
		return &root_schedtune.css;

	if (parent_css != &root_schedtune.css) {
		pr_err("Nested SchedTune boosting groups not allowed\n");
		return ERR_PTR(-ENOMEM);
	}

	for (idx = 1; idx < BOOSTGROUPS_COUNT; ++idx)
		if (!allocated_group[idx])
			break;
	if (idx == BOOSTGROUPS_COUNT) {
		pr_err("Trying to create more than %d SchedTune boosting groups\n",
		       BOOSTGROUPS_COUNT);
		return ERR_PTR(-ENOSPC);
	}

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		goto out;

	st->idx = idx;
	init_sched_boost(st);

	/* Esports: Child cgroups (top-app) default to prefer_idle = 1 */
	st->prefer_idle = 1;

	if (schedtune_boostgroup_init(st))
		goto release;

	return &st->css;

release:
	kfree(st);
out:
	return ERR_PTR(-ENOMEM);
}

static void
schedtune_boostgroup_release(struct schedtune *st)
{
	schedtune_boostgroup_update(st->idx, 0);
	allocated_group[st->idx] = NULL;
}

static void
schedtune_css_free(struct cgroup_subsys_state *css)
{
	struct schedtune *st = css_st(css);

	schedtune_boostgroup_release(st);
	kfree(st);
}

struct cgroup_subsys schedtune_cgrp_subsys = {
	.css_alloc	= schedtune_css_alloc,
	.css_free	= schedtune_css_free,
	.attach		= schedtune_attach,
	.can_attach     = schedtune_can_attach,
	.cancel_attach  = schedtune_cancel_attach,
	.legacy_cftypes	= files,
	.early_init	= 1,
};

static inline void
schedtune_init_cgroups(void)
{
	struct boost_groups *bg;
	int cpu;

	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);
		memset(bg, 0, sizeof(struct boost_groups));
		raw_spin_lock_init(&bg->lock);
	}

	pr_info("schedtune: configured to support %d boost groups\n",
		BOOSTGROUPS_COUNT);

	schedtune_initialized = true;
}

static int
schedtune_init(void)
{
	schedtune_spc_rdiv = reciprocal_value(100);
	schedtune_init_cgroups();
	return 0;
}
postcore_initcall(schedtune_init);
