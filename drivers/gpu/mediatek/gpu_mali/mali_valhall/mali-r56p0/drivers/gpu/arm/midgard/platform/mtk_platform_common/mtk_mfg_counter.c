// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017-2021 MediaTek Inc.
 *
 * Backported from mali-r32p1 platform/mtk_platform_common/mtk_mfg_counter.c.
 */

#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <mali_kbase.h>
#include "hwcnt/mali_kbase_hwcnt_types.h"
#include "hwcnt/mali_kbase_hwcnt_gpu.h"
#include "hwcnt/mali_kbase_hwcnt_virtualizer.h"
#include <platform/mtk_mfg_counter.h>

#include "mali_kbase_gator_hwcnt_names_tnax.h"

#define MALI_HWC_TYPES			4
#define MALI_COUNTERS_PER_BLOCK		64

/* gator hwc block classes, index the tNAx name table in 64-value windows */
enum mtk_hwc_class {
	JM_BLOCK = 0,
	TILER_BLOCK,
	SHADER_BLOCK,
	MMU_L2_BLOCK,
};

/* GPU stall counter registers (infara bus monitor), unchanged from r32p1 */
#if IS_ENABLED(CONFIG_MACH_MT6873) || IS_ENABLED(CONFIG_MACH_MT6853) || \
	IS_ENABLED(CONFIG_MACH_MT6833) || IS_ENABLED(CONFIG_MACH_MT6877) || \
	IS_ENABLED(CONFIG_MACH_MT6781)
#define GPU_STALL_ADD_BASE	0x1021C000
#else
#define GPU_STALL_ADD_BASE	0x1021E000
#endif
#define GPU_STALL_SIZE		0x1000
#define OFFSET_STALL_GPU_M0_WR_CNT	0x200
#define OFFSET_STALL_GPU_M0_RD_CNT	0x204
#define OFFSET_STALL_GPU_M1_WR_CNT	0x208
#define OFFSET_STALL_GPU_M1_RD_CNT	0x20c

static DEFINE_MUTEX(counter_info_lock);

static struct GPU_PMU *mali_pmus;
static int number_of_hardware_counters;
static int binited;
static uint32_t active_cycle;
static unsigned int nr_shader_cores;

static struct kbase_device *mfg_kbdev;
static struct kbase_hwcnt_virtualizer_client *mfg_hvcli;
static const struct kbase_hwcnt_metadata *mfg_metadata;
static struct kbase_hwcnt_enable_map mfg_enable_map;
static struct kbase_hwcnt_dump_buffer mfg_dump_buf;

/* (block, value index) of each exposed PMU entry in the dump buffer */
static u16 *mfg_pmu_blk;
static u16 *mfg_pmu_val;
static int mfg_nr_hw_pmus;

static void __iomem *io_addr_gpu_stall;
static unsigned int pre_stall_counters[4];

static int _find_name_pos(const char *name, int *pos)
{
	int i;

	if (!name || !pos)
		return -1;

	for (i = 0; i < number_of_hardware_counters; i++) {
		if (strstr(mali_pmus[i].name, name))
			break;
	}
	*pos = (i == number_of_hardware_counters) ? -1 : i;

	return (i == number_of_hardware_counters) ? -1 : 0;
}

static uint32_t _cal_urate(uint32_t fractions, uint32_t denominator)
{
	uint32_t value, urate;

	urate = value = 0;
	if (!denominator)
		return 0;
	if (fractions >= denominator)
		urate = 100;
	else {
		value = denominator / 100;
		if (!value)
			value = 1;
		urate = fractions / value;
		if (urate > 100)
			urate = 100;
		else if (!urate)
			urate = 1;
	}

	return urate;
}

static uint32_t _read_shader_u_rate(void)
{
	static int pos_exec_core_active = -1, pos_gpu_active = -1;
	uint32_t exec_core_active, gpu_active, value, urate = 0;

	if (!mali_pmus)
		return 0;

	if (pos_exec_core_active < 0)
		_find_name_pos("EXEC_CORE_ACTIVE", &pos_exec_core_active);
	if (pos_gpu_active < 0)
		_find_name_pos("GPU_ACTIVE", &pos_gpu_active);
	if (pos_exec_core_active < 0 || pos_gpu_active < 0 || !nr_shader_cores)
		return 0;

	exec_core_active = mali_pmus[pos_exec_core_active].value;
	gpu_active = mali_pmus[pos_gpu_active].value;

	if (gpu_active) {
		value = exec_core_active / nr_shader_cores;
		urate = _cal_urate(value, gpu_active);
	}

	return urate;
}

static uint32_t _read_alu_u_rate(void)
{
	static int pos_exec_instr_count = -1, pos_exec_active = -1;
	uint32_t exec_instr_count, exec_active, urate = 0;

	if (!mali_pmus)
		return 0;

	if (pos_exec_instr_count < 0)
		_find_name_pos("EXEC_INSTR_FMA", &pos_exec_instr_count);
	if (pos_exec_active < 0)
		_find_name_pos("EXEC_CORE_ACTIVE", &pos_exec_active);
	if (pos_exec_instr_count < 0 || pos_exec_active < 0)
		return 0;

	exec_instr_count = mali_pmus[pos_exec_instr_count].value;
	exec_active = mali_pmus[pos_exec_active].value;

	if (exec_active)
		urate = _cal_urate(exec_instr_count, exec_active);

	return urate;
}

static uint32_t _read_tex_u_rate(void)
{
	static int pos_tex_coord_issu = -1, pos_exec_active = -1;
	uint32_t tex_coord_issue, exec_active, urate = 0;

	if (!mali_pmus)
		return 0;

	if (pos_tex_coord_issu < 0)
		_find_name_pos("TEX_FILT_NUM_OPERATIONS", &pos_tex_coord_issu);
	if (pos_exec_active < 0)
		_find_name_pos("EXEC_CORE_ACTIVE", &pos_exec_active);
	if (pos_tex_coord_issu < 0 || pos_exec_active < 0)
		return 0;

	tex_coord_issue = mali_pmus[pos_tex_coord_issu].value;
	exec_active = mali_pmus[pos_exec_active].value;

	if (exec_active)
		urate = _cal_urate(tex_coord_issue, exec_active);

	return urate;
}

static uint32_t _read_lsc_u_rate(void)
{
	static int pos_ls_mem_read_full = -1, pos_ls_mem_read_short = -1;
	static int pos_ls_mem_write_full = -1, pos_ls_mem_write_short = -1;
	static int pos_ls_mem_atomic = -1, pos_exec_active = -1;
	uint32_t lsc_active, exec_active, urate = 0;

	if (!mali_pmus)
		return 0;

	if (pos_ls_mem_read_full < 0)
		_find_name_pos("LS_MEM_READ_FULL", &pos_ls_mem_read_full);
	if (pos_ls_mem_read_short < 0)
		_find_name_pos("LS_MEM_READ_SHORT", &pos_ls_mem_read_short);
	if (pos_ls_mem_write_full < 0)
		_find_name_pos("LS_MEM_WRITE_FULL", &pos_ls_mem_write_full);
	if (pos_ls_mem_write_short < 0)
		_find_name_pos("LS_MEM_WRITE_SHORT", &pos_ls_mem_write_short);
	if (pos_ls_mem_atomic < 0)
		_find_name_pos("LS_MEM_ATOMIC", &pos_ls_mem_atomic);
	if (pos_exec_active < 0)
		_find_name_pos("EXEC_CORE_ACTIVE", &pos_exec_active);
	if (pos_ls_mem_read_full < 0 || pos_ls_mem_read_short < 0 ||
	    pos_ls_mem_write_full < 0 || pos_ls_mem_write_short < 0 ||
	    pos_ls_mem_atomic < 0 || pos_exec_active < 0)
		return 0;

	lsc_active = mali_pmus[pos_ls_mem_read_full].value +
		     mali_pmus[pos_ls_mem_read_short].value +
		     mali_pmus[pos_ls_mem_write_full].value +
		     mali_pmus[pos_ls_mem_write_short].value +
		     mali_pmus[pos_ls_mem_atomic].value;
	exec_active = mali_pmus[pos_exec_active].value;

	if (exec_active) {
		if (lsc_active < (mali_pmus[pos_ls_mem_read_full].value +
				  mali_pmus[pos_ls_mem_write_full].value))
			urate = 0;
		else
			urate = _cal_urate(lsc_active, exec_active);
	}

	return urate;
}

static uint32_t _read_var_u_rate(void)
{
	static int pos_vary_slot_32 = -1, pos_vary_slot_16 = -1, pos_exec_active = -1;
	uint32_t var_active, exec_active, urate = 0;

	if (!mali_pmus)
		return 0;

	if (pos_vary_slot_32 < 0)
		_find_name_pos("VARY_SLOT_32", &pos_vary_slot_32);
	if (pos_vary_slot_16 < 0)
		_find_name_pos("VARY_SLOT_16", &pos_vary_slot_16);
	if (pos_exec_active < 0)
		_find_name_pos("EXEC_CORE_ACTIVE", &pos_exec_active);
	if (pos_vary_slot_32 < 0 || pos_vary_slot_16 < 0 || pos_exec_active < 0)
		return 0;

	var_active = mali_pmus[pos_vary_slot_32].value +
		     mali_pmus[pos_vary_slot_16].value;
	exec_active = mali_pmus[pos_exec_active].value;

	if (exec_active) {
		if (var_active < mali_pmus[pos_vary_slot_32].value)
			urate = 0;
		else
			urate = _cal_urate(var_active, exec_active);
	}

	return urate;
}

static uint32_t _read_counter_w_loading(const char *name)
{
	int pos = -1;
	uint32_t value = 0;

	if (!mali_pmus)
		return 0;

	_find_name_pos(name, &pos);
	if (pos >= 0)
		value = mali_pmus[pos].value;

	return (!value || !active_cycle) ? 1 : _cal_urate(value, active_cycle);
}

static uint32_t _read_shader_u_rate_w_loading(void)
{
	return _read_counter_w_loading("EXEC_CORE_ACTIVE");
}

static uint32_t _read_alu_u_rate_w_loading(void)
{
	return _read_counter_w_loading("EXEC_INSTR_FMA");
}

static uint32_t _read_tex_u_rate_w_loading(void)
{
	return _read_counter_w_loading("TEX_FILT_NUM_OPERATIONS");
}

static uint32_t _read_lsc_u_rate_w_loading(void)
{
	static int pos_ls_mem_read_full = -1, pos_ls_mem_read_short = -1;
	static int pos_ls_mem_write_full = -1, pos_ls_mem_write_short = -1;
	static int pos_ls_mem_atomic = -1;
	uint32_t lsc_active;

	if (!mali_pmus)
		return 0;

	if (pos_ls_mem_read_full < 0)
		_find_name_pos("LS_MEM_READ_FULL", &pos_ls_mem_read_full);
	if (pos_ls_mem_read_short < 0)
		_find_name_pos("LS_MEM_READ_SHORT", &pos_ls_mem_read_short);
	if (pos_ls_mem_write_full < 0)
		_find_name_pos("LS_MEM_WRITE_FULL", &pos_ls_mem_write_full);
	if (pos_ls_mem_write_short < 0)
		_find_name_pos("LS_MEM_WRITE_SHORT", &pos_ls_mem_write_short);
	if (pos_ls_mem_atomic < 0)
		_find_name_pos("LS_MEM_ATOMIC", &pos_ls_mem_atomic);
	if (pos_ls_mem_read_full < 0 || pos_ls_mem_read_short < 0 ||
	    pos_ls_mem_write_full < 0 || pos_ls_mem_write_short < 0 ||
	    pos_ls_mem_atomic < 0)
		return 0;

	lsc_active = mali_pmus[pos_ls_mem_read_full].value +
		     mali_pmus[pos_ls_mem_read_short].value +
		     mali_pmus[pos_ls_mem_write_full].value +
		     mali_pmus[pos_ls_mem_write_short].value +
		     mali_pmus[pos_ls_mem_atomic].value;

	if (lsc_active < (mali_pmus[pos_ls_mem_read_full].value +
			  mali_pmus[pos_ls_mem_write_full].value))
		lsc_active = 0;

	return (!lsc_active || !active_cycle) ? 1 : _cal_urate(lsc_active, active_cycle);
}

static uint32_t _read_var_u_rate_w_loading(void)
{
	static int pos_vary_slot_32 = -1, pos_vary_slot_16 = -1;
	uint32_t var_active;

	if (!mali_pmus)
		return 0;

	if (pos_vary_slot_32 < 0)
		_find_name_pos("VARY_SLOT_32", &pos_vary_slot_32);
	if (pos_vary_slot_16 < 0)
		_find_name_pos("VARY_SLOT_16", &pos_vary_slot_16);
	if (pos_vary_slot_32 < 0 || pos_vary_slot_16 < 0)
		return 0;

	var_active = mali_pmus[pos_vary_slot_32].value +
		     mali_pmus[pos_vary_slot_16].value;
	if (var_active < mali_pmus[pos_vary_slot_32].value)
		var_active = 0;

	return (!var_active || !active_cycle) ? 1 : _cal_urate(var_active, active_cycle);
}

typedef uint32_t (*mfg_read_pfn)(void);
static const struct {
	const char *name;
	mfg_read_pfn read;
} mfg_mtk_counters[] = {
	{ "MTK_SHADER_U_RATE", _read_shader_u_rate },
	{ "MTK_ALU_FMA_U_RATE", _read_alu_u_rate },
	{ "MTK_TEX_U_RATE", _read_tex_u_rate },
	{ "MTK_LSC_U_RATE", _read_lsc_u_rate },
	{ "MTK_VAR_U_RATE", _read_var_u_rate },
	{ "MTK_SHADER_U_RATE_W_LOADING", _read_shader_u_rate_w_loading },
	{ "MTK_ALU_FMA_U_RATE_W_LOADING", _read_alu_u_rate_w_loading },
	{ "MTK_TEX_U_RATE_W_LOADING", _read_tex_u_rate_w_loading },
	{ "MTK_LSC_U_RATE_W_LOADING", _read_lsc_u_rate_w_loading },
	{ "MTK_VAR_U_RATE_W_LOADING", _read_var_u_rate_w_loading },
};
#define MFG_MTK_COUNTER_SIZE	ARRAY_SIZE(mfg_mtk_counters)

static void _mtk_mfg_reset_counter(int ret)
{
	int i;

	if (!binited || !mali_pmus || !ret)
		return;

	for (i = 0; i < number_of_hardware_counters; i++) {
		mali_pmus[i].value = 0;
		mali_pmus[i].overflow = 0;
	}
}

static int _mtk_hwcnt_class_base(u64 blk_type)
{
	switch (blk_type) {
	case KBASE_HWCNT_GPU_V5_BLOCK_TYPE_PERF_FE:
		return JM_BLOCK * MALI_COUNTERS_PER_BLOCK;
	case KBASE_HWCNT_GPU_V5_BLOCK_TYPE_PERF_TILER:
		return TILER_BLOCK * MALI_COUNTERS_PER_BLOCK;
	case KBASE_HWCNT_GPU_V5_BLOCK_TYPE_PERF_SC:
		return SHADER_BLOCK * MALI_COUNTERS_PER_BLOCK;
	case KBASE_HWCNT_GPU_V5_BLOCK_TYPE_PERF_MEMSYS:
		return MMU_L2_BLOCK * MALI_COUNTERS_PER_BLOCK;
	default:
		return -1;
	}
}

static int _mtk_mfg_init_counter(void)
{
	struct kbase_device *kbdev;
	struct kbase_hwcnt_virtualizer *hvirt;
	size_t blk, val, cnt = 0;
	bool class_seen[MALI_HWC_TYPES] = { false };
	int err;

	kbdev = kbase_find_device(-1);
	if (!kbdev)
		return PMU_NG;

	hvirt = kbdev->hwcnt_gpu_virt;
	if (!hvirt)
		goto err_dev;

	mfg_metadata = kbase_hwcnt_virtualizer_metadata(hvirt);
	if (!mfg_metadata)
		goto err_dev;

	err = kbase_hwcnt_enable_map_alloc(mfg_metadata, &mfg_enable_map);
	if (err)
		goto err_dev;

	kbase_hwcnt_enable_map_enable_all(&mfg_enable_map);

	err = kbase_hwcnt_virtualizer_client_create(hvirt, &mfg_enable_map, &mfg_hvcli);
	if (err)
		goto err_map;

	err = kbase_hwcnt_dump_buffer_alloc(mfg_metadata, &mfg_dump_buf);
	if (err)
		goto err_cli;

	if (binited)
		return PMU_OK;

	/* one entry per non-empty tNAx name, first instance of each class */
	mfg_nr_hw_pmus = 0;
	for (blk = 0; blk < mfg_metadata->blk_cnt; blk++) {
		int base = _mtk_hwcnt_class_base(
			kbase_hwcnt_metadata_block_type(mfg_metadata, blk));
		size_t val_cnt;

		if (base < 0)
			continue;
		if (class_seen[base / MALI_COUNTERS_PER_BLOCK])
			continue;
		class_seen[base / MALI_COUNTERS_PER_BLOCK] = true;

		val_cnt = kbase_hwcnt_metadata_block_values_count(mfg_metadata, blk);
		if (val_cnt > MALI_COUNTERS_PER_BLOCK)
			val_cnt = MALI_COUNTERS_PER_BLOCK;

		for (val = 0; val < val_cnt; val++) {
			if (hardware_counters_mali_tNAx[base + val][0] == '\0')
				continue;
			mfg_nr_hw_pmus++;
		}
	}

	if (!mfg_nr_hw_pmus)
		goto err_buf;

	number_of_hardware_counters = mfg_nr_hw_pmus + MFG_MTK_COUNTER_SIZE;
	mali_pmus = kcalloc(number_of_hardware_counters,
			    sizeof(struct GPU_PMU), GFP_KERNEL);
	mfg_pmu_blk = kcalloc(mfg_nr_hw_pmus, sizeof(u16), GFP_KERNEL);
	mfg_pmu_val = kcalloc(mfg_nr_hw_pmus, sizeof(u16), GFP_KERNEL);
	if (!mali_pmus || !mfg_pmu_blk || !mfg_pmu_val)
		goto err_mem;

	memset(class_seen, 0, sizeof(class_seen));
	cnt = 0;
	for (blk = 0; blk < mfg_metadata->blk_cnt; blk++) {
		int base = _mtk_hwcnt_class_base(
			kbase_hwcnt_metadata_block_type(mfg_metadata, blk));
		size_t val_cnt;

		if (base < 0)
			continue;
		if (class_seen[base / MALI_COUNTERS_PER_BLOCK])
			continue;
		class_seen[base / MALI_COUNTERS_PER_BLOCK] = true;

		val_cnt = kbase_hwcnt_metadata_block_values_count(mfg_metadata, blk);
		if (val_cnt > MALI_COUNTERS_PER_BLOCK)
			val_cnt = MALI_COUNTERS_PER_BLOCK;

		for (val = 0; val < val_cnt; val++) {
			const char *name = hardware_counters_mali_tNAx[base + val];

			if (name[0] == '\0')
				continue;
			mali_pmus[cnt].id = cnt;
			mali_pmus[cnt].name = name;
			mfg_pmu_blk[cnt] = (u16)blk;
			mfg_pmu_val[cnt] = (u16)val;
			cnt++;
		}
	}

	for (val = 0; val < MFG_MTK_COUNTER_SIZE; val++) {
		mali_pmus[cnt].id = cnt;
		mali_pmus[cnt].name = mfg_mtk_counters[val].name;
		cnt++;
	}

	mfg_kbdev = kbdev;
	nr_shader_cores = kbdev->gpu_props.num_cores;
	binited = 1;

	return PMU_OK;

err_mem:
	kfree(mali_pmus);
	kfree(mfg_pmu_blk);
	kfree(mfg_pmu_val);
	mali_pmus = NULL;
	mfg_pmu_blk = NULL;
	mfg_pmu_val = NULL;
err_buf:
	kbase_hwcnt_dump_buffer_free(&mfg_dump_buf);
err_cli:
	kbase_hwcnt_virtualizer_client_destroy(mfg_hvcli);
	mfg_hvcli = NULL;
err_map:
	kbase_hwcnt_enable_map_free(&mfg_enable_map);
err_dev:
	kbase_release_device(kbdev);
	return PMU_NG;
}

static int _mtk_mfg_update_counter(void)
{
	u64 ts_start_ns, ts_end_ns, elapsed_us;
	uint32_t gpu_freq;
	int i, err;

	if (!mfg_hvcli)
		return PMU_NG;

	err = kbase_hwcnt_virtualizer_client_dump(mfg_hvcli, &ts_start_ns,
						  &ts_end_ns, &mfg_dump_buf);
	if (err)
		return PMU_NG;

	elapsed_us = div_u64(ts_end_ns - ts_start_ns, NSEC_PER_USEC);

	_mtk_mfg_reset_counter(1);

	for (i = 0; i < mfg_nr_hw_pmus; i++) {
		const u64 *blk_buf = kbase_hwcnt_dump_buffer_block_instance(
			&mfg_dump_buf, mfg_pmu_blk[i], 0);

		mali_pmus[i].value = (uint32_t)blk_buf[mfg_pmu_val[i]];
	}

	gpu_freq = mt_gpufreq_get_cur_freq();
	active_cycle = (uint32_t)div_u64((u64)gpu_freq * elapsed_us, 1000);

	/* GPU_ACTIVE == 0, all counters invalid */
	{
		int pos = -1;

		_find_name_pos("GPU_ACTIVE", &pos);
		if (pos >= 0 && !mali_pmus[pos].value)
			return PMU_RESET_VALUE;
	}

	for (i = 0; i < MFG_MTK_COUNTER_SIZE; i++) {
		uint32_t v = mfg_mtk_counters[i].read();

		if (!v)
			return PMU_RESET_VALUE;
		mali_pmus[mfg_nr_hw_pmus + i].value = v;
	}

	return PMU_OK;
}

static void _mtk_mfg_term_counter(void)
{
	if (mfg_hvcli) {
		kbase_hwcnt_virtualizer_client_destroy(mfg_hvcli);
		mfg_hvcli = NULL;
	}
	kbase_hwcnt_dump_buffer_free(&mfg_dump_buf);
	kbase_hwcnt_enable_map_free(&mfg_enable_map);
	if (mfg_kbdev) {
		kbase_release_device(mfg_kbdev);
		mfg_kbdev = NULL;
	}
}

static int mali_get_gpu_pmu_init(struct GPU_PMU *pmus, int pmu_size, int *ret_size)
{
	int i, ret = PMU_OK;

	mutex_lock(&counter_info_lock);
	if (!binited)
		ret = _mtk_mfg_init_counter();
	mutex_unlock(&counter_info_lock);

	if (ret != PMU_OK)
		return ret;

	if (pmus) {
		mutex_lock(&counter_info_lock);
		for (i = 0; i < number_of_hardware_counters && i < pmu_size; i++) {
			pmus[i].id = mali_pmus[i].id;
			pmus[i].name = mali_pmus[i].name;
		}
		mutex_unlock(&counter_info_lock);
	}

	if (ret_size) {
		mutex_lock(&counter_info_lock);
		*ret_size = number_of_hardware_counters;
		mutex_unlock(&counter_info_lock);
	}

	return PMU_OK;
}

static int mali_get_gpu_pmu_swapnreset(struct GPU_PMU *pmus, int pmu_size)
{
	int i, ret = PMU_OK;

	if (!binited) {
		pr_info("[PMU] not inited, call mtk_get_gpu_pmu_init first\n");
		return PMU_NG;
	}

	if (pmus) {
		mutex_lock(&counter_info_lock);

		ret = _mtk_mfg_update_counter();
		if (ret == PMU_RESET_VALUE) {
			_mtk_mfg_reset_counter(ret);
			ret = PMU_OK;
		}

		if (!ret) {
			for (i = 0; i < pmu_size && i < number_of_hardware_counters; i++) {
				pmus[i].id = mali_pmus[i].id;
				pmus[i].name = mali_pmus[i].name;
				pmus[i].value = mali_pmus[i].value;
				pmus[i].overflow = mali_pmus[i].overflow;
			}
		}

		mutex_unlock(&counter_info_lock);
	}

	return ret;
}

static int mali_get_gpu_pmu_swapnreset_stop(void)
{
	if (!binited) {
		pr_info("[PMU] not inited, call mtk_get_gpu_pmu_init first\n");
		return PMU_NG;
	}

	mutex_lock(&counter_info_lock);
	_mtk_mfg_term_counter();
	mutex_unlock(&counter_info_lock);

	return PMU_OK;
}

int mali_get_gpu_pmu_deinit(void)
{
	if (!binited) {
		pr_info("[PMU] not inited, call mtk_get_gpu_pmu_init first\n");
		return PMU_NG;
	}

	mutex_lock(&counter_info_lock);
	_mtk_mfg_term_counter();
	kfree(mali_pmus);
	kfree(mfg_pmu_blk);
	kfree(mfg_pmu_val);
	mali_pmus = NULL;
	mfg_pmu_blk = NULL;
	mfg_pmu_val = NULL;
	binited = 0;
	mutex_unlock(&counter_info_lock);

	return PMU_OK;
}

int mtk_mfg_pmu_start(void)
{
	int ret;

	mutex_lock(&counter_info_lock);
	if (!binited)
		ret = _mtk_mfg_init_counter();
	else
		ret = PMU_OK;
	mutex_unlock(&counter_info_lock);

	return ret;
}

void mtk_mfg_pmu_stop(void)
{
	mutex_lock(&counter_info_lock);
	if (mfg_hvcli) {
		kbase_hwcnt_virtualizer_client_destroy(mfg_hvcli);
		mfg_hvcli = NULL;
	}
	mutex_unlock(&counter_info_lock);
}

void mtk_mfg_counter_init(void)
{
	mtk_get_gpu_pmu_init_fp = mali_get_gpu_pmu_init;
	mtk_get_gpu_pmu_deinit_fp = mali_get_gpu_pmu_deinit;
	mtk_get_gpu_pmu_swapnreset_fp = mali_get_gpu_pmu_swapnreset;
	mtk_get_gpu_pmu_swapnreset_stop_fp = mali_get_gpu_pmu_swapnreset_stop;

	binited = 0;
}

void mtk_mfg_counter_destroy(void)
{
	if (binited)
		mali_get_gpu_pmu_deinit();

	mtk_get_gpu_pmu_init_fp = NULL;
	mtk_get_gpu_pmu_deinit_fp = NULL;
	mtk_get_gpu_pmu_swapnreset_fp = NULL;
	mtk_get_gpu_pmu_swapnreset_stop_fp = NULL;
}

/* init but don't enable met */
int gator_gpu_pmu_init(void)
{
	int i, ret = PMU_OK;

	mutex_lock(&counter_info_lock);
	if (!binited)
		ret = _mtk_mfg_init_counter();
	mutex_unlock(&counter_info_lock);

	if (ret != PMU_OK)
		return ret;

	mutex_lock(&counter_info_lock);
	for (i = 0; i < number_of_hardware_counters; i++)
		mali_pmus[i].id = i;
	mutex_unlock(&counter_info_lock);

	return PMU_OK;
}

int mtk_gpu_stall_create_subfs(void)
{
	io_addr_gpu_stall = ioremap(GPU_STALL_ADD_BASE, GPU_STALL_SIZE);
	if (!io_addr_gpu_stall) {
		pr_info("Failed to init GPU stall counters!!\n");
		return -ENODEV;
	}
	return 0;
}

void mtk_gpu_stall_delete_subfs(void)
{
	if (io_addr_gpu_stall) {
		iounmap(io_addr_gpu_stall);
		io_addr_gpu_stall = NULL;
	}
}

void mtk_gpu_stall_start(void)
{
	unsigned int value = 0x00000001;

	if (io_addr_gpu_stall) {
		writel(value, io_addr_gpu_stall + OFFSET_STALL_GPU_M0_WR_CNT);
		writel(value, io_addr_gpu_stall + OFFSET_STALL_GPU_M0_RD_CNT);
		writel(value, io_addr_gpu_stall + OFFSET_STALL_GPU_M1_WR_CNT);
		writel(value, io_addr_gpu_stall + OFFSET_STALL_GPU_M1_RD_CNT);
	}
}

void mtk_gpu_stall_stop(void)
{
	if (io_addr_gpu_stall) {
		writel(0x00000000, io_addr_gpu_stall + OFFSET_STALL_GPU_M0_WR_CNT);
		writel(0x00000000, io_addr_gpu_stall + OFFSET_STALL_GPU_M0_RD_CNT);
		writel(0x00000000, io_addr_gpu_stall + OFFSET_STALL_GPU_M1_WR_CNT);
		writel(0x00000000, io_addr_gpu_stall + OFFSET_STALL_GPU_M1_RD_CNT);
	}
}

void mtk_GPU_STALL_RAW(unsigned int *diff, int size)
{
	unsigned int stall_counters[4] = { 0 };
	int i;

	if (io_addr_gpu_stall) {
		stall_counters[0] = ((unsigned int)readl(io_addr_gpu_stall + OFFSET_STALL_GPU_M0_WR_CNT)) >> 1;
		stall_counters[1] = ((unsigned int)readl(io_addr_gpu_stall + OFFSET_STALL_GPU_M0_RD_CNT)) >> 1;
		stall_counters[2] = ((unsigned int)readl(io_addr_gpu_stall + OFFSET_STALL_GPU_M1_WR_CNT)) >> 1;
		stall_counters[3] = ((unsigned int)readl(io_addr_gpu_stall + OFFSET_STALL_GPU_M1_RD_CNT)) >> 1;

		for (i = 0; i < size && i < 4; i++) {
			if (pre_stall_counters[i] > stall_counters[i])
				diff[i] = stall_counters[i] + (0xFFFFFFFF - pre_stall_counters[i]);
			else
				diff[i] = stall_counters[i] - pre_stall_counters[i];
		}
		for (i = 0; i < size && i < 4; i++)
			pre_stall_counters[i] = stall_counters[i];
	}
}
