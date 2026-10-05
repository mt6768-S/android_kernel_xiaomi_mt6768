// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 MediaTek Inc.
 *
 * Backported from mali-r32p1 platform/mtk_platform_common/mtk_gpu_dvfs.c.
 */

#include <mali_kbase.h>
#include <linux/math64.h>
#include <backend/gpu/mali_kbase_pm_internal.h>
#include <backend/gpu/mali_kbase_pm_defs.h>
#include <platform/mtk_platform_common.h>

#include "mtk_gpu_dvfs.h"

static unsigned int current_util_active;

#if IS_ENABLED(CONFIG_MTK_GED_SUPPORT)
static struct kbasep_pm_metrics ged_last_metrics;

static unsigned int mtk_get_gpu_freq(void)
{
	struct kbase_device *kbdev = mtk_common_get_kbdev();

	if (kbdev)
		return (unsigned int)(kbdev->current_nominal_freq / 1000);
	return 0;
}
#endif

#if IS_ENABLED(CONFIG_MTK_GED_SUPPORT) && IS_ENABLED(CONFIG_MTK_GPU_COMMON_DVFS)
void mtk_common_ged_dvfs_commit(unsigned long ui32NewFreqID,
				GED_DVFS_COMMIT_TYPE eCommitType,
				int *pbCommited)
{
	int ret = mtk_common_gpufreq_commit((int)ui32NewFreqID);

	if (pbCommited)
		*pbCommited = (ret == 0) ? true : false;
}

void mtk_common_cal_gpu_utilization(unsigned int *pui32Loading,
				    unsigned int *pui32Block,
				    unsigned int *pui32Idle)
{
	struct kbase_device *kbdev = mtk_common_get_kbdev();
	struct kbasep_pm_metrics diff;
	u64 total_time;
	unsigned int utilisation = 0;

	if (!kbdev)
		return;

	kbase_pm_get_dvfs_metrics(kbdev, &ged_last_metrics, &diff);

	total_time = diff.time_busy + diff.time_idle;
	if (total_time > 0)
		utilisation = (unsigned int)div64_u64(100ULL * diff.time_busy,
						      total_time);

	if (utilisation > 100)
		utilisation = 100;

	current_util_active = utilisation;

	if (pui32Loading)
		*pui32Loading = utilisation;

	if (pui32Block)
		*pui32Block = 0;

	if (pui32Idle)
		*pui32Idle = 100 - utilisation;
}
#endif /* CONFIG_MTK_GED_SUPPORT && CONFIG_MTK_GPU_COMMON_DVFS */

int mtk_common_get_util_active(void)
{
	return (int)current_util_active;
}

int mtk_common_get_util_3d(void)
{
	return 0;
}

int mtk_common_get_util_ta(void)
{
	return 0;
}

int mtk_common_get_util_compute(void)
{
	return 0;
}

#if IS_ENABLED(CONFIG_MTK_GED_SUPPORT)
extern unsigned int (*mtk_get_gpu_freq_fp)(void);

int mtk_common_dvfs_init(struct kbase_device *kbdev)
{
	if (!kbdev)
		return -EINVAL;

#if IS_ENABLED(CONFIG_MTK_GPU_COMMON_DVFS)
	ged_dvfs_cal_gpu_utilization_fp = mtk_common_cal_gpu_utilization;
	ged_dvfs_gpu_freq_commit_fp = mtk_common_ged_dvfs_commit;
#endif
	mtk_get_gpu_freq_fp = mtk_get_gpu_freq;

	return 0;
}

void mtk_common_dvfs_term(struct kbase_device *kbdev)
{
#if IS_ENABLED(CONFIG_MTK_GPU_COMMON_DVFS)
	ged_dvfs_cal_gpu_utilization_fp = NULL;
	ged_dvfs_gpu_freq_commit_fp = NULL;
#endif
	mtk_get_gpu_freq_fp = NULL;
	current_util_active = 0;
}
#endif /* CONFIG_MTK_GED_SUPPORT */
