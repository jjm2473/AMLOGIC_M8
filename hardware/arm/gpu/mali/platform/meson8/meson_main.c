/*
 * Copyright (C) 2010, 2012-2013 ARM Limited. All rights reserved.
 * 
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 * 
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/platform_device.h>
#include <linux/version.h>
#include <linux/pm.h>
#include <linux/module.h>            /* kernel module definitions */
#include <linux/ioport.h>            /* request_mem_region */
#include <mach/register.h>
#include <mach/irqs.h>
#include <mach/io.h>
#ifdef CONFIG_PM_RUNTIME
#include <linux/pm_runtime.h>
#endif
#include <asm/io.h>
#include <linux/mali/mali_utgard.h>
#include "mali_kernel_common.h"

#include "mali_scaling.h"
#include "mali_clock.h"
#include "common/mali_pmu.h"
#include "common/mali_osk_profiling.h"

/* Configure dvfs mode */
enum mali_scale_mode_t {
	MALI_PP_SCALING,
	MALI_PP_FS_SCALING,
	MALI_SCALING_DISABLE,
	MALI_TURBO_MODE,
	MALI_SCALING_STAGING,
	MALI_SCALING_MODE_MAX
};

static int  scaling_mode = MALI_PP_FS_SCALING;
module_param(scaling_mode, int, 0664);
MODULE_PARM_DESC(scaling_mode, "0 disable, 1 pp, 2 fs, 4 double");

static int  internal_param = 0x300000;
module_param(internal_param, int, 0664);
MODULE_PARM_DESC(internal_param, " team internal.");

static int reseted_for_turbo = 0;

static void mali_platform_device_release(struct device *device);
static void mali_platform_device_release(struct device *device);
static int mali_os_suspend(struct device *device);
static int mali_os_resume(struct device *device);
static int mali_os_freeze(struct device *device);
static int mali_os_thaw(struct device *device);
#ifdef CONFIG_PM_RUNTIME
static int mali_runtime_suspend(struct device *device);
static int mali_runtime_resume(struct device *device);
static int mali_runtime_idle(struct device *device);
#endif
void mali_gpu_utilization_callback(struct mali_gpu_utilization_data *data);

static struct resource mali_gpu_resources_m450[] =
{
#if 1
	MALI_GPU_RESOURCES_MALI450_MP6_PMU(IO_MALI_APB_PHY_BASE, INT_MALI_GP, INT_MALI_GP_MMU, 
				INT_MALI_PP0, INT_MALI_PP0_MMU, 
				INT_MALI_PP1, INT_MALI_PP1_MMU, 
				INT_MALI_PP2, INT_MALI_PP2_MMU, 
				INT_MALI_PP4, INT_MALI_PP4_MMU, 
				INT_MALI_PP5, INT_MALI_PP5_MMU,
				INT_MALI_PP6, INT_MALI_PP6_MMU, 
				INT_MALI_PP)
#else 
MALI_GPU_RESOURCES_MALI450_MP2_PMU(IO_MALI_APB_PHY_BASE, INT_MALI_GP, INT_MALI_GP_MMU, 
				INT_MALI_PP0, INT_MALI_PP0_MMU, 
				INT_MALI_PP1, INT_MALI_PP1_MMU, 
				INT_MALI_PP)
#endif
};

static struct dev_pm_ops mali_gpu_device_type_pm_ops =
{
	.suspend = mali_os_suspend,
	.resume = mali_os_resume,
	.freeze = mali_os_freeze,
	.thaw = mali_os_thaw,
#ifdef CONFIG_PM_RUNTIME
	.runtime_suspend = mali_runtime_suspend,
	.runtime_resume = mali_runtime_resume,
	.runtime_idle = mali_runtime_idle,
#endif
};

static struct device_type mali_gpu_device_device_type =
{
	.pm = &mali_gpu_device_type_pm_ops,
};

static struct platform_device mali_gpu_device =
{
	.name = MALI_GPU_NAME_UTGARD,
	.id = 0,
	.dev.release = mali_platform_device_release,
	/*
	 * We temporarily make use of a device type so that we can control the Mali power
	 * from within the mali.ko (since the default platform bus implementation will not do that).
	 * Ideally .dev.pm_domain should be used instead, as this is the new framework designed
	 * to control the power of devices.
	 */
	.dev.type = &mali_gpu_device_device_type, /* We should probably use the pm_domain instead of type on newer kernels */
};

static struct mali_gpu_device_data mali_gpu_data =
{
#ifdef CONFIG_ARCH_MESON8
	.shared_mem_size = 1024 * 1024 * 1024,
	.fb_start = 0x05000000,
#elif CONFIG_MESON_LOW_PLAT_OFFSET
	.shared_mem_size =CONFIG_MALI400_OS_MEMORY_SIZE * 1024 * 1024,
	.fb_start = 0x24000000,
#else
	.shared_mem_size =CONFIG_MALI400_OS_MEMORY_SIZE * 1024 * 1024,
	.fb_start = 0x84000000,
#endif
	.fb_size = 0x06000000,
	.utilization_interval = 500,
	.utilization_callback = mali_gpu_utilization_callback,
	.pmu_switch_delay = 0xFFFF, /* do not have to be this high on FPGA, but it is good for testing to have a delay */
};

int mali_platform_device_register(void)
{
	int err = -1;
	int num_pp_cores = MALI_PP_NUMBER;

	mali_clock_set(mali_dvfs_clk[mali_default_clock_step]);

	if (mali_gpu_data.shared_mem_size < 10) {
		MALI_DEBUG_PRINT(2, ("mali os memory didn't configered, set to default(512M)\n"));
		mali_gpu_data.shared_mem_size = 512 * 1024 *1024;
	}

	MALI_DEBUG_PRINT(4, ("mali_platform_device_register() called\n"));
	err = platform_device_add_resources(&mali_gpu_device, mali_gpu_resources_m450, 
					sizeof(mali_gpu_resources_m450) / sizeof(mali_gpu_resources_m450[0]));

	if (0 == err)
	{
		err = platform_device_add_data(&mali_gpu_device, &mali_gpu_data, sizeof(mali_gpu_data));
		if (0 == err)
		{
			/* Register the platform device */
			err = platform_device_register(&mali_gpu_device);
			if (0 == err)
			{
#ifdef CONFIG_PM_RUNTIME
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37))
				pm_runtime_set_autosuspend_delay(&(mali_gpu_device.dev), 1000);
				pm_runtime_use_autosuspend(&(mali_gpu_device.dev));
#endif
				pm_runtime_enable(&(mali_gpu_device.dev));
#endif

				MALI_DEBUG_ASSERT(0 < num_pp_cores);
				mali_core_scaling_init(num_pp_cores, mali_default_clock_step);

				return 0;
			}
		}

		platform_device_unregister(&mali_gpu_device);
	}
	return err;
}

void mali_platform_device_unregister(void)
{
	MALI_DEBUG_PRINT(4, ("mali_platform_device_unregister() called\n"));
	mali_core_scaling_term();
	platform_device_unregister(&mali_gpu_device);
}

static void mali_platform_device_release(struct device *device)
{
	MALI_DEBUG_PRINT(4, ("mali_platform_device_release() called\n"));
}

void mali_gpu_utilization_callback(struct mali_gpu_utilization_data *data)
{
	switch (scaling_mode) {
	case MALI_PP_FS_SCALING:
		reseted_for_turbo = 0;
		set_turbo_mode(0);
		mali_pp_fs_scaling_update(data);
		break;
	case MALI_PP_SCALING:
		reseted_for_turbo = 0;
		set_turbo_mode(0);
		mali_pp_scaling_update(data);
		break;
    case MALI_TURBO_MODE:
		set_turbo_mode(1);
		if (reseted_for_turbo == 0) {
				reseted_for_turbo = 1;
				reset_mali_scaling_stat();
		}
		break;

	case MALI_SCALING_STAGING:
		set_turbo_mode(0);
		mali_pp_scaling_staging(data);
		break;
	default:
		set_turbo_mode(0);
		reseted_for_turbo = 0;
	}
}

u32 get_mali_schel_mode(void)
{
	return scaling_mode;
}
void set_mali_schel_mode(u32 mode)
{
	MALI_DEBUG_ASSERT(mode < MALI_SCALING_MODE_MAX);
	if (mode >= MALI_SCALING_MODE_MAX)return;
	scaling_mode = mode;
	reset_mali_scaling_stat();
}

int mali_cri_pmu_on_off(u64 param)
{
	struct mali_pmu_core *pmu;
	pmu = mali_pmu_get_global_pmu_core();
	if (param == 0)
		mali_pmu_power_down_all(pmu);
	else 
		mali_pmu_power_up_all(pmu);
}

static int mali_os_suspend(struct device *device)
{
	int ret = 0;
	MALI_DEBUG_PRINT(4, ("mali_os_suspend() called\n"));

	flush_scaling_job();
	if (NULL != device->driver &&
	    NULL != device->driver->pm &&
	    NULL != device->driver->pm->suspend)
	{
		/* Need to notify Mali driver about this event */
		ret = device->driver->pm->suspend(device);
	}

	/* pmu off. Kasin... */
	enable_clock();
	mali_clock_critical(mali_cri_pmu_on_off, 0);
	disable_clock();

	return ret;
}

static int mali_os_resume(struct device *device)
{
	int ret = 0;

	MALI_DEBUG_PRINT(4, ("mali_os_resume() called\n"));

	/* clock scaling up. Kasin.. */
	enable_clock();
	mali_clock_critical(mali_cri_pmu_on_off, 1);
	if (NULL != device->driver &&
	    NULL != device->driver->pm &&
	    NULL != device->driver->pm->resume)
	{
		/* Need to notify Mali driver about this event */
		ret = device->driver->pm->resume(device);
	}

	return ret;
}

static int mali_os_freeze(struct device *device)
{
	int ret = 0;

	MALI_DEBUG_PRINT(4, ("mali_os_freeze() called\n"));

	if (NULL != device->driver &&
	    NULL != device->driver->pm &&
	    NULL != device->driver->pm->freeze)
	{
		/* Need to notify Mali driver about this event */
		ret = device->driver->pm->freeze(device);
	}

	return ret;
}

static int mali_os_thaw(struct device *device)
{
	int ret = 0;

	MALI_DEBUG_PRINT(4, ("mali_os_thaw() called\n"));

	if (NULL != device->driver &&
	    NULL != device->driver->pm &&
	    NULL != device->driver->pm->thaw)
	{
		/* Need to notify Mali driver about this event */
		ret = device->driver->pm->thaw(device);
	}

	return ret;
}

#ifdef CONFIG_PM_RUNTIME
static int mali_runtime_suspend(struct device *device)
{
	int ret = 0;

	MALI_DEBUG_PRINT(4, ("mali_runtime_suspend() called\n"));
#ifdef CONFIG_MALI400_PROFILING
	_mali_osk_profiling_add_event(MALI_PROFILING_EVENT_TYPE_SINGLE |
					MALI_PROFILING_EVENT_CHANNEL_GPU |
					MALI_PROFILING_EVENT_REASON_SINGLE_GPU_FREQ_VOLT_CHANGE,
					0, 0,	0,	0,	0);
#endif

	if (NULL != device->driver &&
	    NULL != device->driver->pm &&
	    NULL != device->driver->pm->runtime_suspend)
	{
		/* Need to notify Mali driver about this event */
		ret = device->driver->pm->runtime_suspend(device);
	}

	/* pmu off Kasin..*/
	mali_clock_critical(mali_cri_pmu_on_off, 0);
	//disable_clock();
	return ret;
}

static int mali_runtime_resume(struct device *device)
{
	int ret = 0;

	MALI_DEBUG_PRINT(4, ("mali_runtime_resume() called\n"));

	/* clock scaling. Kasin..*/
	//enable_clock();
	mali_clock_critical(mali_cri_pmu_on_off, 1);
#ifdef CONFIG_MALI400_PROFILING
	_mali_osk_profiling_add_event(MALI_PROFILING_EVENT_TYPE_SINGLE |
					MALI_PROFILING_EVENT_CHANNEL_GPU |
					MALI_PROFILING_EVENT_REASON_SINGLE_GPU_FREQ_VOLT_CHANGE,
					get_current_frequency(), 0,	0,	0,	0);
#endif

	if (NULL != device->driver &&
	    NULL != device->driver->pm &&
	    NULL != device->driver->pm->runtime_resume)
	{
		/* Need to notify Mali driver about this event */
		ret = device->driver->pm->runtime_resume(device);
	}

	return ret;
}

static int mali_runtime_idle(struct device *device)
{
	MALI_DEBUG_PRINT(4, ("mali_runtime_idle() called\n"));

	if (NULL != device->driver &&
	    NULL != device->driver->pm &&
	    NULL != device->driver->pm->runtime_idle)
	{
		/* Need to notify Mali driver about this event */
		int ret = device->driver->pm->runtime_idle(device);
		if (0 != ret)
		{
			return ret;
		}
	}

	pm_runtime_suspend(device);

	return 0;
}
#endif
