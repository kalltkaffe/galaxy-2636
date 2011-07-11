/*
 * drivers/video/tegra/host/nvhost_acm.c
 *
 * Tegra Graphics Host Automatic Clock Management
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "dev.h"
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/device.h>
#include <mach/powergate.h>
#include <mach/clk.h>
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
#include "nvhost_syncpt.h"
#endif

#include "dev.h"

#define ACM_TIMEOUT_MSEC 25
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
#define SUSPEND_TIMEOUT 6*HZ // As per __device_suspend timer.expires
#endif

#define DISABLE_MPE_POWERGATING

void nvhost_module_busy(struct nvhost_module *mod)
{
	mutex_lock(&mod->lock);
	cancel_delayed_work(&mod->powerdown);
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
	if (mod->force_suspend)
	{
		printk("tegra_grhost: module_busy despite %s force_suspend!\n",
			mod->name);
		dump_stack();
		// WARN_ON(1); // dump stack
	}	
#endif
	if ((atomic_inc_return(&mod->refcount) == 1) && !mod->powered) {
		if (mod->parent)
			nvhost_module_busy(mod->parent);
		if (mod->powergate_id != -1) {
			BUG_ON(mod->num_clks != 1);
			tegra_powergate_sequence_power_up(
				mod->powergate_id, mod->clk[0]);
		} else {
			int i;
			for (i = 0; i < mod->num_clks; i++)
				clk_enable(mod->clk[i]);
		}
		if (mod->func)
			mod->func(mod, NVHOST_POWER_ACTION_ON);
		mod->powered = true;
	}
	mutex_unlock(&mod->lock);
}

static void powerdown_handler(struct work_struct *work)
{
	struct nvhost_module *mod;
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
	int refcount;	
#endif
	mod = container_of(to_delayed_work(work), struct nvhost_module, powerdown);
	mutex_lock(&mod->lock);
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
	refcount = atomic_read(&mod->refcount);
	if ((refcount == 0) && mod->powered) {
#else
	if ((atomic_read(&mod->refcount) == 0) && mod->powered) {
#endif
		int i;
		if (mod->func)
			mod->func(mod, NVHOST_POWER_ACTION_OFF);
		for (i = 0; i < mod->num_clks; i++) {
			clk_disable(mod->clk[i]);
		}
		if (mod->powergate_id != -1) {
			tegra_periph_reset_assert(mod->clk[0]);
			tegra_powergate_power_off(mod->powergate_id);
		}
		mod->powered = false;
		if (mod->parent)
			nvhost_module_idle(mod->parent);
	}
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
	else if (mod->force_suspend) {
		printk("tegra_grhost: module %s (refcnt %d)"
			" force_suspend powerdown skipped!\n",
			mod->name, refcount);
	}
	mod->force_suspend = false;	
#endif
	mutex_unlock(&mod->lock);
}

void nvhost_module_idle_mult(struct nvhost_module *mod, int refs)
{
	bool kick = false;

	mutex_lock(&mod->lock);
	if (atomic_sub_return(refs, &mod->refcount) == 0) {
		BUG_ON(!mod->powered);
		schedule_delayed_work(
			&mod->powerdown, msecs_to_jiffies(ACM_TIMEOUT_MSEC));
		kick = true;
	}
	mutex_unlock(&mod->lock);

	if (kick)
		wake_up(&mod->idle);
}

static const char *get_module_clk_id(const char *module, int index)
{
	if (index == 1 && strcmp(module, "gr2d") == 0)
		return "epp";
	else if (index == 2 && strcmp(module, "gr2d") == 0)
		return "emc";
	else if (index == 1 && strcmp(module, "gr3d") == 0)
		return "emc";
	else if (index == 1 && strcmp(module, "mpe") == 0)
		return "emc";
	else if (index == 0)
		return module;
	return NULL;
}

static int get_module_powergate_id(const char *module)
{
	if (strcmp(module, "gr3d") == 0)
		return TEGRA_POWERGATE_3D;
	else if (strcmp(module, "mpe") == 0)
		return TEGRA_POWERGATE_MPE;
	return -1;
}

int nvhost_module_init(struct nvhost_module *mod, const char *name,
		nvhost_modulef func, struct nvhost_module *parent,
		struct device *dev)
{
	int i = 0;
	mod->name = name;

	while (i < NVHOST_MODULE_MAX_CLOCKS) {
		long rate;
		mod->clk[i] = clk_get(dev, get_module_clk_id(name, i));
		if (IS_ERR_OR_NULL(mod->clk[i]))
			break;
		rate = clk_round_rate(mod->clk[i], UINT_MAX);
		if (rate < 0) {
			pr_err("%s: can't get maximum rate for %s\n",
				__func__, name);
			break;
		}
		if (rate != clk_get_rate(mod->clk[i])) {
			clk_set_rate(mod->clk[i], rate);
		}
		i++;
	}

	mod->num_clks = i;
	mod->func = func;
	mod->parent = parent;
	mod->powered = false;
	mod->powergate_id = get_module_powergate_id(name);
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
	mod->force_suspend = false;
#endif

#if CONFIG_DISABLE_3D_POWERGATING
	/*
	 * It is possible for the 3d block to generate an invalid memory
	 * request during the power up sequence in some cases.  Workaround
	 * is to disable 3d block power gating.
	 */
	if (mod->powergate_id == TEGRA_POWERGATE_3D) {
		tegra_powergate_sequence_power_up(mod->powergate_id,
			mod->clk[0]);
		clk_disable(mod->clk[0]);
		mod->powergate_id = -1;
	}
#endif

#ifdef DISABLE_MPE_POWERGATING
	/*
	 * Disable power gating for MPE as it seems to cause issues with
	 * camera record stress tests when run in loop.
	 */
	if (mod->powergate_id == TEGRA_POWERGATE_MPE) {
		tegra_powergate_sequence_power_up(mod->powergate_id,
			mod->clk[0]);
		clk_disable(mod->clk[0]);
		mod->powergate_id = -1;
	}
#endif

	mutex_init(&mod->lock);
	init_waitqueue_head(&mod->idle);
	INIT_DELAYED_WORK(&mod->powerdown, powerdown_handler);

	return 0;
}

static int is_module_idle(struct nvhost_module *mod)
{
	int count;
	mutex_lock(&mod->lock);
	count = atomic_read(&mod->refcount);
	mutex_unlock(&mod->lock);
	return (count == 0);
}

static void debug_not_idle(struct nvhost_module *mod)
{
	int i;
	bool lock_released = true;
	struct nvhost_master *dev = container_of(mod, struct nvhost_master, mod);

	for (i = 0; i < NVHOST_NUMCHANNELS; i++) {
		struct nvhost_module *m = &dev->channels[i].mod;
		if (m->name)
			printk("tegra_grhost: %s: refcnt %d\n",
				m->name, atomic_read(&m->refcount));
	}

	for (i = 0; i < NV_HOST1X_SYNC_MLOCK_NUM; i++) {
		int c = atomic_read(&dev->cpuaccess.lock_counts[i]);
		if (c) {
			printk("tegra_grhost: lock id %d: refcnt %d\n", i, c);
			lock_released = false;
		}
	}
	if (lock_released)
		printk("tegra_grhost: all locks released\n");
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
	nvhost_debug_dump();
#endif
}

int nvhost_module_suspend(struct nvhost_module *mod, bool system_suspend)
{
#if defined(CONFIG_MACH_SAMSUNG_P4LTE) || defined(CONFIG_MACH_SAMSUNG_P4) || defined(CONFIG_MACH_SAMSUNG_P4WIFI) || defined(CONFIG_MACH_SAMSUNG_P5)
	int i = 0, idle_timeout;
	struct nvhost_master *dev;

	if (system_suspend) {
		idle_timeout = SUSPEND_TIMEOUT;
		dev = container_of(mod, struct nvhost_master, mod);
	}
	else {
		idle_timeout = ACM_TIMEOUT_MSEC;
		dev = container_of(mod, struct nvhost_channel, mod)->dev;
	}

	if (!wait_event_timeout(mod->idle, is_module_idle(mod),
				 idle_timeout)) {
		// Timeout occurred: clear refcnt forcibly
		mod->force_suspend = true;		
		for (i = 0; i < NVHOST_NUMCHANNELS; i++) {
			struct nvhost_module *m = &dev->channels[i].mod;
			if (m->name) {
				int refcount = atomic_read(&m->refcount);
				if (refcount != 0) {	
					printk("tegra_grhost: %s: force refcnt %d to zero\n",
						m->name, refcount);
					nvhost_module_idle_mult(m, refcount);
					flush_delayed_work(&m->powerdown);
				}
			}
		}
		wait_event(mod->idle, is_module_idle(mod)); // no timeout: failure fatal

	}

	if (system_suspend) {
		// Ensure that syncpoints are not stuck
		for (i = 0; i < NV_HOST1X_SYNCPT_NB_PTS; i++) {
			if (BIT(i) & ~NVSYNCPTS_HOST_MANAGED)
				continue;
			while (!nvhost_syncpt_min_eq_max(&dev->syncpt, i)) {
				printk("tegra_grhost: force syncpt id %d (%s) = %d + 1 : %d\n",
					i, nvhost_syncpt_name(i),
					nvhost_syncpt_read_min(&dev->syncpt, i),
					nvhost_syncpt_read_max(&dev->syncpt, i));
				nvhost_syncpt_incr_min(&dev->syncpt, i, 1);
			}
		}

		if (!is_module_idle(mod))
			debug_not_idle(mod);
		printk("tegra_grhost: entered idle\n");
	}
	
#else
	int ret;

	if (system_suspend && (!is_module_idle(mod)))
		debug_not_idle(mod);

	ret = wait_event_timeout(mod->idle, is_module_idle(mod),
			   msecs_to_jiffies(ACM_TIMEOUT_MSEC + 500));
	if (ret == 0)
		nvhost_debug_dump();

	if (system_suspend)
		printk("tegra_grhost: entered idle\n");

#endif
	flush_delayed_work(&mod->powerdown);
	if (system_suspend) {
		printk("tegra_grhost: flushed delayed work\n");

		if(mod->powered)
			return -EBUSY;
	}

	BUG_ON(mod->powered);
	return 0;
}

void nvhost_module_deinit(struct nvhost_module *mod)
{
	int i;
	nvhost_module_suspend(mod, false);
	for (i = 0; i < mod->num_clks; i++)
		clk_put(mod->clk[i]);
}
