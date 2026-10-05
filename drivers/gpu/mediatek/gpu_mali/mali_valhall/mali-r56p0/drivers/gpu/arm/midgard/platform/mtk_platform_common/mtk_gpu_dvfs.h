/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 MediaTek Inc.
 *
 * Backported from mali-r32p1, adapted to the CSF (r56p0) power
 * metrics, which only expose time_busy/time_idle.
 */

#ifndef __MTK_GPU_DVFS_H__
#define __MTK_GPU_DVFS_H__

#include <ged_dvfs.h>

struct kbase_device;

#if IS_ENABLED(CONFIG_MTK_GED_SUPPORT) && IS_ENABLED(CONFIG_MTK_GPU_COMMON_DVFS)
void mtk_common_cal_gpu_utilization(unsigned int *pui32Loading,
				    unsigned int *pui32Block,
				    unsigned int *pui32Idle);
void mtk_common_ged_dvfs_commit(unsigned long ui32NewFreqID,
				GED_DVFS_COMMIT_TYPE eCommitType,
				int *pbCommited);

extern void (*ged_dvfs_cal_gpu_utilization_fp)(unsigned int *pui32Loading,
					       unsigned int *pui32Block,
					       unsigned int *pui32Idle);
extern void (*ged_dvfs_gpu_freq_commit_fp)(unsigned long ui32NewFreqID,
					   GED_DVFS_COMMIT_TYPE eCommitType,
					   int *pbCommited);
#endif

int mtk_common_get_util_active(void);
int mtk_common_get_util_3d(void);
int mtk_common_get_util_ta(void);
int mtk_common_get_util_compute(void);

#if IS_ENABLED(CONFIG_MTK_GED_SUPPORT)
int mtk_common_dvfs_init(struct kbase_device *kbdev);
void mtk_common_dvfs_term(struct kbase_device *kbdev);
#else
static inline int mtk_common_dvfs_init(struct kbase_device *kbdev) { return 0; }
static inline void mtk_common_dvfs_term(struct kbase_device *kbdev) { }
#endif

#endif /* __MTK_GPU_DVFS_H__ */
