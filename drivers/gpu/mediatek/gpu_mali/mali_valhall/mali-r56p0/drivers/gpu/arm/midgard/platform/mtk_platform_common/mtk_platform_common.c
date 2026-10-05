// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 MediaTek Inc.
 */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <mali_kbase.h>
#include <platform/mtk_platform_common.h>
#include <mtk_gpufreq.h>
#include <ged_dvfs.h>

#include "mtk_gpu_dvfs.h"

static bool mfg_powered;
static DEFINE_MUTEX(mfg_pm_lock);
static struct kbase_device *mali_kbdev;
#if IS_ENABLED(CONFIG_PROC_FS)
static struct proc_dir_entry *mtk_mali_root;
#endif

struct kbase_device *mtk_common_get_kbdev(void)
{
	return mali_kbdev;
}

bool mtk_common_pm_is_mfg_active(void)
{
	return mfg_powered;
}

void mtk_common_pm_mfg_active(void)
{
	mutex_lock(&mfg_pm_lock);
	mfg_powered = true;
	mutex_unlock(&mfg_pm_lock);
}

void mtk_common_pm_mfg_idle(void)
{
	mutex_lock(&mfg_pm_lock);
	mfg_powered = false;
	mutex_unlock(&mfg_pm_lock);
}

int mtk_common_gpufreq_commit(int opp_idx)
{
	int ret = -1;

	mutex_lock(&mfg_pm_lock);
	if (opp_idx >= 0 && mtk_common_pm_is_mfg_active()) {
#if defined(CONFIG_MACH_MT6768) || defined(CONFIG_MACH_MT6785)
		ret = mt_gpufreq_target(opp_idx, false);
#else
		ret = mt_gpufreq_target(opp_idx, KIR_POLICY);
#endif
	}
	mutex_unlock(&mfg_pm_lock);

	return ret;
}

int mtk_common_ged_dvfs_get_last_commit_idx(void)
{
#if IS_ENABLED(CONFIG_MTK_GED_SUPPORT)
	return (int)ged_dvfs_get_last_commit_idx();
#else
	return -1;
#endif
}

#if IS_ENABLED(CONFIG_PROC_FS)
static int mtk_common_gpu_utilization_show(struct seq_file *m, void *v)
{
	unsigned int cur_opp_idx = mt_gpufreq_get_cur_freq_index();

	seq_printf(m, "ACTIVE=%d 3D/TA/COMPUTE=%d/%d/%d OPP_IDX=%u MFG_PWR=%d\n",
		   mtk_common_get_util_active(),
		   mtk_common_get_util_3d(),
		   mtk_common_get_util_ta(),
		   mtk_common_get_util_compute(),
		   cur_opp_idx, mfg_powered);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_common_gpu_utilization);

void mtk_common_procfs_init(void)
{
	mtk_mali_root = proc_mkdir("mtk_mali", NULL);
	if (!mtk_mali_root) {
		pr_info("cannot create /proc/%s\n", "mtk_mali");
		return;
	}
	proc_create("utilization", 0444, mtk_mali_root, &mtk_common_gpu_utilization_fops);
}

void mtk_common_procfs_exit(void)
{
	if (mtk_mali_root) {
		remove_proc_entry("utilization", mtk_mali_root);
		remove_proc_entry("mtk_mali", NULL);
		mtk_mali_root = NULL;
	}
}
#endif

int mtk_common_device_init(struct kbase_device *kbdev)
{
	if (!kbdev) {
		pr_info("@%s: kbdev is NULL\n", __func__);
		return -1;
	}

	mali_kbdev = kbdev;

	mtk_common_dvfs_init(kbdev);
	mtk_mfg_counter_init();

	return 0;
}

void mtk_common_device_term(struct kbase_device *kbdev)
{
	if (!kbdev) {
		pr_info("@%s: kbdev is NULL\n", __func__);
		return;
	}

	mtk_common_dvfs_term(kbdev);
	mtk_mfg_counter_destroy();

	mali_kbdev = NULL;
}
