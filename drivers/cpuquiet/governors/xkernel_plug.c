/*
 * Copyright (c) 2012 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <linux/kernel.h>
#include <linux/cpuquiet.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include "load_calc.h"

// from cpuquiet.c
extern unsigned int cpq_max_cpus(void);
extern unsigned int cpq_min_cpus(void);

#define X_PLUG_TAG	"[X-Plug]: "

typedef enum {
	DISABLED,
	IDLE,
	DOWN,
	UP,
} XPLUG_STATE;

static struct delayed_work xplug_work;
static struct kobject *xplug_kobject;

/* configurable parameters */
static unsigned int sample_rate = 150;		/* msec */

/* 1 - target_load; 2 - target_thermal; 3 - target_history; 4 - target_predict */
static unsigned int policy = 1;		
static void policy_function(void (*cpu_policy)(void))	{	cpu_policy();	}

/* target_load parameters */	
static unsigned int target_load = 60;
static void target_load_policy(void);

/* target_predict */
//static void target_predict_policy(void);

static XPLUG_STATE xplug_state;
static struct workqueue_struct *xplug_wq;

DEFINE_MUTEX(xplug_work_lock);

static void target_load_policy(void)	{

	unsigned int curr_load = report_load();
	static signed int check_count = 0;	
	int scaled_sampler = ((sample_rate * 25 * 5)/1000);

	if((curr_load) > target_load)	{
		check_count--;
	}
	else if((curr_load) < (target_load))
		check_count++;

	if(check_count >= (scaled_sampler*2))		{	
		if(num_online_cpus() > 1)
			printk(strcat(X_PLUG_TAG, "Going down\n"));
		xplug_state = DOWN;
		check_count = 0;
	}
	else if(check_count <= (-1 * scaled_sampler))	{
		if(num_online_cpus() != nr_cpu_ids)
			printk(strcat(X_PLUG_TAG, "Going up\n"));
		xplug_state = UP;
		check_count = 0;
	}
}

static void update_xplug_state(void)
{
	
	switch(policy)	{

	case 1 : policy_function(&target_load_policy);
		 break;

	case 4 : break;	
	
	}
}

static void xplug_work_func(struct work_struct *work)
{
	bool up = false;
	bool sample = false;
	unsigned int cpu = nr_cpu_ids;

	//static int cpu_load[10] = {0,0,0,0,0,0,0,0,0,1};

	mutex_lock(&xplug_work_lock);

	update_xplug_state();

	switch (xplug_state) {
	case DISABLED:
		break;
	case IDLE:
		sample = true;
		break;
	case UP:
		cpu = cpumask_next_zero(0, cpu_online_mask);
		up = true;
		sample = true;
		xplug_state = IDLE;
		break;
	case DOWN:
		cpu = get_lightest_loaded_cpu_n();
		sample = true;
		xplug_state = IDLE;
		break;
	default:
		pr_err("%s: invalid cpuquiet runnable governor state %d\n",
			__func__, xplug_state);
		break;
	}

	if (sample)
		queue_delayed_work(xplug_wq, &xplug_work,
					msecs_to_jiffies(sample_rate));

	if (cpu < nr_cpu_ids) {
		if (up)
			cpuquiet_wake_cpu(cpu);
		else
			cpuquiet_quiesence_cpu(cpu);
	}
	mutex_unlock(&xplug_work_lock);
}


CPQ_BASIC_ATTRIBUTE(sample_rate, 0644, uint);
CPQ_BASIC_ATTRIBUTE(policy, 0644, uint);
CPQ_BASIC_ATTRIBUTE(target_load, 0644, uint);

static struct attribute *xplug_attributes[] = {
	&sample_rate_attr.attr,
	&policy_attr.attr,
	&target_load_attr.attr,
	NULL,
};

static const struct sysfs_ops xplug_sysfs_ops = {
	.show = cpuquiet_auto_sysfs_show,
	.store = cpuquiet_auto_sysfs_store,
};

static struct kobj_type ktype_xplug = {
	.sysfs_ops = &xplug_sysfs_ops,
	.default_attrs = xplug_attributes,
};

static int xplug_sysfs(void)
{
	int err;

	xplug_kobject = kzalloc(sizeof *xplug_kobject,
				GFP_KERNEL);

	if (!xplug_kobject)
		return -ENOMEM;

	err = cpuquiet_kobject_init(xplug_kobject, &ktype_xplug,
				"xplug");

	if (err)
		kfree(xplug_kobject);

	return err;
}

static void xplug_device_busy(void)
{
	if (xplug_state != DISABLED) {
		xplug_state = DISABLED;
		cancel_delayed_work_sync(&xplug_work);
	}
}

static void xplug_device_free(void)
{
	if (xplug_state == DISABLED) {
		xplug_state = IDLE;
		xplug_work_func(NULL);
	}
}

static void xplug_stop(void)
{
	xplug_state = DISABLED;
	cancel_delayed_work_sync(&xplug_work);
	destroy_workqueue(xplug_wq);
	kobject_put(xplug_kobject);
}

static int xplug_start(void)
{
	int err;

	unsigned int cpu;

	err = xplug_sysfs();
	if (err)
		return err;

	xplug_wq = alloc_workqueue("cpuquiet-xplug", WQ_HIGHPRI, 0);
	if (!xplug_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&xplug_work, xplug_work_func);

	xplug_state = IDLE;
	xplug_work_func(NULL);

	for_each_possible_cpu(cpu)	{
		cpuquiet_wake_cpu(cpu);
	}

	return 0;
}

struct cpuquiet_governor xplug_governor = {
	.name		   	  = "X-Plug",
	.start			  = xplug_start,
	.device_free_notification = xplug_device_free,
	.device_busy_notification = xplug_device_busy,
	.stop			  = xplug_stop,
	.owner		   	  = THIS_MODULE,
};

static int __init init_xplug(void)
{
	return cpuquiet_register_governor(&xplug_governor);
}

static void __exit exit_xplug(void)
{
	cpuquiet_unregister_governor(&xplug_governor);
}

MODULE_LICENSE("GPL");
module_init(init_xplug);
module_exit(exit_xplug);
