/*
 *  linux/drivers/thermal/cpu_cooling.c
 *
 *  Copyright (C) 2012	Samsung Electronics Co., Ltd(http://www.samsung.com)
 *  Copyright (C) 2012  Amit Daniel <amit.kachhap@linaro.org>
 *
 *  Copyright (C) 2014  Viresh Kumar <viresh.kumar@linaro.org>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 and
 *  only version 2 as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpu_cooling.h>
#include <linux/sched.h>
#include <linux/of_device.h>
#include <linux/suspend.h>

#include <trace/events/thermal.h>

/*
 * Cooling state <-> CPUFreq frequency
 *
 * Cooling states are translated to frequencies throughout this driver and this
 * is the relation between them.
 *
 * Highest cooling state corresponds to lowest possible frequency.
 *
 * i.e.
 *	level 0 --> 1st Max Freq
 *	level 1 --> 2nd Max Freq
 *	...
 *	level n --> core isolated
 */

/**
 * struct freq_table - frequency table along with power entries
 * @frequency:	frequency in KHz
 * @power:	power in mW
 *
 * This structure is built when the cooling device registers and helps
 * in translating frequency to power and vice versa.
 */
struct freq_table {
	u32 frequency;
	u32 power;
};

/**
 * struct time_in_idle - Idle time stats
 * @time: previous reading of the absolute time that this cpu was idle
 * @timestamp: wall time of the last invocation of get_cpu_idle_time_us()
 */
struct time_in_idle {
	u64 time;
	u64 timestamp;
};

/**
 * struct cpufreq_cooling_device - data for cooling device with cpufreq
 * @id: unique integer value corresponding to each cpufreq_cooling_device
 *	registered.
 * @cpu_id: The CPU for which the cooling device will do the mitigation.
 * @last_load: load measured by the latest call to cpufreq_get_requested_power()
 * @cpufreq_state: integer value representing the current state of cpufreq
 *	cooling	devices.
 * @clipped_freq: integer value representing the absolute value of the clipped
 *	frequency.
 * @cpufreq_floor_state: integer value representing the frequency floor state
 *	of cpufreq cooling devices.
 * @floor_freq: integer value representing the absolute value of the floor
 *	frequency.
 * @max_level: maximum cooling level. One less than total number of valid
 *	cpufreq frequencies.
 * @freq_table: Freq table in descending order of frequencies
 * @cdev: thermal_cooling_device pointer to keep track of the
 *	registered cooling device.
 * @policy: cpufreq policy.
 * @node: list_head to link all cpufreq_cooling_device together.
 * @idle_time: idle time stats
 * @plat_get_static_power: callback to calculate the static power
 *
 * This structure is required for keeping information of each registered
 * cpufreq_cooling_device.
 */
struct cpufreq_cooling_device {
	int id;
	u32 last_load;
	int cpu_id;
	unsigned int cpufreq_state;
	unsigned int clipped_freq;
	unsigned int cpufreq_floor_state;
	unsigned int floor_freq;
	unsigned int max_level;
	struct freq_table *freq_table;	/* In descending order */
	struct thermal_cooling_device *cdev;
	struct cpufreq_policy *policy;
	struct list_head node;
	struct time_in_idle *idle_time;
	get_static_t plat_get_static_power;
	struct cpu_cooling_ops *plat_ops;
};

static atomic_t in_suspend;
static int8_t cpuhp_registered;
static struct work_struct cpuhp_register_work;
static struct cpumask cpus_pending_online;
static struct cpumask cpus_isolated_by_thermal;
static DEFINE_MUTEX(core_isolate_lock);

static DEFINE_IDA(cpufreq_ida);
static DEFINE_MUTEX(cooling_list_lock);
static LIST_HEAD(cpufreq_cdev_list);

static struct cpumask cpus_in_max_cooling_level;
static BLOCKING_NOTIFIER_HEAD(cpu_max_cooling_level_notifer);

void cpu_cooling_max_level_notifier_register(struct notifier_block *n)
{
	blocking_notifier_chain_register(&cpu_max_cooling_level_notifer, n);
}

void cpu_cooling_max_level_notifier_unregister(struct notifier_block *n)
{
	blocking_notifier_chain_unregister(&cpu_max_cooling_level_notifer, n);
}

const struct cpumask *cpu_cooling_get_max_level_cpumask(void)
{
	return &cpus_in_max_cooling_level;
}

/* Below code defines functions to be used for cpufreq as cooling device */

/**
 * get_level: Find the level for a particular frequency
 * @cpufreq_cdev: cpufreq_cdev for which the property is required
 * @freq: Frequency
 *
 * Return: level corresponding to the frequency.
 */
static unsigned long get_level(struct cpufreq_cooling_device *cpufreq_cdev,
			       unsigned int freq)
{
	struct freq_table *freq_table = cpufreq_cdev->freq_table;
	unsigned long level;

	for (level = 1; level <= cpufreq_cdev->max_level; level++)
		if (freq > freq_table[level].frequency)
			break;

	return level - 1;
}

static int cpufreq_cooling_pm_notify(struct notifier_block *nb,
				unsigned long mode, void *_unused)
{
	struct cpufreq_cooling_device *cpufreq_cdev, *next;
	unsigned int cpu;

	switch (mode) {
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
	case PM_SUSPEND_PREPARE:
		atomic_set(&in_suspend, 1);
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		list_for_each_entry_safe(cpufreq_cdev, next, &cpufreq_cdev_list,
						node) {
			if (cpufreq_cdev->cpu_id == -1)
				continue;
			mutex_lock(&core_isolate_lock);
			if (cpufreq_cdev->cpufreq_state ==
				cpufreq_cdev->max_level) {
				cpu = cpufreq_cdev->cpu_id;
				/*
				 * Unlock this lock before calling
				 * schedule_isolate, as this could lead to
				 * deadlock with hotplug path.
				 */
				mutex_unlock(&core_isolate_lock);
				if (cpu_online(cpu) &&
					!cpumask_test_and_set_cpu(cpu,
					&cpus_isolated_by_thermal)) {
					if (sched_isolate_cpu(cpu))
						cpumask_clear_cpu(cpu,
						&cpus_isolated_by_thermal);
				}
				continue;
			}
			mutex_unlock(&core_isolate_lock);
		}

		atomic_set(&in_suspend, 0);
		break;
	default:
		break;
	}
	return 0;
}

static struct notifier_block cpufreq_cooling_pm_nb = {
	.notifier_call = cpufreq_cooling_pm_notify,
};

static int cpufreq_hp_offline(unsigned int offline_cpu)
{
	struct cpufreq_cooling_device *cpufreq_cdev;

	mutex_lock(&cooling_list_lock);
	list_for_each_entry(cpufreq_cdev, &cpufreq_cdev_list, node) {
		if (!cpumask_test_cpu(offline_cpu,
					cpufreq_cdev->policy->related_cpus))
			continue;
		if (cpufreq_cdev->cpu_id != -1 &&
				offline_cpu != cpufreq_cdev->cpu_id)
			continue;

		mutex_lock(&core_isolate_lock);
		if ((cpufreq_cdev->cpufreq_state == cpufreq_cdev->max_level) &&
			(cpumask_test_and_clear_cpu(offline_cpu,
			&cpus_isolated_by_thermal)))
			sched_unisolate_cpu_unlocked(offline_cpu);
		mutex_unlock(&core_isolate_lock);
		break;
	}
	mutex_unlock(&cooling_list_lock);

	return 0;
}

static int cpufreq_hp_online(unsigned int online_cpu)
{
	struct cpufreq_cooling_device *cpufreq_cdev;
	int ret = 0;

	if (atomic_read(&in_suspend))
		return 0;

	mutex_lock(&cooling_list_lock);
	list_for_each_entry(cpufreq_cdev, &cpufreq_cdev_list, node) {
		if (!cpumask_test_cpu(online_cpu,
					cpufreq_cdev->policy->related_cpus))
			continue;
		if (cpufreq_cdev->cpu_id != -1 &&
				online_cpu != cpufreq_cdev->cpu_id)
			continue;

		mutex_lock(&core_isolate_lock);
		if (cpufreq_cdev->cpufreq_state == cpufreq_cdev->max_level) {
			cpumask_set_cpu(online_cpu, &cpus_pending_online);
			ret = NOTIFY_BAD;
		}
		mutex_unlock(&core_isolate_lock);
		break;
	}
	mutex_unlock(&cooling_list_lock);

	return ret;
}

/**
 * cpufreq_thermal_notifier - notifier callback for cpufreq policy change.
 * @nb:	struct notifier_block * with callback info.
 * @event: value showing cpufreq event for which this function invoked.
 * @data: callback-specific data
 *
 * Callback to hijack the notification on cpufreq policy transition.
 * Every time there is a change in policy, we will intercept and
 * update the cpufreq policy with thermal constraints.
 *
 * Return: 0 (success)
 */
static int cpufreq_thermal_notifier(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned long clipped_freq = ULONG_MAX, floor_freq = 0;
	struct cpufreq_cooling_device *cpufreq_cdev;

	if (event != CPUFREQ_INCOMPATIBLE)
		return NOTIFY_DONE;

	mutex_lock(&cooling_list_lock);
	list_for_each_entry(cpufreq_cdev, &cpufreq_cdev_list, node) {
		if (!cpumask_intersects(cpufreq_cdev->policy->related_cpus,
					policy->related_cpus))
			continue;
		if (cpufreq_cdev->clipped_freq < clipped_freq)
			clipped_freq = cpufreq_cdev->clipped_freq;
		if (cpufreq_cdev->floor_freq > floor_freq)
			floor_freq = cpufreq_cdev->floor_freq;
	}

	if (policy->max > clipped_freq || policy->min < floor_freq)
		cpufreq_verify_within_limits(policy, floor_freq, clipped_freq);
	mutex_unlock(&cooling_list_lock);

	return NOTIFY_OK;
}

/**
 * update_freq_table() - Update the freq table with power numbers
 */
static int update_freq_table(struct cpufreq_cooling_device *cpufreq_cdev,
			     u32 capacitance)
{
	struct freq_table *freq_table = cpufreq_cdev->freq_table;
	struct dev_pm_opp *opp;
	struct device *dev = NULL;
	int num_opps = 0, cpu = cpufreq_cdev->policy->cpu, i;

	dev = get_cpu_device(cpu);
	if (unlikely(!dev)) {
		dev_warn(&cpufreq_cdev->cdev->device,
			 "No cpu device for cpu %d\n", cpu);
		return -ENODEV;
	}

	num_opps = dev_pm_opp_get_opp_count(dev);
	if (num_opps < 0)
		return num_opps;

	if (num_opps != cpufreq_cdev->max_level + 1) {
		dev_warn(dev, "Number of OPPs not matching with max_levels\n");
		return -EINVAL;
	}

	for (i = 0; i <= cpufreq_cdev->max_level; i++) {
		unsigned long freq = freq_table[i].frequency * 1000;
		u32 freq_mhz = freq_table[i].frequency / 1000;
		u64 power;
		u32 voltage_mv;

		opp = dev_pm_opp_find_freq_ceil(dev, &freq);
		if (IS_ERR(opp)) {
			dev_err(dev, "failed to get opp for %lu frequency\n",
				freq);
			return -EINVAL;
		}

		voltage_mv = dev_pm_opp_get_voltage(opp) / 1000;
		dev_pm_opp_put(opp);

		power = (u64)capacitance * freq_mhz * voltage_mv * voltage_mv;
		do_div(power, 1000000000);

		freq_table[i].power = power;
	}

	return 0;
}

static u32 cpu_freq_to_power(struct cpufreq_cooling_device *cpufreq_cdev,
			     u32 freq)
{
	int i;
	struct freq_table *freq_table = cpufreq_cdev->freq_table;

	for (i = 1; i <= cpufreq_cdev->max_level; i++)
		if (freq > freq_table[i].frequency)
			break;

	return freq_table[i - 1].power;
}

static u32 cpu_power_to_freq(struct cpufreq_cooling_device *cpufreq_cdev,
			     u32 power)
{
	int i;
	struct freq_table *freq_table = cpufreq_cdev->freq_table;

	for (i = 0; i < cpufreq_cdev->max_level; i++)
		if (power >= freq_table[i].power)
			break;

	return freq_table[i].frequency;
}

static u32 get_load(struct cpufreq_cooling_device *cpufreq_cdev, int cpu,
		    int cpu_idx)
{
	u32 load;
	u64 now, now_idle, delta_time, delta_idle;
	struct time_in_idle *idle_time = &cpufreq_cdev->idle_time[cpu_idx];

	now_idle = get_cpu_idle_time(cpu, &now, 0);
	delta_idle = now_idle - idle_time->time;
	delta_time = now - idle_time->timestamp;

	if (delta_time <= delta_idle)
		load = 0;
	else
		load = div64_u64(100 * (delta_time - delta_idle), delta_time);

	idle_time->time = now_idle;
	idle_time->timestamp = now;

	return load;
}

static int get_static_power(struct cpufreq_cooling_device *cpufreq_cdev,
			    struct thermal_zone_device *tz, unsigned long freq,
			    u32 *power)
{
	struct dev_pm_opp *opp;
	unsigned long voltage;
	struct cpufreq_policy *policy = cpufreq_cdev->policy;
	struct cpumask *cpumask = policy->related_cpus;
	unsigned long freq_hz = freq * 1000;
	struct device *dev;

	if (!cpufreq_cdev->plat_get_static_power) {
		*power = 0;
		return 0;
	}

	dev = get_cpu_device(policy->cpu);
	WARN_ON(!dev);

	opp = dev_pm_opp_find_freq_exact(dev, freq_hz, true);
	if (IS_ERR(opp)) {
		dev_warn_ratelimited(dev, "Failed to find OPP for frequency %lu: %ld\n",
				     freq_hz, PTR_ERR(opp));
		return -EINVAL;
	}

	voltage = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	if (voltage == 0) {
		dev_err_ratelimited(dev, "Failed to get voltage for frequency %lu\n",
				    freq_hz);
		return -EINVAL;
	}

	return cpufreq_cdev->plat_get_static_power(cpumask, tz->passive_delay,
						  voltage, power);
}

static u32 get_dynamic_power(struct cpufreq_cooling_device *cpufreq_cdev,
			     unsigned long freq)
{
	u32 raw_cpu_power;

	raw_cpu_power = cpu_freq_to_power(cpufreq_cdev, freq);
	return (raw_cpu_power * cpufreq_cdev->last_load) / 100;
}

static int cpufreq_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;

	*state = cpufreq_cdev->max_level;
	return 0;
}

static int cpufreq_get_min_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;

	*state = cpufreq_cdev->cpufreq_floor_state;

	return 0;
}

static int cpufreq_set_min_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;
	unsigned int cpu = cpufreq_cdev->policy->cpu;
	unsigned int floor_freq;

	if (state > cpufreq_cdev->max_level)
		state = cpufreq_cdev->max_level;

	if (cpufreq_cdev->cpufreq_floor_state == state)
		return 0;

	cpufreq_cdev->cpufreq_floor_state = state;

	if (cpufreq_cdev->plat_ops &&
		cpufreq_cdev->plat_ops->floor_limit) {
		if (state == cpufreq_cdev->max_level)
			state--;
		floor_freq = cpufreq_cdev->freq_table[state].frequency;
		cpufreq_cdev->floor_freq = floor_freq;
		cpufreq_cdev->plat_ops->floor_limit(cpu, floor_freq);
	} else {
		floor_freq = cpufreq_cdev->freq_table[state].frequency;
		cpufreq_cdev->floor_freq = floor_freq;
		cpufreq_update_policy(cpu);
	}

	return 0;
}

static int cpufreq_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;

	*state = cpufreq_cdev->cpufreq_state;

	return 0;
}

/**
 * cpufreq_set_cur_state - Calibrated for sustained 60 FPS esports
 */
static int cpufreq_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;
	unsigned int clip_freq;
	unsigned long prev_state;
	struct device *cpu_dev;
	int ret = 0;
	int cpu = 0;

	/* Request state should be less than max_level */
	if (WARN_ON(state > cpufreq_cdev->max_level))
		return -EINVAL;

	/* Check if the old cooling action is same as new cooling action */
	if (cpufreq_cdev->cpufreq_state == state)
		return 0;

	mutex_lock(&core_isolate_lock);
	cpu = (cpufreq_cdev->cpu_id == -1) ?
		cpufreq_cdev->policy->cpu : cpufreq_cdev->cpu_id;
	prev_state = cpufreq_cdev->cpufreq_state;
	cpufreq_cdev->cpufreq_state = state;
	mutex_unlock(&core_isolate_lock);

	/*
	 * Esports Optimization 1: NEVER isolate Big Kryo Gold cores (CPU 6, 7).
	 * Isolate prevention keeps dual high-performance cores active for 60 FPS.
	 */
	if (state == cpufreq_cdev->max_level && cpu >= 6) {
		state = (cpufreq_cdev->max_level > 1) ? cpufreq_cdev->max_level - 1 : 0;
		goto update_frequency;
	}

	/* If state is the last, isolate the Little CPU */
	if (state == cpufreq_cdev->max_level) {
		if (cpu_online(cpu) &&
			(!cpumask_test_and_set_cpu(cpu,
			&cpus_isolated_by_thermal))) {
			if (sched_isolate_cpu(cpu))
				cpumask_clear_cpu(cpu,
					&cpus_isolated_by_thermal);
		}
		cpumask_set_cpu(cpu, &cpus_in_max_cooling_level);
		blocking_notifier_call_chain(&cpu_max_cooling_level_notifer,
					     1, (void *)(long)cpu);
		return ret;
	} else if ((prev_state == cpufreq_cdev->max_level)
			&& (state < cpufreq_cdev->max_level)) {
		if (cpumask_test_and_clear_cpu(cpu, &cpus_pending_online)) {
			cpu_dev = get_cpu_device(cpu);
			ret = device_online(cpu_dev);
			if (ret)
				pr_err("CPU:%d online error:%d\n", cpu, ret);
			goto update_frequency;
		} else if (cpumask_test_and_clear_cpu(cpu,
			&cpus_isolated_by_thermal)) {
			sched_unisolate_cpu(cpu);
		}
		cpumask_clear_cpu(cpu, &cpus_in_max_cooling_level);
		blocking_notifier_call_chain(&cpu_max_cooling_level_notifer,
					     0, (void *)(long)cpu);
	}
update_frequency:
	clip_freq = cpufreq_cdev->freq_table[state].frequency;

	/*
	 * Esports Optimization 2: Sustained Thermal Floor (65°C - 70°C target)
	 * Big Kryo Gold (CPU 6-7): Never throttled below 1.74 GHz (1747200 kHz)
	 * Little Kryo Silver (CPU 0-5): Never throttled below 1.34 GHz (1344000 kHz)
	 * Eliminates mid-game thermal collapse while keeping temperatures manageable.
	 */
	if (cpu >= 6 && clip_freq < 1747200)
		clip_freq = 1747200;
	else if (cpu < 6 && clip_freq < 1344000)
		clip_freq = 1344000;

	cpufreq_cdev->cpufreq_state = state;
	cpufreq_cdev->clipped_freq = clip_freq;

	/* Notify cpufreq framework of the bounded thermal limit */
	if (cpufreq_cdev->plat_ops) {
		if (cpufreq_cdev->plat_ops->ceil_limit)
			cpufreq_cdev->plat_ops->ceil_limit(cpu,
						clip_freq);
	} else {
		cpufreq_update_policy(cpu);
	}

	return 0;
}

static int cpufreq_get_requested_power(struct thermal_cooling_device *cdev,
				       struct thermal_zone_device *tz,
				       u32 *power)
{
	unsigned long freq;
	int i = 0, cpu, ret;
	u32 static_power, dynamic_power, total_load = 0;
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;
	struct cpufreq_policy *policy = cpufreq_cdev->policy;
	u32 *load_cpu = NULL;

	freq = cpufreq_quick_get(policy->cpu);

	if (trace_thermal_power_cpu_get_power_enabled()) {
		u32 ncpus = cpumask_weight(policy->related_cpus);

		load_cpu = kcalloc(ncpus, sizeof(*load_cpu), GFP_KERNEL);
	}

	for_each_cpu(cpu, policy->related_cpus) {
		u32 load;

		if (cpu_online(cpu))
			load = get_load(cpufreq_cdev, cpu, i);
		else
			load = 0;

		total_load += load;
		if (load_cpu)
			load_cpu[i] = load;

		i++;
	}

	cpufreq_cdev->last_load = total_load;

	dynamic_power = get_dynamic_power(cpufreq_cdev, freq);
	ret = get_static_power(cpufreq_cdev, tz, freq, &static_power);
	if (ret) {
		kfree(load_cpu);
		return ret;
	}

	if (load_cpu) {
		trace_thermal_power_cpu_get_power(policy->related_cpus, freq,
						  load_cpu, i, dynamic_power,
						  static_power);

		kfree(load_cpu);
	}

	*power = static_power + dynamic_power;
	return 0;
}

static int cpufreq_state2power(struct thermal_cooling_device *cdev,
			       struct thermal_zone_device *tz,
			       unsigned long state, u32 *power)
{
	unsigned int freq, num_cpus;
	u32 static_power, dynamic_power;
	int ret;
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;

	if (WARN_ON(state > cpufreq_cdev->max_level))
		return -EINVAL;

	num_cpus = cpumask_weight(cpufreq_cdev->policy->cpus);

	freq = cpufreq_cdev->freq_table[state].frequency;
	dynamic_power = cpu_freq_to_power(cpufreq_cdev, freq) * num_cpus;
	ret = get_static_power(cpufreq_cdev, tz, freq, &static_power);
	if (ret)
		return ret;

	*power = static_power + dynamic_power;
	return ret;
}

static int cpufreq_power2state(struct thermal_cooling_device *cdev,
			       struct thermal_zone_device *tz, u32 power,
			       unsigned long *state)
{
	unsigned int cur_freq, target_freq;
	int ret;
	s32 dyn_power;
	u32 last_load, normalised_power, static_power;
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;
	struct cpufreq_policy *policy = cpufreq_cdev->policy;

	cur_freq = cpufreq_quick_get(policy->cpu);
	ret = get_static_power(cpufreq_cdev, tz, cur_freq, &static_power);
	if (ret)
		return ret;

	dyn_power = power - static_power;
	dyn_power = dyn_power > 0 ? dyn_power : 0;
	last_load = cpufreq_cdev->last_load ?: 1;
	normalised_power = (dyn_power * 100) / last_load;
	target_freq = cpu_power_to_freq(cpufreq_cdev, normalised_power);

	*state = get_level(cpufreq_cdev, target_freq);
	trace_thermal_power_cpu_limit(policy->related_cpus, target_freq, *state,
				      power);
	return 0;
}

static struct thermal_cooling_device_ops cpufreq_cooling_ops = {
	.get_max_state = cpufreq_get_max_state,
	.get_cur_state = cpufreq_get_cur_state,
	.set_cur_state = cpufreq_set_cur_state,
	.set_min_state = cpufreq_set_min_state,
	.get_min_state = cpufreq_get_min_state,
};

static struct thermal_cooling_device_ops cpufreq_power_cooling_ops = {
	.get_max_state		= cpufreq_get_max_state,
	.get_cur_state		= cpufreq_get_cur_state,
	.set_cur_state		= cpufreq_set_cur_state,
	.get_requested_power	= cpufreq_get_requested_power,
	.state2power		= cpufreq_state2power,
	.power2state		= cpufreq_power2state,
};

static struct notifier_block thermal_cpufreq_notifier_block = {
	.notifier_call = cpufreq_thermal_notifier,
};

static unsigned int find_next_max(struct cpufreq_frequency_table *table,
				  unsigned int prev_max)
{
	struct cpufreq_frequency_table *pos;
	unsigned int max = 0;

	cpufreq_for_each_valid_entry(pos, table) {
		if (pos->frequency > max && pos->frequency < prev_max)
			max = pos->frequency;
	}

	return max;
}

static void register_cdev(struct work_struct *work)
{
	int ret = 0;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				"cpu_cooling/no-sched",	cpufreq_hp_online,
				cpufreq_hp_offline);
	if (ret < 0)
		pr_err("Error registering for hotplug callback:%d\n", ret);
}

static struct thermal_cooling_device *
__cpufreq_cooling_register(struct device_node *np,
			struct cpufreq_policy *policy, u32 capacitance,
			get_static_t plat_static_func,
			struct cpu_cooling_ops *plat_ops)
{
	struct thermal_cooling_device *cdev;
	struct cpufreq_cooling_device *cpufreq_cdev;
	char dev_name[THERMAL_NAME_LENGTH];
	unsigned int freq, i, num_cpus, cpu_idx;
	int ret;
	struct thermal_cooling_device_ops *cooling_ops;
	bool first;

	if (IS_ERR_OR_NULL(policy)) {
		pr_err("%s: cpufreq policy isn't valid: %p", __func__, policy);
		return ERR_PTR(-EINVAL);
	}

	i = cpufreq_table_count_valid_entries(policy);
	if (!i) {
		pr_debug("%s: CPUFreq table not found or has no valid entries\n",
			 __func__);
		return ERR_PTR(-ENODEV);
	}

	cpufreq_cdev = kzalloc(sizeof(*cpufreq_cdev), GFP_KERNEL);
	if (!cpufreq_cdev)
		return ERR_PTR(-ENOMEM);

	cpufreq_cdev->policy = policy;
	num_cpus = cpumask_weight(policy->related_cpus);
	cpufreq_cdev->idle_time = kcalloc(num_cpus,
					 sizeof(*cpufreq_cdev->idle_time),
					 GFP_KERNEL);
	if (!cpufreq_cdev->idle_time) {
		cdev = ERR_PTR(-ENOMEM);
		goto free_cdev;
	}
	cpufreq_cdev->cpu_id = -1;
	for_each_cpu(cpu_idx, policy->related_cpus) {
		if (np == of_cpu_device_node_get(cpu_idx)) {
			cpufreq_cdev->cpu_id = cpu_idx;
			break;
		}
	}

	cpufreq_cdev->max_level = i + 1;

	cpufreq_cdev->freq_table = kmalloc_array(cpufreq_cdev->max_level,
					sizeof(*cpufreq_cdev->freq_table),
					GFP_KERNEL);
	if (!cpufreq_cdev->freq_table) {
		cdev = ERR_PTR(-ENOMEM);
		goto free_idle_time;
	}
	cpufreq_cdev->max_level--;

	ret = ida_simple_get(&cpufreq_ida, 0, 0, GFP_KERNEL);
	if (ret < 0) {
		cdev = ERR_PTR(ret);
		goto free_table;
	}
	cpufreq_cdev->id = ret;

	snprintf(dev_name, sizeof(dev_name), "thermal-cpufreq-%d",
		 cpufreq_cdev->id);

	for (i = 0, freq = -1; i < cpufreq_cdev->max_level; i++) {
		freq = find_next_max(policy->freq_table, freq);
		cpufreq_cdev->freq_table[i].frequency = freq;
	}

	cpufreq_cdev->freq_table[cpufreq_cdev->max_level].frequency = 0;

	if (capacitance) {
		cpufreq_cdev->plat_get_static_power = plat_static_func;

		ret = update_freq_table(cpufreq_cdev, capacitance);
		if (ret) {
			cdev = ERR_PTR(ret);
			goto remove_ida;
		}

		cooling_ops = &cpufreq_power_cooling_ops;
	} else {
		cooling_ops = &cpufreq_cooling_ops;
	}

	cpufreq_cdev->plat_ops = plat_ops;

	cdev = thermal_of_cooling_device_register(np, dev_name, cpufreq_cdev,
						  cooling_ops);
	if (IS_ERR(cdev))
		goto remove_ida;

	cpufreq_cdev->clipped_freq = cpufreq_cdev->freq_table[0].frequency;
	cpufreq_cdev->floor_freq =
		cpufreq_cdev->freq_table[cpufreq_cdev->max_level].frequency;
	cpufreq_cdev->cpufreq_floor_state = cpufreq_cdev->max_level;
	cpufreq_cdev->cdev = cdev;

	mutex_lock(&cooling_list_lock);
	first = list_empty(&cpufreq_cdev_list);
	list_add(&cpufreq_cdev->node, &cpufreq_cdev_list);
	mutex_unlock(&cooling_list_lock);

	if (first && !cpufreq_cdev->plat_ops)
		cpufreq_register_notifier(&thermal_cpufreq_notifier_block,
					  CPUFREQ_POLICY_NOTIFIER);
	if (!cpuhp_registered) {
		cpuhp_registered = 1;
		register_pm_notifier(&cpufreq_cooling_pm_nb);
		cpumask_clear(&cpus_pending_online);
		cpumask_clear(&cpus_isolated_by_thermal);
		cpumask_clear(&cpus_in_max_cooling_level);
		INIT_WORK(&cpuhp_register_work, register_cdev);
		queue_work(system_wq, &cpuhp_register_work);
	}

	return cdev;

remove_ida:
	ida_simple_remove(&cpufreq_ida, cpufreq_cdev->id);
free_table:
	kfree(cpufreq_cdev->freq_table);
free_idle_time:
	kfree(cpufreq_cdev->idle_time);
free_cdev:
	kfree(cpufreq_cdev);
	return cdev;
}

struct thermal_cooling_device *
cpufreq_cooling_register(struct cpufreq_policy *policy)
{
	return __cpufreq_cooling_register(NULL, policy, 0, NULL, NULL);
}
EXPORT_SYMBOL_GPL(cpufreq_cooling_register);

struct thermal_cooling_device *
of_cpufreq_cooling_register(struct device_node *np,
			    struct cpufreq_policy *policy)
{
	if (!np)
		return ERR_PTR(-EINVAL);

	return __cpufreq_cooling_register(np, policy, 0, NULL, NULL);
}
EXPORT_SYMBOL_GPL(of_cpufreq_cooling_register);

struct thermal_cooling_device *
cpufreq_power_cooling_register(struct cpufreq_policy *policy, u32 capacitance,
			       get_static_t plat_static_func)
{
	return __cpufreq_cooling_register(NULL, policy, capacitance,
				plat_static_func, NULL);
}
EXPORT_SYMBOL(cpufreq_power_cooling_register);

struct thermal_cooling_device *
cpufreq_platform_cooling_register(const struct cpumask *clip_cpus,
				struct cpu_cooling_ops *plat_ops)
{
	struct device_node *cpu_node;
	struct cpufreq_policy *policy;

	cpu_node = of_cpu_device_node_get(cpumask_first(clip_cpus));
	if (!cpu_node) {
		pr_err("No cpu node\n");
		return ERR_PTR(-EINVAL);
	}
	policy = cpufreq_cpu_get(cpumask_first(clip_cpus));
	if (!policy) {
		pr_err("no policy for cpu%d\n", cpumask_first(clip_cpus));
		return ERR_PTR(-EINVAL);
	}

	return __cpufreq_cooling_register(cpu_node, policy, 0, NULL,
			plat_ops);
}
EXPORT_SYMBOL(cpufreq_platform_cooling_register);

struct thermal_cooling_device *
of_cpufreq_power_cooling_register(struct device_node *np,
				  struct cpufreq_policy *policy,
				  u32 capacitance,
				  get_static_t plat_static_func)
{
	if (!np)
		return ERR_PTR(-EINVAL);

	return __cpufreq_cooling_register(np, policy, capacitance,
				plat_static_func, NULL);
}
EXPORT_SYMBOL(of_cpufreq_power_cooling_register);

void cpufreq_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct cpufreq_cooling_device *cpufreq_cdev;
	bool last;

	if (!cdev)
		return;

	cpufreq_cdev = cdev->devdata;

	mutex_lock(&cooling_list_lock);
	list_del(&cpufreq_cdev->node);
	last = list_empty(&cpufreq_cdev_list);
	mutex_unlock(&cooling_list_lock);

	if (last) {
		unregister_pm_notifier(&cpufreq_cooling_pm_nb);
		if (!cpufreq_cdev->plat_ops)
			cpufreq_unregister_notifier(
					&thermal_cpufreq_notifier_block,
					CPUFREQ_POLICY_NOTIFIER);
	}

	thermal_cooling_device_unregister(cpufreq_cdev->cdev);
	ida_simple_remove(&cpufreq_ida, cpufreq_cdev->id);
	kfree(cpufreq_cdev->idle_time);
	kfree(cpufreq_cdev->freq_table);
	kfree(cpufreq_cdev);
}
EXPORT_SYMBOL_GPL(cpufreq_cooling_unregister);
