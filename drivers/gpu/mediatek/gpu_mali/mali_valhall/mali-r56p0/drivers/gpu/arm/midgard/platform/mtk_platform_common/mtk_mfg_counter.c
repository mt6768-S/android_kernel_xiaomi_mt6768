// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017-2021 MediaTek Inc.
 *
 * Backported from mali-r32p1 platform/mtk_platform_common/mtk_mfg_counter.c.
 */

#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <mali_kbase.h>
#include <csf/mali_kbase_csf_defs.h>
#include <csf/ipa_control/mali_kbase_csf_ipa_control.h>
#include <platform/mtk_mfg_counter.h>

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

#define MFG_PMU_NAME_LEN	64

struct mtk_hw_counter_desc {
	const char *name;
	enum kbase_ipa_core_type type;
	u8 idx;
};

static const struct mtk_hw_counter_desc mtk_hw_counters[] = {
	{ "TNAx_GPU_ACTIVE",            KBASE_IPA_CORE_TYPE_CSHW,   GPU_ACTIVE_CNT_IDX },
	{ "TNAx_TILER_ACTIVE",          KBASE_IPA_CORE_TYPE_TILER,  2 },
	{ "TNAx_EXEC_CORE_ACTIVE",      KBASE_IPA_CORE_TYPE_SHADER, 26 },
	{ "TNAx_EXEC_INSTR_FMA",        KBASE_IPA_CORE_TYPE_SHADER, 27 },
	{ "TNAx_TEX_FILT_NUM_OPERATIONS", KBASE_IPA_CORE_TYPE_SHADER, 39 },
	{ "TNAx_LS_MEM_READ_FULL",      KBASE_IPA_CORE_TYPE_SHADER, 44 },
	{ "TNAx_LS_MEM_READ_SHORT",     KBASE_IPA_CORE_TYPE_SHADER, 45 },
	{ "TNAx_LS_MEM_WRITE_FULL",     KBASE_IPA_CORE_TYPE_SHADER, 46 },
	{ "TNAx_LS_MEM_WRITE_SHORT",    KBASE_IPA_CORE_TYPE_SHADER, 47 },
	{ "TNAx_LS_MEM_ATOMIC",         KBASE_IPA_CORE_TYPE_SHADER, 48 },
	{ "TNAx_VARY_SLOT_32",          KBASE_IPA_CORE_TYPE_SHADER, 50 },
	{ "TNAx_VARY_SLOT_16",          KBASE_IPA_CORE_TYPE_SHADER, 51 },
};
#define NR_HW_COUNTERS		ARRAY_SIZE(mtk_hw_counters)

static DEFINE_MUTEX(counter_info_lock);

static struct GPU_PMU *mali_pmus;
static int number_of_hardware_counters;
static int binited;
static uint32_t active_cycle;
static unsigned int nr_shader_cores;

static struct kbase_device *mfg_kbdev;
static void *ipa_client;
static u64 last_query_ns;

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
	static struct {
		const char *name;
		int pos;
	} cache[8];
	int i, pos = -1;
	uint32_t value = 0;

	if (!mali_pmus)
		return 0;

	for (i = 0; i < ARRAY_SIZE(cache); i++) {
		if (cache[i].name == name) {
			pos = cache[i].pos;
			break;
		}
		if (!cache[i].name) {
			if (!_find_name_pos(name, &pos)) {
				cache[i].name = name;
				cache[i].pos = pos;
			}
			break;
		}
	}

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

static int _mtk_mfg_init_counter(void)
{
	struct kbase_ipa_control_perf_counter counters[NR_HW_COUNTERS];
	struct kbase_device *kbdev;
	int i, err;

	kbdev = kbase_find_device(-1);
	if (!kbdev)
		return PMU_NG;

	for (i = 0; i < NR_HW_COUNTERS; i++) {
		counters[i].scaling_factor = 1;
		counters[i].gpu_norm = false;
		counters[i].type = mtk_hw_counters[i].type;
		counters[i].idx = mtk_hw_counters[i].idx;
	}

	err = kbase_ipa_control_register(kbdev, counters, NR_HW_COUNTERS, &ipa_client);
	if (err) {
		pr_info("[PMU] ipa_control register failed: %d\n", err);
		kbase_release_device(kbdev);
		return PMU_NG;
	}

	mfg_kbdev = kbdev;
	nr_shader_cores = kbdev->gpu_props.num_cores;

	if (!binited) {
		number_of_hardware_counters = NR_HW_COUNTERS + MFG_MTK_COUNTER_SIZE;
		mali_pmus = kcalloc(number_of_hardware_counters,
				    sizeof(struct GPU_PMU), GFP_KERNEL);
		if (!mali_pmus) {
			kbase_ipa_control_unregister(kbdev, ipa_client);
			kbase_release_device(kbdev);
			mfg_kbdev = NULL;
			ipa_client = NULL;
			return PMU_NG;
		}

		for (i = 0; i < NR_HW_COUNTERS; i++) {
			mali_pmus[i].id = i;
			mali_pmus[i].name = mtk_hw_counters[i].name;
		}
		for (i = 0; i < MFG_MTK_COUNTER_SIZE; i++) {
			mali_pmus[NR_HW_COUNTERS + i].id = NR_HW_COUNTERS + i;
			mali_pmus[NR_HW_COUNTERS + i].name = mfg_mtk_counters[i].name;
		}
		binited = 1;
	}

	last_query_ns = ktime_get_ns();
	return PMU_OK;
}

static int _mtk_mfg_update_counter(void)
{
	u64 values[NR_HW_COUNTERS];
	u64 now_ns, elapsed_us;
	uint32_t gpu_freq;
	int i, err, ret = PMU_OK;

	if (!ipa_client || !mfg_kbdev)
		return PMU_NG;

	now_ns = ktime_get_ns();
	elapsed_us = div_u64(now_ns - last_query_ns, NSEC_PER_USEC);

	err = kbase_ipa_control_query(mfg_kbdev, ipa_client, values,
				      NR_HW_COUNTERS, NULL);
	if (err)
		return PMU_NG;

	last_query_ns = now_ns;

	_mtk_mfg_reset_counter(1);

	for (i = 0; i < NR_HW_COUNTERS; i++)
		mali_pmus[i].value = (uint32_t)values[i];

	gpu_freq = mt_gpufreq_get_cur_freq();
	active_cycle = (uint32_t)div_u64((u64)gpu_freq * elapsed_us, 1000);

	if (!mali_pmus[0].value) /* GPU_ACTIVE == 0, all counters invalid */
		return PMU_RESET_VALUE;

	for (i = 0; i < MFG_MTK_COUNTER_SIZE; i++) {
		uint32_t v = mfg_mtk_counters[i].read();

		if (!v)
			return PMU_RESET_VALUE;
		mali_pmus[NR_HW_COUNTERS + i].value = v;
	}

	return ret;
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
	if (ipa_client && mfg_kbdev) {
		kbase_ipa_control_unregister(mfg_kbdev, ipa_client);
		ipa_client = NULL;
	}
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
	if (ipa_client && mfg_kbdev) {
		kbase_ipa_control_unregister(mfg_kbdev, ipa_client);
		ipa_client = NULL;
	}
	if (mfg_kbdev) {
		kbase_release_device(mfg_kbdev);
		mfg_kbdev = NULL;
	}
	kfree(mali_pmus);
	mali_pmus = NULL;
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
	if (ipa_client && mfg_kbdev) {
		kbase_ipa_control_unregister(mfg_kbdev, ipa_client);
		ipa_client = NULL;
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
