/*
 * automatically hotplug/unplug multiple cpu cores
 * based on cpu load and suspend state
 *
 * based on the msm_mpdecision code by
 * Copyright (c) 2012-2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * Copyright (C) 2013-2014, Rauf Gungor, http://github.com/mrg666
 * rewrite to simplify and optimize, Jul. 2013, http://goo.gl/cdGw6x
 * optimize more, generalize for n cores, Sep. 2013, http://goo.gl/448qBz
 * generalize for all arch, rename as autosmp, Dec. 2013, http://goo.gl/x5oyhy
 *
 * Copyright (c) 2017, RyTek <rytek1128@outlook.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU
 * General Public License included with the Linux kernel or available
 * at www.gnu.org/licenses
 */

#include <linux/moduleparam.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/lcd_notify.h>

#define DEBUG 0

#define ASMP_TAG "AutoSMP: "
#define ASMP_STARTDELAY 20000
#define DELAY_CHECK 20

#if DEBUG
struct asmp_cpudata_t {
	long long unsigned int times_hotplugged;
};
static DEFINE_PER_CPU(struct asmp_cpudata_t, asmp_cpudata);
#endif

static struct delayed_work asmp_work;
static struct workqueue_struct *asmp_workq;
static struct notifier_block lcd_notifier_hook;

static struct asmp_param_struct {
	unsigned int delay;
	unsigned int max_cpus;
	unsigned int min_cpus;
	unsigned int cpufreq_up;
	unsigned int cpufreq_down;
	unsigned int cycle_up;
	unsigned int cycle_down;
} asmp_param = {
	.max_cpus = CONFIG_NR_CPUS,
	.min_cpus = 2,
	.cpufreq_up = 90,
	.cpufreq_down = 60,
	.cycle_up = 1,
	.cycle_down = 1,
};

static unsigned int cycle = 0;
static int enabled __read_mostly = 1;

static void reschedule_asmp_workq(void)
{
	queue_delayed_work(asmp_workq, &asmp_work,
			msecs_to_jiffies(DELAY_CHECK));
}

static void __ref asmp_work_fn(struct work_struct *work)
{
	int cpu, slow_cpu = 0;
	unsigned int rate, cpu0_rate, slow_rate = UINT_MAX, fast_rate;
	unsigned int max_rate, up_rate, down_rate;
	unsigned int nr_cpu_online;

	cycle++;

	/* get maximum possible freq for cpu0 and
	   calculate up/down limits */
	max_rate  = cpufreq_quick_get_max(0);
	up_rate   = asmp_param.cpufreq_up * max_rate / 100;
	down_rate = asmp_param.cpufreq_down * max_rate / 100;

	/* find current max and min cpu freq to estimate load */
	get_online_cpus();
	nr_cpu_online = num_online_cpus();
	cpu0_rate = cpufreq_quick_get(0);
	fast_rate = cpu0_rate;
	for_each_online_cpu(cpu)
		if (cpu) {
			rate = cpufreq_quick_get(cpu);
			if (rate <= slow_rate) {
				slow_cpu = cpu;
				slow_rate = rate;
			} else if (rate > fast_rate)
				fast_rate = rate;
		}
	put_online_cpus();
	if (cpu0_rate < slow_rate)
		slow_rate = cpu0_rate;

	/* hotplug one core if all online cores are over up_rate limit */
	if (slow_rate > up_rate) {
		if ((nr_cpu_online < asmp_param.max_cpus) &&
		    (cycle >= asmp_param.cycle_up)) {
			cpu = cpumask_next_zero(0, cpu_online_mask);
			if (!cpu_online(cpu))
				cpu_up(cpu);
			cycle = 0;
#if DEBUG
			pr_info(ASMP_TAG"CPU[%d] on\n", cpu);
#endif
		}
	/* unplug slowest core if all online cores are under down_rate limit */
	} else if (slow_cpu && (fast_rate < down_rate)) {
		if ((nr_cpu_online > asmp_param.min_cpus) &&
		    (cycle >= asmp_param.cycle_down)) {
			if (cpu_online(slow_cpu) && (slow_cpu))
				cpu_down(slow_cpu);
			cycle = 0;
#if DEBUG
			pr_info(ASMP_TAG"CPU[%d] off\n", slow_cpu);
			per_cpu(asmp_cpudata, cpu).times_hotplugged += 1;
#endif
		}
	} /* else do nothing */

	reschedule_asmp_workq();
}

static void __ref cpu_all_up(struct work_struct *work);
static DECLARE_WORK(cpu_all_up_work, cpu_all_up);

static void cpu_all_up(struct work_struct *work)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (cpu_online(cpu))
			continue;
		cpu_up(cpu);
	}
}

static void asmp_suspend(void)
{
	int cpu;

	/* unplug online cpu cores */
	for (cpu = nr_cpu_ids; cpu > -1; cpu--) {
		if (!cpu_online(cpu) || !cpu)
			continue;
		cpu_down(cpu);
	}

	/* suspend main work thread */
	if (enabled)
		cancel_delayed_work_sync(&asmp_work);

	pr_info(ASMP_TAG"suspended\n");
}

static void asmp_resume(void)
{
	/* hotplug offline cpu cores */
	schedule_work(&cpu_all_up_work);
	/* resume main work thread */
	if (enabled)
		reschedule_asmp_workq();

	pr_info(ASMP_TAG"resumed\n");
}

static int lcd_notifier_call(struct notifier_block *this,
                        unsigned long event, void *data)
{
	switch (event) {
		case LCD_EVENT_ON_START:
			asmp_resume();
			break;
		case LCD_EVENT_OFF_END:
			asmp_suspend();
			break;
		default:
			break;
	}

	return 0;
}

static int set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret, cpu;

	ret = param_set_bool(val, kp);
	if (enabled) {
		reschedule_asmp_workq();
		pr_info(ASMP_TAG"enabled\n");
	} else {
		cancel_delayed_work_sync(&asmp_work);
		schedule_work(&cpu_all_up_work);
		pr_info(ASMP_TAG"disabled\n");
	}
	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "hotplug/unplug cpu cores based on cpu load");

/***************************** SYSFS START *****************************/
#define define_one_global_ro(_name)					\
static struct global_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)

#define define_one_global_rw(_name)					\
static struct global_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)

struct kobject *asmp_kobject;

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", asmp_param.object);			\
}
show_one(max_cpus, max_cpus);
show_one(min_cpus, min_cpus);
show_one(cpufreq_up, cpufreq_up);
show_one(cpufreq_down, cpufreq_down);
show_one(cycle_up, cycle_up);
show_one(cycle_down, cycle_down);

#define store_one(file_name, object)					\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	asmp_param.object = input;					\
	return count;							\
}									\
define_one_global_rw(file_name);
store_one(max_cpus, max_cpus);
store_one(min_cpus, min_cpus);
store_one(cpufreq_up, cpufreq_up);
store_one(cpufreq_down, cpufreq_down);
store_one(cycle_up, cycle_up);
store_one(cycle_down, cycle_down);

static struct attribute *asmp_attributes[] = {
	&max_cpus.attr,
	&min_cpus.attr,
	&cpufreq_up.attr,
	&cpufreq_down.attr,
	&cycle_up.attr,
	&cycle_down.attr,
	NULL
};

static struct attribute_group asmp_attr_group = {
	.attrs = asmp_attributes,
	.name = "conf",
};
#if DEBUG
static ssize_t show_times_hotplugged(struct kobject *a,
					struct attribute *b, char *buf) {
	ssize_t len = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		len += sprintf(buf + len, "%i %llu\n", cpu,
			per_cpu(asmp_cpudata, cpu).times_hotplugged);
	}
	return len;
}
define_one_global_ro(times_hotplugged);

static struct attribute *asmp_stats_attributes[] = {
	&times_hotplugged.attr,
	NULL
};

static struct attribute_group asmp_stats_attr_group = {
	.attrs = asmp_stats_attributes,
	.name = "stats",
};
#endif
/****************************** SYSFS END ******************************/

static int __init asmp_init(void)
{
	unsigned int cpu;
	int rc;

	asmp_param.max_cpus = nr_cpu_ids;
#if DEBUG
	for_each_possible_cpu(cpu)
		per_cpu(asmp_cpudata, cpu).times_hotplugged = 0;
#endif

	asmp_workq = alloc_workqueue("asmp", WQ_HIGHPRI, 0);
	if (!asmp_workq)
		return -ENOMEM;
	INIT_DELAYED_WORK(&asmp_work, asmp_work_fn);
	if (enabled)
		queue_delayed_work(asmp_workq, &asmp_work,
				   msecs_to_jiffies(ASMP_STARTDELAY));

	lcd_notifier_hook.notifier_call = lcd_notifier_call;
	lcd_register_client(&lcd_notifier_hook);

	asmp_kobject = kobject_create_and_add("autosmp", kernel_kobj);
	if (asmp_kobject) {
		rc = sysfs_create_group(asmp_kobject, &asmp_attr_group);
		if (rc)
			pr_warn(ASMP_TAG"ERROR, create sysfs group");
#if DEBUG
		rc = sysfs_create_group(asmp_kobject, &asmp_stats_attr_group);
		if (rc)
			pr_warn(ASMP_TAG"ERROR, create sysfs stats group");
#endif
	} else
		pr_warn(ASMP_TAG"ERROR, create sysfs kobj");

	pr_info(ASMP_TAG"initialized\n");
	return 0;
}
late_initcall(asmp_init);
