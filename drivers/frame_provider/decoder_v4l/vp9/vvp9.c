 /*
  * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
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
  * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
  *
  * Description:
  */
#define DEBUG
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/semaphore.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/utils/amstream.h>
#include <linux/amlogic/media/utils/vformat.h>
#include <linux/amlogic/media/frame_sync/ptsserv.h>
#include <linux/amlogic/media/canvas/canvas.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#include <linux/dma-map-ops.h>
#else
#include <linux/dma-contiguous.h>
#endif
#include <linux/slab.h>
#include <uapi/linux/tee.h>
#include <linux/sched/clock.h>
#include "../../../stream_input/amports/amports_priv.h"
#include <linux/amlogic/media/codec_mm/codec_mm.h>
#include <linux/amlogic/media/codec_mm/configs.h>
#include <linux/amlogic/media/video_sink/video.h>
#include <media/v4l2-mem2mem.h>

#include "../../decoder/utils/decoder_mmu_box.h"
#include "../../decoder/utils/decoder_bmmu_box.h"
#include "../../decoder/utils/vdec.h"
#include "../../decoder/utils/amvdec.h"
#ifdef CONFIG_AMLOGIC_MEDIA_MULTI_DEC
#include "../../decoder/utils/vdec_profile.h"
#endif
#include "../../decoder/utils/config_parser.h"
#include "../../decoder/utils/firmware.h"
#include "../../../common/chips/decoder_cpu_ver_info.h"
#include "../../decoder/utils/vdec_v4l2_buffer_ops.h"
#include "../../decoder/utils/vdec_feature.h"
#include "../../decoder/utils/aml_buf_helper.h"
#include "../../decoder/utils/vdec_ge2d_utils.h"
#include "vvp9.h"
#include "../../decoder/utils/decoder_dma_alloc.h"

#define MEM_NAME "codec_vp9"
#define MIX_STREAM_SUPPORT

#define CO_MV_COMPRESS
#define HW_MASK_FRONT    0x1
#define HW_MASK_BACK     0x2
#define HEVC_MPRED_CTRL4                           0x324c
#define HEVC_CM_HEADER_START_ADDR                  0x3628
#define HEVC_DBLK_CFGB                             0x350b
#define HEVCD_MPP_ANC2AXI_TBL_DATA                 0x3464
#define HEVC_SAO_MMU_VH1_ADDR                      0x363b
#define HEVC_SAO_MMU_VH0_ADDR                      0x363a

#define HEVC_MV_INFO                               0x310d
#define HEVC_QP_INFO                               0x3137
#define HEVC_SKIP_INFO                             0x3136

#define VP9_10B_DEC_IDLE                           0
#define VP9_10B_DEC_FRAME_HEADER                   1
#define VP9_10B_DEC_SLICE_SEGMENT                  2
#define VP9_10B_DECODE_SLICE                     5
#define VP9_10B_DISCARD_NAL                  6
#define VP9_DUMP_LMEM                7
#define HEVC_DECPIC_DATA_DONE            0xa
#define HEVC_DECPIC_DATA_ERROR            0xb
#define HEVC_NAL_DECODE_DONE            0xe
#define HEVC_DECODE_BUFEMPTY        0x20
#define HEVC_DECODE_TIMEOUT         0x21
#define HEVC_SEARCH_BUFEMPTY        0x22
#define HEVC_DECODE_OVER_SIZE       0x23
#define HEVC_S2_DECODING_DONE       0x50
#define VP9_HEAD_PARSER_DONE            0xf0
#define VP9_HEAD_SEARCH_DONE          0xf1
#define VP9_EOS                        0xf2
#define HEVC_ACTION_DONE                0xff

#define VF_POOL_SIZE        32

#undef pr_info
#define pr_info pr_cont

#define DECODE_MODE_SINGLE		((0x80 << 24) | 0)
#define DECODE_MODE_MULTI_STREAMBASE	((0x80 << 24) | 1)
#define DECODE_MODE_MULTI_FRAMEBASE	((0x80 << 24) | 2)
#define DECODE_MODE_SINGLE_LOW_LATENCY ((0x80 << 24) | 3)
#define DECODE_MODE_MULTI_FRAMEBASE_NOHEAD ((0x80 << 24) | 4)

#define  VP9_TRIGGER_FRAME_DONE		0x100
#define  VP9_TRIGGER_FRAME_ENABLE	0x200

#define MV_MEM_UNIT 0x240
/*---------------------------------------------------
 * Include "parser_cmd.h"
 *---------------------------------------------------
 */
#define PARSER_CMD_SKIP_CFG_0 0x0000090b

#define PARSER_CMD_SKIP_CFG_1 0x1b14140f

#define PARSER_CMD_SKIP_CFG_2 0x001b1910

#define PARSER_CMD_NUMBER 37

#define MULTI_INSTANCE_SUPPORT
#define SUPPORT_10BIT

#ifndef STAT_KTHREAD
#define STAT_KTHREAD 0x40
#endif

#ifdef MULTI_INSTANCE_SUPPORT
#define MULTI_DRIVER_NAME "ammvdec_vp9_v4l"

static unsigned int max_decode_instance_num = MAX_INSTANCE_MUN;
static unsigned int decode_frame_count[MAX_INSTANCE_MUN];
static unsigned int display_frame_count[MAX_INSTANCE_MUN];
static unsigned int max_process_time[MAX_INSTANCE_MUN];
static unsigned int run_count[MAX_INSTANCE_MUN];
static unsigned int input_empty[MAX_INSTANCE_MUN];
static unsigned int not_run_ready[MAX_INSTANCE_MUN];

static u32 decode_timeout_val = 200;
static int start_decode_buf_level = 0x8000;
static u32 work_buf_size;

static u32 force_pts_unstable;

static u32 mv_buf_margin;

static u32 mv_buf_dynamic_alloc;

/* DOUBLE_WRITE_MODE is enabled only when NV21 8 bit output is needed */
/* double_write_mode:
 *	0, no double write;
 *	1, 1:1 ratio;
 *	2, (1/4):(1/4) ratio;
 *	3, (1/4):(1/4) ratio, with both compressed frame included
 *	4, (1/2):(1/2) ratio;
 *	8, (1/8):(1/8) ratio;
 *	0x10, double write only
 *	0x100, if > 1080p,use mode 4,else use mode 1;
 *	0x200, if > 1080p,use mode 2,else use mode 1;
 *	0x300, if > 720p, use mode 4, else use mode 1;
 */
static u32 double_write_mode;

#define DRIVER_NAME "amvdec_vp9_v4l"
#define DRIVER_HEADER_NAME "amvdec_vp9_header"


#define PUT_INTERVAL        (HZ/100)
#define ERROR_SYSTEM_RESET_COUNT   200

#define PTS_NORMAL                0
#define PTS_NONE_REF_USE_DURATION 1

#define PTS_MODE_SWITCHING_THRESHOLD           3
#define PTS_MODE_SWITCHING_RECOVERY_THRESHOLD 3

#define DUR2PTS(x) ((x)*90/96)

struct VP9Decoder_s;
static int vvp9_vf_states(struct vframe_states *states, void *);
static struct vframe_s *vvp9_vf_peek(void *);
static struct vframe_s *vvp9_vf_get(void *);
static void vvp9_vf_put(struct vframe_s *, void *);
static int vvp9_event_cb(int type, void *data, void *private_data);
#ifdef MULTI_INSTANCE_SUPPORT
static s32 vvp9_init(struct vdec_s *vdec);
#else
static s32 vvp9_init(struct VP9Decoder_s *pbi);
#endif
static void vvp9_prot_init(struct VP9Decoder_s *pbi, u32 mask);
static int vvp9_local_init(struct VP9Decoder_s *pbi);
static void vvp9_put_timer_func(struct timer_list *timer);
static void dump_data(struct VP9Decoder_s *pbi, int size);
static unsigned char get_data_check_sum
	(struct VP9Decoder_s *pbi, int size);
static void dump_pic_list(struct VP9Decoder_s *pbi);
static int vp9_alloc_mmu(
		struct VP9Decoder_s *pbi,
		int cur_buf_idx,
		int pic_width,
		int pic_height,
		unsigned short bit_depth,
		unsigned int *mmu_index_adr);

static int vp9_recycle_frame_buffer(struct VP9Decoder_s *pbi);
static int vp9_reset_frame_buffer(struct VP9Decoder_s *pbi);
static void vp9_recycle_dec_resource(void *priv, struct aml_buf *aml_buf);
static void vp9_buf_ref_process_for_exception(struct VP9Decoder_s *pbi);


static const char vvp9_dec_id[] = "vvp9-dev";

#define PROVIDER_NAME   "decoder.vp9"
#define MULTI_INSTANCE_PROVIDER_NAME    "vdec.vp9"

static const struct vframe_operations_s vvp9_vf_provider = {
	.peek = vvp9_vf_peek,
	.get = vvp9_vf_get,
	.put = vvp9_vf_put,
	.event_cb = vvp9_event_cb,
	.vf_states = vvp9_vf_states,
};

#ifndef MULTI_INSTANCE_SUPPORT
static struct vframe_provider_s vvp9_vf_prov;
#endif

static struct task_ops_s task_dec_ops;

static u32 bit_depth_luma;
static u32 bit_depth_chroma;
static u32 frame_width;
static u32 frame_height;
static u32 video_signal_type;

static u32 on_no_keyframe_skiped;

#define PROB_SIZE    (496 * 2 * 4)
#define PROB_BUF_SIZE    (0x5000)
#define COUNT_BUF_SIZE   (0x300 * 4 * 4)
/*compute_losless_comp_body_size(4096, 2304, 1) = 18874368(0x1200000)*/
#define MAX_FRAME_4K_NUM 0x1200
#define MAX_FRAME_8K_NUM 0x4800

#define HEVC_ASSIST_MMU_MAP_ADDR                   0x3009

#ifdef SUPPORT_FB_DECODING
/* register define */
#define HEVC_ASSIST_HED_FB_W_CTL                   0x3006
#define HEVC_ASSIST_HED_FB_R_CTL                   0x3007
#define HEVC_ASSIST_HED_FB_ADDR                    0x3008
#define HEVC_ASSIST_FB_MMU_MAP_ADDR                0x300a
#define HEVC_ASSIST_FBD_MMU_MAP_ADDR               0x300b

#define MAX_STAGE_PAGE_NUM 0x1200
#define STAGE_MMU_MAP_SIZE (MAX_STAGE_PAGE_NUM * 4)
#endif
static inline int div_r32(int64_t m, int n)
{
#ifndef CONFIG_ARM64
	int64_t qu = 0;
	qu = div_s64(m, n);
	return (int)qu;
#else
	return (int)(m/n);
#endif
}

/*USE_BUF_BLOCK*/
struct BUF_s {
	int index;
	unsigned int alloc_flag;
	/*buffer */
	unsigned int cma_page_count;
	unsigned long alloc_addr;
	unsigned long start_adr;
	unsigned int size;

	unsigned int free_start_adr;
	ulong v4l_ref_buf_addr;
	ulong	header_addr;
	u32 	header_size;
	u32	luma_size;
	ulong	chroma_addr;
	u32	chroma_size;
} /*BUF_t */;

struct MVBUF_s {
	unsigned long start_adr;
	unsigned int size;
	int used_flag;
} /*MVBUF_t */;

#define WR_PTR_INC_NUM 1

#define SIMULATION
#define DOS_PROJECT
#undef MEMORY_MAP_IN_REAL_CHIP

#define   MCRCC_ENABLE

#define VP9_LPF_LVL_UPDATE
/*#define DBG_LF_PRINT*/

#ifdef VP9_10B_NV21
#else
#define LOSLESS_COMPRESS_MODE
#endif

#define DOUBLE_WRITE_YSTART_TEMP 0x02000000
#define DOUBLE_WRITE_CSTART_TEMP 0x02900000

#define VP9_DEBUG_BUFMGR                   0x01
#define VP9_DEBUG_BUFMGR_MORE              0x02
#define VP9_DEBUG_BUFMGR_DETAIL            0x04
#define VP9_DEBUG_OUT_PTS                  0x10
#define VP9_DEBUG_SEND_PARAM_WITH_REG      0x100
#define VP9_DEBUG_MERGE                    0x200
#define VP9_DEBUG_DBG_LF_PRINT             0x400
#define VP9_DEBUG_REG                      0x800
#define VP9_DEBUG_2_STAGE                  0x1000
#define VP9_DEBUG_2_STAGE_MORE             0x2000
#define VP9_DEBUG_QOS_INFO                 0x4000
#define VP9_DEBUG_DIS_LOC_ERROR_PROC       0x10000
#define VP9_DEBUG_DIS_SYS_ERROR_PROC   0x20000
#define VP9_DEBUG_DUMP_PIC_LIST       0x40000
#define VP9_DEBUG_TRIG_SLICE_SEGMENT_PROC 0x80000
#define VP9_DEBUG_NO_TRIGGER_FRAME       0x100000
#define VP9_DEBUG_LOAD_UCODE_FROM_FILE   0x200000
#define VP9_DEBUG_FORCE_SEND_AGAIN       0x400000
#define VP9_DEBUG_DUMP_DATA              0x800000
#define VP9_DEBUG_CACHE                  0x1000000
#define VP9_DEBUG_CACHE_HIT_RATE         0x2000000
#define IGNORE_PARAM_FROM_CONFIG         0x8000000
#ifdef MULTI_INSTANCE_SUPPORT
#define PRINT_FLAG_ERROR		0x0
#define PRINT_FLAG_V4L_DETAIL		0x10000000
#define PRINT_FLAG_VDEC_STATUS		0x20000000
#define PRINT_FLAG_VDEC_DETAIL		0x40000000
#define PRINT_FLAG_VDEC_DATA		0x80000000
#endif
static u32 force_bufspec;
static u32 debug;
static bool is_reset;
/*for debug*/
/*
	udebug_flag:
	bit 0, enable ucode print
	bit 1, enable ucode detail print
	bit [31:16] not 0, pos to dump lmem
		bit 2, pop bits to lmem
		bit [11:8], pre-pop bits for alignment (when bit 2 is 1)
*/
static u32 udebug_flag;
/*
	when udebug_flag[1:0] is not 0
	udebug_pause_pos not 0,
		pause position
*/
static u32 udebug_pause_pos;
/*
	when udebug_flag[1:0] is not 0
	and udebug_pause_pos is not 0,
		pause only when DEBUG_REG2 is equal to this val
*/
static u32 udebug_pause_val;

static u32 udebug_pause_decode_idx;

static u32 without_display_mode;

static u32 v4l_bitstream_id_enable = 1;

/*
 *[3:0] 0: default use config from omx.
 *      1: force enable fence.
 *      2: disable fence.
 *[7:4] 0: fence use for driver.
 *      1: fence fd use for app.
 */
static u32 force_config_fence;

#define DEBUG_REG
#ifdef DEBUG_REG
void WRITE_VREG_DBG2(unsigned int adr, unsigned int val)
{
	if (debug & VP9_DEBUG_REG)
		pr_info("%s(%x, %x)\n", __func__, adr, val);
	if (adr != 0)
		WRITE_VREG(adr, val);
}

#undef WRITE_VREG
#define WRITE_VREG WRITE_VREG_DBG2
#endif

#define FRAME_CNT_WINDOW_SIZE 59
#define RATE_CORRECTION_THRESHOLD 5
/**************************************************

VP9 buffer management start

***************************************************/
#define MMU_COMPRESS_HEADER_SIZE_1080P  0x10000
#define MMU_COMPRESS_HEADER_SIZE_4K  0x48000
#define MMU_COMPRESS_HEADER_SIZE_8K  0x120000

#define MMU_COMPRESS_HEADER_SIZE  0x48000
#define MMU_COMPRESS_8K_HEADER_SIZE  (0x48000*4)
#define MAX_SIZE_8K (8192 * 4608)
#define MAX_SIZE_4K (4096 * 2304)
#define MAX_SIZE_2K (1920 * 1088)
#define IS_8K_SIZE(w, h)	(((w) * (h)) > MAX_SIZE_4K)
#define IS_4K_SIZE(w, h)  (((w) * (h)) > (1920*1088))

#define INVALID_IDX -1  /* Invalid buffer index.*/

#define RPM_BEGIN                                              0x200
#define RPM_END                                                0x280
#define MAX_SLICE_NUM 1024

union param_u {
	struct {
		unsigned short data[RPM_END - RPM_BEGIN];
	} l;
	struct {
		/* from ucode lmem, do not change this struct */
		unsigned short profile;
		unsigned short show_existing_frame;
		unsigned short frame_to_show_idx;
		unsigned short frame_type; /*1 bit*/
		unsigned short show_frame; /*1 bit*/
		unsigned short error_resilient_mode; /*1 bit*/
		unsigned short intra_only; /*1 bit*/
		unsigned short display_size_present; /*1 bit*/
		unsigned short reset_frame_context;
		unsigned short refresh_frame_flags;
		unsigned short width;
		unsigned short height;
		unsigned short display_width;
		unsigned short display_height;
	/*
	 *bit[11:8] - ref_frame_info_0 (ref(3-bits), ref_frame_sign_bias(1-bit))
	 *bit[7:4]  - ref_frame_info_1 (ref(3-bits), ref_frame_sign_bias(1-bit))
	 *bit[3:0]  - ref_frame_info_2 (ref(3-bits), ref_frame_sign_bias(1-bit))
	 */
		unsigned short ref_info;
		/*
		 *bit[2]: same_frame_size0
		 *bit[1]: same_frame_size1
		 *bit[0]: same_frame_size2
		 */
		unsigned short same_frame_size;

		unsigned short mode_ref_delta_enabled;
		unsigned short ref_deltas[4];
		unsigned short mode_deltas[2];
		unsigned short filter_level;
		unsigned short sharpness_level;
		unsigned short bit_depth;
		unsigned short seg_quant_info[8];
		unsigned short seg_enabled;
		unsigned short seg_abs_delta;
		/* bit 15: feature enabled; bit 8, sign; bit[5:0], data */
		unsigned short seg_lf_info[8];
	} p;
};


struct vpx_codec_frame_buffer_s {
	uint8_t *data;  /**< Pointer to the data buffer */
	size_t size;  /**< Size of data in bytes */
	void *priv;  /**< Frame's private data */
};

enum vpx_color_space_t {
	VPX_CS_UNKNOWN    = 0,  /**< Unknown */
	VPX_CS_BT_601     = 1,  /**< BT.601 */
	VPX_CS_BT_709     = 2,  /**< BT.709 */
	VPX_CS_SMPTE_170  = 3,  /**< SMPTE.170 */
	VPX_CS_SMPTE_240  = 4,  /**< SMPTE.240 */
	VPX_CS_BT_2020    = 5,  /**< BT.2020 */
	VPX_CS_RESERVED   = 6,  /**< Reserved */
	VPX_CS_SRGB       = 7   /**< sRGB */
}; /**< alias for enum vpx_color_space */

enum vpx_bit_depth_t {
	VPX_BITS_8  =  8,  /**<  8 bits */
	VPX_BITS_10 = 10,  /**< 10 bits */
	VPX_BITS_12 = 12,  /**< 12 bits */
};

struct PIC_BUFFER_CONFIG_s {
	int index;
	int BUF_index;
	int mv_buf_index;
	int comp_body_size;
	int buf_size;
	int vf_ref;
	int y_canvas_index;
	int uv_canvas_index;
#ifdef MULTI_INSTANCE_SUPPORT
	struct canvas_config_s canvas_config[2];
#endif
	int decode_idx;
	int slice_type;
	int stream_offset;
	u32 pts;
	u64 pts64;
	u64 timestamp;
	uint8_t error_mark;

	int slice_idx;
	/*buffer*/
	unsigned long header_adr;
	unsigned long mpred_mv_wr_start_addr;
	int mv_size;

	unsigned int dw_y_adr;
	unsigned int dw_u_v_adr;
	u32	luma_size;
	u32	chroma_size;
	int mc_canvas_y;
	int mc_canvas_u_v;

	int lcu_total;

	int   y_width;
	int   y_height;
	int   y_crop_width;
	int   y_crop_height;
	int   y_stride;

	int   uv_width;
	int   uv_height;
	int   uv_crop_width;
	int   uv_crop_height;
	int   uv_stride;

	int   alpha_width;
	int   alpha_height;
	int   alpha_stride;

	uint8_t *y_buffer;
	uint8_t *u_buffer;
	uint8_t *v_buffer;
	uint8_t *alpha_buffer;

	uint8_t *buffer_alloc;
	int buffer_alloc_sz;
	int border;
	int frame_size;
	int subsampling_x;
	int subsampling_y;
	unsigned int bit_depth;
	enum vpx_color_space_t color_space;

	int corrupted;
	int flags;
	unsigned long cma_alloc_addr;

	int double_write_mode;

	/* picture qos information*/
	int max_qp;
	int avg_qp;
	int min_qp;
	int max_skip;
	int avg_skip;
	int min_skip;
	int max_mv;
	int min_mv;
	int avg_mv;

	u32 hw_decode_time;
	u32 frame_size2; // For frame base mode

	/* vdec sync. */
	struct dma_fence *fence;

	int ge2d_copy_frame;
	int frame_to_show;

	/* hdr10 plus data */
	u32 hdr10p_data_size;
	char *hdr10p_data_buf;
	int v4l_buf_index;
	int repeat_count;
	struct PIC_BUFFER_CONFIG_s *repeat_pic;
} PIC_BUFFER_CONFIG;

enum BITSTREAM_PROFILE {
	PROFILE_0,
	PROFILE_1,
	PROFILE_2,
	PROFILE_3,
	MAX_PROFILES
};

enum FRAME_TYPE {
	KEY_FRAME = 0,
	INTER_FRAME = 1,
	FRAME_TYPES,
};

enum REFERENCE_MODE {
	SINGLE_REFERENCE      = 0,
	COMPOUND_REFERENCE    = 1,
	REFERENCE_MODE_SELECT = 2,
	REFERENCE_MODES       = 3,
};

#define NONE           -1
#define INTRA_FRAME     0
#define LAST_FRAME      1
#define GOLDEN_FRAME    2
#define ALTREF_FRAME    3
#define MAX_REF_FRAMES  4

#define REFS_PER_FRAME 3

#define REF_FRAMES_LOG2 3
#define REF_FRAMES (1 << REF_FRAMES_LOG2)
#define REF_FRAMES_4K (6)

/*4 scratch frames for the new frames to support a maximum of 4 cores decoding
 *in parallel, 3 for scaled references on the encoder.
 *TODO(hkuang): Add ondemand frame buffers instead of hardcoding the number
 * // of framebuffers.
 *TODO(jkoleszar): These 3 extra references could probably come from the
 *normal reference pool.
 */
#define FRAME_BUFFERS (REF_FRAMES + 16)
#define HEADER_FRAME_BUFFERS (FRAME_BUFFERS)
#define MAX_BUF_NUM (FRAME_BUFFERS)
#define MV_BUFFER_NUM	FRAME_BUFFERS
#ifdef SUPPORT_FB_DECODING
#define STAGE_MAX_BUFFERS		16
#else
#define STAGE_MAX_BUFFERS		0
#endif

#define FRAME_CONTEXTS_LOG2 2
#define FRAME_CONTEXTS (1 << FRAME_CONTEXTS_LOG2)
/*buffer + header buffer + workspace*/
#ifdef MV_USE_FIXED_BUF
#define MAX_BMMU_BUFFER_NUM (FRAME_BUFFERS + HEADER_FRAME_BUFFERS + 1)
#define VF_BUFFER_IDX(n) (n)
#define HEADER_BUFFER_IDX(n) (FRAME_BUFFERS + n)
#define WORK_SPACE_BUF_ID (FRAME_BUFFERS + HEADER_FRAME_BUFFERS)
#else
#define MAX_BMMU_BUFFER_NUM \
	(FRAME_BUFFERS + HEADER_FRAME_BUFFERS + MV_BUFFER_NUM + 1)
#define VF_BUFFER_IDX(n) (n)
#define HEADER_BUFFER_IDX(n) (FRAME_BUFFERS + n)
#define MV_BUFFER_IDX(n) (FRAME_BUFFERS + HEADER_FRAME_BUFFERS + n)
#define WORK_SPACE_BUF_ID \
	(FRAME_BUFFERS + HEADER_FRAME_BUFFERS + MV_BUFFER_NUM)
#endif

struct RefCntBuffer_s {
	int ref_count;
	/*MV_REF *mvs;*/
	int mi_rows;
	int mi_cols;
	struct vpx_codec_frame_buffer_s raw_frame_buffer;
	struct PIC_BUFFER_CONFIG_s buf;

/*The Following variables will only be used in frame parallel decode.
 *
 *frame_worker_owner indicates which FrameWorker owns this buffer. NULL means
 *that no FrameWorker owns, or is decoding, this buffer.
 *VP9Worker *frame_worker_owner;
 *
 *row and col indicate which position frame has been decoded to in real
 *pixel unit. They are reset to -1 when decoding begins and set to INT_MAX
 *when the frame is fully decoded.
 */
	int row;
	int col;

	int show_frame;
} RefCntBuffer;

struct RefBuffer_s {
/*TODO(dkovalev): idx is not really required and should be removed, now it
 *is used in vp9_onyxd_if.c
 */
	int idx;
	struct PIC_BUFFER_CONFIG_s *buf;
} RefBuffer;

struct InternalFrameBuffer_s {
	uint8_t *data;
	size_t size;
	int in_use;
} InternalFrameBuffer;

struct InternalFrameBufferList_s {
	int num_internal_frame_buffers;
	struct InternalFrameBuffer_s *int_fb;
} InternalFrameBufferList;

struct BufferPool_s {
	/*Protect BufferPool from being accessed by several FrameWorkers at
	 *the same time during frame parallel decode.
	 *TODO(hkuang): Try to use atomic variable instead of locking the whole pool.
	 *
	 *Private data associated with the frame buffer callbacks.
	 *void *cb_priv;
	 *
	 *vpx_get_frame_buffer_cb_fn_t get_fb_cb;
	 *vpx_release_frame_buffer_cb_fn_t release_fb_cb;
	 */
	struct RefCntBuffer_s frame_bufs[FRAME_BUFFERS];

	/*Frame buffers allocated internally by the codec.*/
	struct InternalFrameBufferList_s int_frame_buffers;
	unsigned long flags;
	spinlock_t lock;

} BufferPool;

#define lock_buffer_pool(pool, flags) \
		spin_lock_irqsave(&pool->lock, flags)

#define unlock_buffer_pool(pool, flags) \
		spin_unlock_irqrestore(&pool->lock, flags)

struct VP9_Common_s {
	enum vpx_color_space_t color_space;
	int width;
	int height;
	int display_width;
	int display_height;
	int last_width;
	int last_height;

	int subsampling_x;
	int subsampling_y;

	int use_highbitdepth;/*Marks if we need to use 16bit frame buffers.*/

	struct PIC_BUFFER_CONFIG_s *frame_to_show;
	struct RefCntBuffer_s *prev_frame;

	/*TODO(hkuang): Combine this with cur_buf in macroblockd.*/
	struct RefCntBuffer_s *cur_frame;

	int ref_frame_map[REF_FRAMES]; /* maps fb_idx to reference slot */

	/*Prepare ref_frame_map for the next frame.
	 *Only used in frame parallel decode.
	 */
	int next_ref_frame_map[REF_FRAMES];

	/* TODO(jkoleszar): could expand active_ref_idx to 4,
	 *with 0 as intra, and roll new_fb_idx into it.
	 */

	/*Each frame can reference REFS_PER_FRAME buffers*/
	struct RefBuffer_s frame_refs[REFS_PER_FRAME];

	int prev_fb_idx;
	int new_fb_idx;
	int cur_fb_idx_mmu;
	/*last frame's frame type for motion search*/
	enum FRAME_TYPE last_frame_type;
	enum FRAME_TYPE frame_type;

	int show_frame;
	int last_show_frame;
	int show_existing_frame;

	/*Flag signaling that the frame is encoded using only INTRA modes.*/
	uint8_t intra_only;
	uint8_t last_intra_only;

	int allow_high_precision_mv;

	/*Flag signaling that the frame context should be reset to default
	 *values. 0 or 1 implies don't reset, 2 reset just the context
	 *specified in the  frame header, 3 reset all contexts.
	 */
	int reset_frame_context;

	/*MBs, mb_rows/cols is in 16-pixel units; mi_rows/cols is in
	 *	MODE_INFO (8-pixel) units.
	 */
	int MBs;
	int mb_rows, mi_rows;
	int mb_cols, mi_cols;
	int mi_stride;

	/*Whether to use previous frame's motion vectors for prediction.*/
	int use_prev_frame_mvs;

	int refresh_frame_context;    /* Two state 0 = NO, 1 = YES */

	int ref_frame_sign_bias[MAX_REF_FRAMES];    /* Two state 0, 1 */

	/*TODO(hkuang):Remove this as it is the same as frame_parallel_decode*/
	/* in pbi.*/
	int frame_parallel_decode;  /* frame-based threading.*/

	/*Context probabilities for reference frame prediction*/
	enum REFERENCE_MODE reference_mode;

	unsigned int current_video_frame;
	enum BITSTREAM_PROFILE profile;

	enum vpx_bit_depth_t bit_depth;

	int error_resilient_mode;
	int frame_parallel_decoding_mode;

	int byte_alignment;
	int skip_loop_filter;

	/*External BufferPool passed from outside.*/
	struct BufferPool_s *buffer_pool;

	int above_context_alloc_cols;
};

static void set_canvas(struct VP9Decoder_s *pbi,
	struct PIC_BUFFER_CONFIG_s *pic_config);
static int prepare_display_buf(struct VP9Decoder_s *pbi,
					struct PIC_BUFFER_CONFIG_s *pic_config);

static void fill_frame_info(struct VP9Decoder_s *pbi,
	struct PIC_BUFFER_CONFIG_s *frame,
	unsigned int framesize,
	unsigned int pts);

static struct PIC_BUFFER_CONFIG_s *get_frame_new_buffer(struct VP9_Common_s *cm)
{
	return &cm->buffer_pool->frame_bufs[cm->new_fb_idx].buf;
}

static void ref_cnt_fb(struct RefCntBuffer_s *bufs, int *idx, int new_idx)
{
	const int ref_index = *idx;

	if (ref_index >= 0 && bufs[ref_index].ref_count > 0) {
		bufs[ref_index].ref_count--;
		/*pr_info("[MMU DEBUG 2] dec ref_count[%d] : %d\r\n",
		 *		ref_index, bufs[ref_index].ref_count);
		 */
	}

	*idx = new_idx;

	bufs[new_idx].ref_count++;
	/*pr_info("[MMU DEBUG 3] inc ref_count[%d] : %d\r\n",
	 *			new_idx, bufs[new_idx].ref_count);
	 */
}

int vp9_release_frame_buffer(struct vpx_codec_frame_buffer_s *aml_buf)
{
	struct InternalFrameBuffer_s *const int_fb =
				(struct InternalFrameBuffer_s *)aml_buf->priv;
	if (int_fb)
		int_fb->in_use = 0;
	return 0;
}

static int  compute_losless_comp_body_size(int width, int height,
				bool is_bit_depth_10);

static void setup_display_size(struct VP9_Common_s *cm, union param_u *params,
						int print_header_info)
{
	cm->display_width = cm->width;
	cm->display_height = cm->height;
	if (params->p.display_size_present) {
		if (print_header_info)
			pr_info(" * 1-bit display_size_present read : 1\n");
		cm->display_width = params->p.display_width;
		cm->display_height = params->p.display_height;
		/*vp9_read_frame_size(rb, &cm->display_width,
		 *					&cm->display_height);
		 */
	} else {
		if (print_header_info)
			pr_info(" * 1-bit display_size_present read : 0\n");
	}
}


uint8_t print_header_info = 0;

struct buff_s {
	u32 buf_start;
	u32 buf_size;
	u32 buf_end;
} buff_t;

struct BuffInfo_s {
	u32 max_width;
	u32 max_height;
	u32 start_adr;
	u32 end_adr;
	struct buff_s ipp;
	struct buff_s sao_abv;
	struct buff_s sao_vb;
	struct buff_s short_term_rps;
	struct buff_s vps;
	struct buff_s sps;
	struct buff_s pps;
	struct buff_s sao_up;
	struct buff_s swap_buf;
	struct buff_s swap_buf2;
	struct buff_s scalelut;
	struct buff_s dblk_para;
	struct buff_s dblk_data;
	struct buff_s seg_map;
	struct buff_s mmu_vbh;
	struct buff_s cm_header;
	struct buff_s mpred_above;
#ifdef MV_USE_FIXED_BUF
	struct buff_s mpred_mv;
#endif
	struct buff_s rpm;
	struct buff_s lmem;
	struct buff_s prob_buf;
	struct buff_s prob_cnt_buf;
} BuffInfo_t;
#ifdef MULTI_INSTANCE_SUPPORT
#define DEC_RESULT_NONE             0
#define DEC_RESULT_DONE             1
#define DEC_RESULT_AGAIN            2
#define DEC_RESULT_CONFIG_PARAM     3
#define DEC_RESULT_ERROR            4
#define DEC_INIT_PICLIST			5
#define DEC_UNINIT_PICLIST			6
#define DEC_RESULT_GET_DATA         7
#define DEC_RESULT_GET_DATA_RETRY   8
#define DEC_RESULT_EOS              9
#define DEC_RESULT_FORCE_EXIT       10
#define DEC_RESULT_NEED_MORE_BUFFER	11
#define DEC_RESULT_UNFINISH	        12
#define DEC_V4L2_CONTINUE_DECODING  18

#define DEC_S1_RESULT_NONE          0
#define DEC_S1_RESULT_DONE          1
#define DEC_S1_RESULT_FORCE_EXIT       2
#define DEC_S1_RESULT_TEST_TRIGGER_DONE	0xf0

#ifdef FB_DECODING_TEST_SCHEDULE
#define TEST_SET_NONE        0
#define TEST_SET_PIC_DONE    1
#define TEST_SET_S2_DONE     2
#endif

static void vp9_work(struct work_struct *work);
#endif
struct loop_filter_info_n;
struct loopfilter;
struct segmentation;

#ifdef SUPPORT_FB_DECODING
static void mpred_process(struct VP9Decoder_s *pbi);
static void vp9_s1_work(struct work_struct *work);

struct stage_buf_s {
	int index;
	unsigned short rpm[RPM_END - RPM_BEGIN];
};

static unsigned int not_run2_ready[MAX_INSTANCE_MUN];

static unsigned int run2_count[MAX_INSTANCE_MUN];

#ifdef FB_DECODING_TEST_SCHEDULE
u32 stage_buf_num; /* = 16;*/
#else
u32 stage_buf_num;
#endif
#endif

struct afbc_buf {
	ulong fb;
	int   used;
};

struct vp9_fence_vf_t {
	u32 used_size;
	struct vframe_s *fence_vf[VF_POOL_SIZE];
};

struct vp9_repeat_buf_t {
	u32 used_size;
	struct RefCntBuffer_s *frame_bufs[FRAME_BUFFERS];
};

struct VP9Decoder_s {
#ifdef MULTI_INSTANCE_SUPPORT
	unsigned char index;

	struct device *cma_dev;
	struct platform_device *platform_dev;
	void (*vdec_cb)(struct vdec_s *, void *, int);
	void *vdec_cb_arg;
	struct vframe_chunk_s *chunk;
	int dec_result;
	struct work_struct work;
	struct work_struct recycle_mmu_work;
	struct work_struct set_clk_work;
	u32 start_shift_bytes;

	struct BuffInfo_s work_space_buf_store;
	unsigned long buf_start;
	u32 buf_size;
	u32 cma_alloc_count;
	unsigned long cma_alloc_addr;
	bool eos;
	unsigned long int start_process_time;
	unsigned last_lcu_idx;
	int decode_timeout_count;
	unsigned timeout_num;
	int save_buffer_mode;

	int double_write_mode;
#endif
	long used_4k_num;
	u32 curr_pic_offset;
	unsigned char m_ins_flag;
	char *provider_name;
	union param_u param;
	int frame_count;
	int pic_count;
	u32 stat;
	struct timer_list timer;
	u32 frame_dur;
	u32 frame_ar;
	int fatal_error;
	uint8_t init_flag;
	uint8_t first_sc_checked;
	uint8_t process_busy;
#define PROC_STATE_INIT			0
#define PROC_STATE_DECODESLICE	1
#define PROC_STATE_SENDAGAIN	2
	uint8_t process_state;
	u32 ucode_pause_pos;

	int show_frame_num;
	struct buff_s mc_buf_spec;
	struct dec_sysinfo vvp9_amstream_dec_info;
	void *rpm_addr;
	void *lmem_addr;
	dma_addr_t rpm_phy_addr;
	dma_addr_t lmem_phy_addr;
	unsigned short *lmem_ptr;
	unsigned short *debug_ptr;

	void *prob_buffer_addr;
	void *count_buffer_addr;
	dma_addr_t prob_buffer_phy_addr;
	dma_addr_t count_buffer_phy_addr;

	void *frame_mmu_map_addr;
	dma_addr_t frame_mmu_map_phy_addr;

	unsigned int use_cma_flag;

	struct BUF_s m_BUF[MAX_BUF_NUM];
	struct MVBUF_s m_mv_BUF[MV_BUFFER_NUM];
	u32 used_buf_num;
	DECLARE_KFIFO(newframe_q, struct vframe_s *, VF_POOL_SIZE);
	DECLARE_KFIFO(display_q, struct vframe_s *, VF_POOL_SIZE);
	DECLARE_KFIFO(pending_q, struct vframe_s *, VF_POOL_SIZE);
	struct vframe_s vfpool[VF_POOL_SIZE];
	atomic_t vf_pre_count;
	atomic_t vf_get_count;
	atomic_t vf_put_count;
	int buf_num;
	int pic_num;
	int lcu_size_log2;
	unsigned int losless_comp_body_size;

	u32 video_signal_type;

	int pts_mode;
	int last_lookup_pts;
	int last_pts;
	u64 last_lookup_pts_us64;
	u64 last_pts_us64;
	u64 shift_byte_count;

	u32 pts_unstable;
	u32 frame_cnt_window;
	u32 pts1, pts2;
	u32 last_duration;
	u32 duration_from_pts_done;
	bool vp9_first_pts_ready;

	u32 shift_byte_count_lo;
	u32 shift_byte_count_hi;
	int pts_mode_switching_count;
	int pts_mode_recovery_count;

	bool get_frame_dur;
	u32 saved_resolution;

	struct VP9_Common_s common;
	struct RefCntBuffer_s *cur_buf;
	int refresh_frame_flags;
	uint8_t need_resync;
	uint8_t hold_ref_buf;
	uint8_t ready_for_new_data;
	struct BufferPool_s vp9_buffer_pool;

	struct BuffInfo_s *work_space_buf;

	struct buff_s *mc_buf;

	unsigned int frame_width;
	unsigned int frame_height;

	unsigned short *rpm_ptr;
	int     init_pic_w;
	int     init_pic_h;
	int     lcu_total;
	int     lcu_size;

	int     slice_type;

	int skip_flag;
	int decode_idx;
	int slice_idx;
	uint8_t has_keyframe;
	uint8_t wait_buf;
	uint8_t error_flag;

	/* bit 0, for decoding; bit 1, for displaying */
	uint8_t ignore_bufmgr_error;
	int PB_skip_mode;
	int PB_skip_count_after_decoding;
	int default_filt_lvl;
	struct loop_filter_info_n *lfi;
	struct loopfilter *lf;
	struct segmentation *seg_4lf;
	struct vdec_info *gvs;

	u32 pre_stream_offset;

	unsigned int dec_status;
	u32 last_put_idx;
	int new_frame_displayed;
	void *mmu_box;
	void *bmmu_box;
	int mmu_enable;
	struct vframe_master_display_colour_s vf_dp;
	struct firmware_s *fw;
	int max_pic_w;
	int max_pic_h;
#ifdef SUPPORT_FB_DECODING
	int dec_s1_result;
	int s1_test_cmd;
	struct work_struct s1_work;
	int used_stage_buf_num;
	int s1_pos;
	int s2_pos;
	void *stage_mmu_map_addr;
	dma_addr_t stage_mmu_map_phy_addr;
	struct stage_buf_s *s1_buf;
	struct stage_buf_s *s2_buf;
	struct stage_buf_s *stage_bufs[STAGE_MAX_BUFFERS];
	unsigned char run2_busy;

	int s1_mv_buf_index;
	int s1_mv_buf_index_pre;
	int s1_mv_buf_index_pre_pre;
	unsigned long s1_mpred_mv_wr_start_addr;
	unsigned long s1_mpred_mv_wr_start_addr_pre;
	unsigned short s1_intra_only;
	unsigned short s1_frame_type;
	unsigned short s1_width;
	unsigned short s1_height;
	unsigned short s1_last_show_frame;
	union param_u s1_param;
	u8 back_not_run_ready;
#endif
	int need_cache_size;
	u64 sc_start_time;
	bool postproc_done;
	int low_latency_flag;
	bool no_head;
	bool pic_list_init_done;
	bool pic_list_init_done2;
	bool is_used_v4l;
	void *v4l2_ctx;
	bool v4l_params_parsed;
	int frameinfo_enable;
	struct vframe_qos_s vframe_qos;
	u32 mem_map_mode;
	u32 dynamic_buf_num_margin;
	struct vframe_s vframe_dummy;
	u32 res_ch_flag;
	union param_u vp9_param;
	int sidebind_type;
	int sidebind_channel_id;
	bool enable_fence;
	int fence_usage;
	u32 frame_mode_pts_save[FRAME_BUFFERS];
	u64 frame_mode_pts64_save[FRAME_BUFFERS];
	int run_ready_min_buf_num;
	int one_package_frame_cnt;
	int buffer_wrap[FRAME_BUFFERS];
	int last_width;
	int last_height;
	u32 error_frame_width;
	u32 error_frame_height;
	u32 endian;
	struct aml_buf *aml_buf;
	bool wait_more_buf;
	spinlock_t wait_buf_lock;
	struct vp9_fence_vf_t fence_vf_s;
	struct mutex fence_mutex;
	dma_addr_t rdma_phy_adr;
	unsigned *rdma_adr;
	struct trace_decoder_name trace;
	int existing_buff_index;
	u32 show_fake_frame;
	bool high_bandwidth_flag;
	int cur_idx;
	struct vdec_ge2d *ge2d;
	struct afbc_buf  afbc_buf_table[BUF_FBC_NUM_MAX];
	struct vp9_repeat_buf_t repeat_buf;
	u32 data_size;
	u32 data_offset;
	u32 data_invalid;
	u32 consume_byte;
	ulong rpm_mem_handle;
	ulong lmem_phy_handle;
	ulong prob_buf_handle;
	ulong count_buf_handle;
	ulong frame_mmu_map_handle;
	ulong stage_mmu_map_handle;
	ulong rdma_mem_handle;
	bool timeout;
	int v4l_duration;
};

static int vp9_print(struct VP9Decoder_s *pbi,
	int flag, const char *fmt, ...)
{
#define HEVC_PRINT_BUF		512
	unsigned char buf[HEVC_PRINT_BUF];
	int len = 0;

	if (pbi == NULL ||
		(flag == 0) ||
		(debug & flag)) {
		va_list args;

		va_start(args, fmt);
		if (pbi)
			len = sprintf(buf, "[%d]", pbi->index);
		vsnprintf(buf + len, HEVC_PRINT_BUF - len, fmt, args);
		if (flag == 0)
			pr_debug("%s", buf);
		else
			pr_info("%s", buf);
		va_end(args);
	}
	return 0;
}

static int is_oversize(int w, int h)
{
	int max = MAX_SIZE_8K;

	if ((get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_SM1) ||
		(get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T5M) ||
		is_cpu_s7() ||
		(get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_S7D))
		max = MAX_SIZE_4K;
	else if ((get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T5D) ||
		(get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXHD2) ||
		is_cpu_s7_s805x3())
		max = MAX_SIZE_2K;

	if (w < 64 || h < 64)
		return true;

	if (h != 0 && (w > max / h))
		return true;

	return false;
}

static int vvp9_mmu_compress_header_size(int w, int h)
{
	w = ALIGN(w, 64);
	h = ALIGN(h, 64);

	if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1) &&
		IS_8K_SIZE(w, h))
		return (MMU_COMPRESS_HEADER_SIZE_8K);
	if (IS_4K_SIZE(w, h))
		return (MMU_COMPRESS_HEADER_SIZE_4K);
	return (MMU_COMPRESS_HEADER_SIZE_1080P);
}

static int vvp9_frame_mmu_map_size(struct VP9Decoder_s *pbi)
{
	if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1) &&
		IS_8K_SIZE(pbi->max_pic_w, pbi->max_pic_h))
		return (MAX_FRAME_8K_NUM << 2);

	return (MAX_FRAME_4K_NUM << 2);
}

static int v4l_alloc_and_config_pic(struct VP9Decoder_s *pbi,
	struct PIC_BUFFER_CONFIG_s *pic);

static void resize_context_buffers(struct VP9Decoder_s *pbi,
	struct VP9_Common_s *cm, int width, int height)
{
	if (cm->width != width || cm->height != height) {
		/* to do ..*/
		if (pbi != NULL) {
			pbi->vp9_first_pts_ready = 0;
			pbi->duration_from_pts_done = 0;
		}
		pr_debug("%s (%d,%d)=>(%d,%d)\r\n", __func__,
			cm->width, cm->height,
			width, height);
		cm->width = width;
		cm->height = height;
	}
}

static int valid_ref_frame_size(int ref_width, int ref_height,
				int this_width, int this_height) {
	return 2 * this_width >= ref_width &&
		2 * this_height >= ref_height &&
		this_width <= 16 * ref_width &&
		this_height <= 16 * ref_height;
}

static int setup_frame_size(
		struct VP9Decoder_s *pbi,
		struct VP9_Common_s *cm, union param_u *params,
		unsigned int *mmu_index_adr,
		int print_header_info) {
	int width, height;
	struct BufferPool_s * const pool = cm->buffer_pool;
	struct PIC_BUFFER_CONFIG_s *ybf;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	int ret = 0;
	int i;

	width = params->p.width;
	height = params->p.height;
	if (is_oversize(width, height)) {
		pbi->error_frame_width = width;
		pbi->error_frame_height = height;
		vp9_print(pbi, 0, "%s, Error: Invalid frame size, width %d, height %d\n",
			__func__, width, height);
		vdec_v4l_post_error_event(ctx, DECODER_WARNING_DATA_ERROR);
		return -1;
	}
	pbi->error_frame_width = 0;
	pbi->error_frame_height = 0;

	if (print_header_info)
		pr_info(" * 16-bits w read : %d (width : %d)\n", width, height);
	if (print_header_info)
		pr_info(" * 16-bits h read : %d (height : %d)\n", width, height);

	WRITE_VREG(HEVC_PARSER_PICTURE_SIZE, (height << 16) | width);
#ifdef VP9_10B_HED_FB
	WRITE_VREG(HEVC_ASSIST_PIC_SIZE_FB_READ, (height << 16) | width);
#endif
	if (pbi->mmu_enable && ((pbi->double_write_mode & 0x10) == 0)) {
		for (i = 0; i < BUF_FBC_NUM_MAX; i++) {
			if (pbi->m_BUF[cm->new_fb_idx].v4l_ref_buf_addr
				== pbi->afbc_buf_table[i].fb) {
				cm->cur_fb_idx_mmu = i;
				vp9_print(pbi, VP9_DEBUG_BUFMGR,
					"cur fb idx mmu %d\n",
					i);
				break;
			}
		}

		if (i >= BUF_FBC_NUM_MAX) {
			vp9_print(pbi, 0, "[ERR]can't find fb(0x%lx) in afbc table\n",
					pbi->m_BUF[cm->new_fb_idx].v4l_ref_buf_addr);
			ret = -1;
		} else {
			ret = vp9_alloc_mmu(pbi,
				cm->cur_fb_idx_mmu,
				params->p.width,
				params->p.height,
				params->p.bit_depth,
				mmu_index_adr);
		}

		if (ret != 0) {
			pr_err("can't alloc need mmu1,idx %d ret =%d\n",
				cm->new_fb_idx,
				ret);
			return ret;
		}
		//cm->cur_fb_idx_mmu = cm->new_fb_idx;
	}

	resize_context_buffers(pbi, cm, width, height);
	setup_display_size(cm, params, print_header_info);

	/* porting */
	ybf = get_frame_new_buffer(cm);
	if (!ybf)
		return -1;

	ybf->y_crop_width = width;
	ybf->y_crop_height = height;
	ybf->bit_depth = params->p.bit_depth;

	pool->frame_bufs[cm->new_fb_idx].buf.subsampling_x = cm->subsampling_x;
	pool->frame_bufs[cm->new_fb_idx].buf.subsampling_y = cm->subsampling_y;
	pool->frame_bufs[cm->new_fb_idx].buf.bit_depth =
						(unsigned int)cm->bit_depth;
	pool->frame_bufs[cm->new_fb_idx].buf.color_space = cm->color_space;
	return ret;
}

static int setup_frame_size_with_refs(
		struct VP9Decoder_s *pbi,
		struct VP9_Common_s *cm,
		union param_u *params,
		unsigned int *mmu_index_adr,
		int print_header_info) {

	int width, height;
	int found = 0, i;
	int has_valid_ref_frame = 0;
	struct PIC_BUFFER_CONFIG_s *ybf;
	struct BufferPool_s * const pool = cm->buffer_pool;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	int ret = 0;

	for (i = 0; i < REFS_PER_FRAME; ++i) {
		if ((params->p.same_frame_size >>
				(REFS_PER_FRAME - i - 1)) & 0x1) {
			struct PIC_BUFFER_CONFIG_s *const buf = cm->frame_refs[i].buf;

			width = buf->y_crop_width;
			height = buf->y_crop_height;
			found = 1;
			break;
		}
	}

	if (!found) {
		width = params->p.width;
		height = params->p.height;
	}

	if (is_oversize(width, height)) {
		pbi->error_frame_width = width;
		pbi->error_frame_height = height;
		vp9_print(pbi, 0, "%s, Error: Invalid frame size, width: %d height: %d\n",
			__func__, width, height);
		vdec_v4l_post_error_event(ctx, DECODER_WARNING_DATA_ERROR);
		return -1;
	}
	pbi->error_frame_width = 0;
	pbi->error_frame_height = 0;

	params->p.width = width;
	params->p.height = height;

	WRITE_VREG(HEVC_PARSER_PICTURE_SIZE, (height << 16) | width);
	if (pbi->mmu_enable && ((pbi->double_write_mode & 0x10) == 0)) {
		for (i = 0; i < BUF_FBC_NUM_MAX; i++) {
			if (pbi->m_BUF[cm->new_fb_idx].v4l_ref_buf_addr
				== pbi->afbc_buf_table[i].fb) {
				cm->cur_fb_idx_mmu = i;
				vp9_print(pbi, VP9_DEBUG_BUFMGR,
					"cur fb idx mmu %d\n",
					i);
				break;
			}
		}
		if (i >= BUF_FBC_NUM_MAX) {
			vp9_print(pbi, 0, "[ERR]can't find fb(0x%lx) in afbc table\n",
					pbi->m_BUF[cm->new_fb_idx].v4l_ref_buf_addr);
			ret = -1;
		} else {
			ret = vp9_alloc_mmu(pbi, cm->cur_fb_idx_mmu,
				params->p.width, params->p.height,
				params->p.bit_depth, mmu_index_adr);
		}

		if (ret != 0) {
			pr_err("can't alloc need mmu,idx %d\r\n",
				cm->new_fb_idx);
			return ret;
		}
		//cm->cur_fb_idx_mmu = cm->new_fb_idx;
	}

	/*Check to make sure at least one of frames that this frame references
	 *has valid dimensions.
	 */
	for (i = 0; i < REFS_PER_FRAME; ++i) {
		struct RefBuffer_s * const ref_frame = &cm->frame_refs[i];

		has_valid_ref_frame |=
			valid_ref_frame_size(ref_frame->buf->y_crop_width,
			ref_frame->buf->y_crop_height,
			width, height);
	}
	if (!has_valid_ref_frame) {
		pr_err("Error: Referenced frame has invalid size\r\n");
		return -1;
	}

	resize_context_buffers(pbi, cm, width, height);
	setup_display_size(cm, params, print_header_info);

	/* porting */
	ybf = get_frame_new_buffer(cm);
	if (!ybf)
		return -1;

	ybf->y_crop_width = width;
	ybf->y_crop_height = height;
	ybf->bit_depth = params->p.bit_depth;

	pool->frame_bufs[cm->new_fb_idx].buf.subsampling_x = cm->subsampling_x;
	pool->frame_bufs[cm->new_fb_idx].buf.subsampling_y = cm->subsampling_y;
	pool->frame_bufs[cm->new_fb_idx].buf.bit_depth =
						(unsigned int)cm->bit_depth;
	pool->frame_bufs[cm->new_fb_idx].buf.color_space = cm->color_space;
	return ret;
}

static inline bool close_to(int a, int b, int m)
{
	return (abs(a - b) < m) ? true : false;
}

#ifdef MULTI_INSTANCE_SUPPORT
static int vp9_print_cont(struct VP9Decoder_s *pbi,
	int flag, const char *fmt, ...)
{
	unsigned char buf[HEVC_PRINT_BUF];
	int len = 0;

	if (pbi == NULL ||
		(flag == 0) ||
		(debug & flag)) {
		va_list args;

		va_start(args, fmt);
		vsnprintf(buf + len, HEVC_PRINT_BUF - len, fmt, args);
		pr_info("%s", buf);
		va_end(args);
	}
	return 0;
}

static void trigger_schedule(struct VP9Decoder_s *pbi)
{
	struct aml_vcodec_ctx *ctx =
		(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);

	if (ctx->param_sets_from_ucode &&
		!pbi->v4l_params_parsed)
		vdec_v4l_write_frame_sync(ctx);

	vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_0, 0);

	if (pbi->vdec_cb)
		pbi->vdec_cb(hw_to_vdec(pbi), pbi->vdec_cb_arg, CORE_MASK_HEVC);
}

static void reset_process_time(struct VP9Decoder_s *pbi)
{
	if (pbi->start_process_time) {
		unsigned process_time =
			1000 * (jiffies - pbi->start_process_time) / HZ;
		pbi->start_process_time = 0;
		if (process_time > max_process_time[pbi->index])
			max_process_time[pbi->index] = process_time;
	}
}

static void start_process_time(struct VP9Decoder_s *pbi)
{
	pbi->start_process_time = jiffies;
	pbi->decode_timeout_count = 0;
	pbi->last_lcu_idx = 0;
}

static void timeout_process(struct VP9Decoder_s *pbi)
{
	struct aml_vcodec_ctx * ctx = pbi->v4l2_ctx;

	pbi->timeout_num++;
	if (pbi->process_busy) {
		vp9_print(pbi,
			0, "%s decoder timeout but process_busy\n", __func__);
		return;
	}
	amhevc_stop();
	vp9_print(pbi, 0, "%s decoder timeout\n", __func__);

	vdec_v4l_post_error_event(ctx, DECODER_WARNING_DECODER_TIMEOUT);

	pbi->timeout = true;
	pbi->dec_result = DEC_RESULT_DONE;
	reset_process_time(pbi);
	vdec_schedule_work(&pbi->work);
}

static u32 get_valid_double_write_mode(struct VP9Decoder_s *pbi)
{
	return ((double_write_mode & 0x80000000) == 0) ?
		pbi->double_write_mode :
		(double_write_mode & 0x7fffffff);
}

static int get_double_write_mode(struct VP9Decoder_s *pbi)
{
	u32 dw;

	vdec_v4l_get_dw_mode(pbi->v4l2_ctx, &dw);

	return dw;
}

/* for double write buf alloc */
static int get_double_write_mode_init(struct VP9Decoder_s *pbi)
{
	u32 valid_dw_mode = get_valid_double_write_mode(pbi);
	u32 dw;
	int w = pbi->init_pic_w;
	int h = pbi->init_pic_h;

	dw = 0x1; /*1:1*/
	switch (valid_dw_mode) {
	case 0x100:
		if (w > 1920 && h > 1088)
			dw = 0x4; /*1:2*/
		break;
	case 0x200:
		if (w > 1920 && h > 1088)
			dw = 0x2; /*1:4*/
		break;
	case 0x300:
		if (w > 1280 && h > 720)
			dw = 0x4; /*1:2*/
		break;
	default:
		dw = valid_dw_mode;
		break;
	}
	return dw;
}
#endif

/* return page number */
static int vp9_mmu_page_num(struct VP9Decoder_s *pbi,
		int w, int h, bool is_bit_depth_10)
{
	int picture_size;
	int cur_mmu_4k_number, max_frame_num;

	picture_size = compute_losless_comp_body_size(w, h, is_bit_depth_10);
	cur_mmu_4k_number = ((picture_size + (PAGE_SIZE - 1)) >> PAGE_SHIFT);

	max_frame_num = (vvp9_frame_mmu_map_size(pbi) >> 2);

	if (cur_mmu_4k_number > max_frame_num) {
		pr_err("over max !! cur_mmu_4k_number 0x%x width %d height %d\n",
			cur_mmu_4k_number, w, h);
		return -1;
	}

	return cur_mmu_4k_number;
}

static struct aml_buf *index_to_afbc_aml_buf(struct VP9Decoder_s *pbi, int index)
{
	return (struct aml_buf *)pbi->afbc_buf_table[index].fb;
}

static struct aml_buf *index_to_aml_buf(struct VP9Decoder_s *pbi, int index)
{
	return (struct aml_buf *)pbi->m_BUF[index].v4l_ref_buf_addr;
}


int vp9_alloc_mmu(
	struct VP9Decoder_s *pbi,
	int cur_buf_idx,
	int pic_width,
	int pic_height,
	unsigned short bit_depth,
	unsigned int *mmu_index_adr)
{
	int ret;
	bool is_bit_depth_10 = (bit_depth == VPX_BITS_10);
	int cur_mmu_4k_number;
	struct aml_buf *aml_buf;
	struct aml_vcodec_ctx * ctx = pbi->v4l2_ctx;

	if (get_double_write_mode(pbi) == 0x10)
		return 0;

	if (bit_depth >= VPX_BITS_12) {
		pbi->fatal_error = DECODER_FATAL_ERROR_SIZE_OVERFLOW;
		pr_err("fatal_error, un support bit depth 12!\n\n");
		vdec_v4l_post_error_event(ctx, DECODER_EMERGENCY_UNSUPPORT);
		return -1;
	}

	cur_mmu_4k_number = vp9_mmu_page_num(pbi,
				pic_width,
				pic_height,
				is_bit_depth_10);
	if (cur_mmu_4k_number < 0)
		return -1;
	ATRACE_COUNTER(pbi->trace.decode_header_memory_time_name, TRACE_HEADER_MEMORY_START);

	aml_buf = index_to_afbc_aml_buf(pbi, cur_buf_idx);

	ret = decoder_mmu_box_alloc_idx(
				aml_buf->fbc->mmu,
				aml_buf->fbc->index,
				aml_buf->fbc->frame_size,
				mmu_index_adr);

	if (!ret)
		aml_buf->fbc->used[aml_buf->fbc->index] |= 1;

	vp9_print(pbi, VP9_DEBUG_BUFMGR, "%s afbc_index %d, haddr:%lx, dma 0x%lx\n",
			__func__, aml_buf->fbc->index, aml_buf->fbc->haddr, aml_buf->planes[0].addr);

	ATRACE_COUNTER(pbi->trace.decode_header_memory_time_name, TRACE_HEADER_MEMORY_END);
	if (ret < 0) {
		vdec_v4l_post_error_event(ctx, DECODER_ERROR_ALLOC_BUFFER_FAIL);
	}
	return ret;
}

#ifndef MV_USE_FIXED_BUF
static void dealloc_mv_bufs(struct VP9Decoder_s *pbi)
{
	int i;
	for (i = 0; i < MV_BUFFER_NUM; i++) {
		if (pbi->m_mv_BUF[i].start_adr) {
			if (debug)
				pr_info(
				"dealloc mv buf(%d) adr %ld size 0x%x used_flag %d\n",
				i, pbi->m_mv_BUF[i].start_adr,
				pbi->m_mv_BUF[i].size,
				pbi->m_mv_BUF[i].used_flag);
			decoder_bmmu_box_free_idx(
				pbi->bmmu_box,
				MV_BUFFER_IDX(i));
			pbi->m_mv_BUF[i].start_adr = 0;
			pbi->m_mv_BUF[i].size = 0;
			pbi->m_mv_BUF[i].used_flag = 0;
		}
	}
}

static int alloc_mv_buf(struct VP9Decoder_s *pbi,
	int i, int size)
{
	int ret = 0;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);

	if (pbi->m_mv_BUF[i].start_adr &&
		size > pbi->m_mv_BUF[i].size) {
		dealloc_mv_bufs(pbi);
	} else if (pbi->m_mv_BUF[i].start_adr)
		return 0;

	if (decoder_bmmu_box_alloc_buf_phy
		(pbi->bmmu_box,
		MV_BUFFER_IDX(i), size,
		DRIVER_NAME,
		&pbi->m_mv_BUF[i].start_adr) < 0) {
		pbi->m_mv_BUF[i].start_adr = 0;
		ret = -1;
		vdec_v4l_post_error_event(ctx, DECODER_ERROR_ALLOC_BUFFER_FAIL);
	} else {
		if (!vdec_secure(hw_to_vdec(pbi)))
			vdec_mm_dma_flush(pbi->m_mv_BUF[i].start_adr, size);
		pbi->m_mv_BUF[i].size = size;
		pbi->m_mv_BUF[i].used_flag = 0;
		ret = 0;
		if (debug) {
			pr_info(
			"MV Buffer %d: start_adr %px size %x\n",
			i,
			(void *)pbi->m_mv_BUF[i].start_adr,
			pbi->m_mv_BUF[i].size);
		}
	}
	return ret;
}

static int cal_mv_buf_size(struct VP9Decoder_s *pbi, int pic_width, int pic_height)
{
	int lcu_size = 64; /*fixed 64*/
	int pic_width_64 = (pic_width + 63) & (~0x3f);
	int pic_height_32 = (pic_height + 31) & (~0x1f);
	int pic_width_lcu  = (pic_width_64 % lcu_size) ?
				pic_width_64 / lcu_size  + 1
				: pic_width_64 / lcu_size;
	int pic_height_lcu = (pic_height_32 % lcu_size) ?
				pic_height_32 / lcu_size + 1
				: pic_height_32 / lcu_size;
	int lcu_total       = pic_width_lcu * pic_height_lcu;
	int size_a = lcu_total * 36 * 16;
	int size_b = pic_width_lcu * 16 *
		((pic_height_lcu >> 3) + (pic_height_lcu & 0x7));
	int size = (size_a + size_b + 0xffff) &
		(~0xffff);

	return size;
}


static int init_mv_buf_list(struct VP9Decoder_s *pbi)
{
	int i;
	int ret = 0;
	int count = 2;
	int pic_width = pbi->init_pic_w;
	int pic_height = pbi->init_pic_h;
	int size = cal_mv_buf_size(pbi, pic_width, pic_height);

	if (mv_buf_dynamic_alloc)
		return 0;

	if (debug) {
		pr_info("%s w:%d, h:%d, count: %d\n",
		__func__, pbi->init_pic_w, pbi->init_pic_h, count);
	}

	for (i = 0;
		i < count; i++) {
		if (alloc_mv_buf(pbi, i, size) < 0) {
			ret = -1;
			break;
		}
	}
	return ret;
}

static int get_mv_buf(struct VP9Decoder_s *pbi,
		struct PIC_BUFFER_CONFIG_s *pic_config)
{
	int i;
	int ret = -1;
	if (mv_buf_dynamic_alloc) {
		union param_u *params = &pbi->vp9_param;
		int size = cal_mv_buf_size(pbi,
			params->p.width, params->p.height);
		for (i = 0; i < MV_BUFFER_NUM; i++) {
			if (pbi->m_mv_BUF[i].start_adr == 0) {
				ret = i;
				break;
			}
		}
		if (i == MV_BUFFER_NUM) {
			pr_info(
			"%s: Error, mv buf MV_BUFFER_NUM is not enough\n",
			__func__);
			return ret;
		}

		if (alloc_mv_buf(pbi, ret, size) >= 0) {
			pic_config->mv_buf_index = ret;
			pic_config->mpred_mv_wr_start_addr =
				(pbi->m_mv_BUF[ret].start_adr + 0xffff) &
				(~0xffff);
			pic_config->mv_size = size;

			if (debug & VP9_DEBUG_BUFMGR_MORE)
				pr_info(
				"%s alloc => %d (%ld) size 0x%x\n",
				__func__, ret,
				pic_config->mpred_mv_wr_start_addr,
				pic_config->mv_size);
		} else {
			pr_info(
			"%s: Error, mv buf alloc fail\n",
			__func__);
		}
		return ret;
	}

	for (i = 0; i < MV_BUFFER_NUM; i++) {
		if (pbi->m_mv_BUF[i].start_adr &&
			pbi->m_mv_BUF[i].used_flag == 0) {
			pbi->m_mv_BUF[i].used_flag = 1;
			ret = i;
			break;
		}
	}

	if (ret >= 0) {
		pic_config->mv_buf_index = ret;
		pic_config->mpred_mv_wr_start_addr =
			(pbi->m_mv_BUF[ret].start_adr + 0xffff) &
			(~0xffff);
		pic_config->mv_size = pbi->m_mv_BUF[ret].size;
		if (debug & VP9_DEBUG_BUFMGR_MORE)
			pr_info("%s => %d (%lx) size 0x%x\n", __func__, ret,
				pic_config->mpred_mv_wr_start_addr, pic_config->mv_size);
	} else {
		pr_info("%s: Error, mv buf is not enough\n", __func__);
	}
	return ret;
}

static void put_mv_buf(struct VP9Decoder_s *pbi,
				int *mv_buf_index)
{
	int i = *mv_buf_index;
	if (i >= MV_BUFFER_NUM) {
		if (debug & VP9_DEBUG_BUFMGR_MORE)
			pr_info("%s: index %d beyond range\n", __func__, i);
		return;
	}

	if (mv_buf_dynamic_alloc) {
		if (pbi->m_mv_BUF[i].start_adr) {
			if (debug)
				pr_info(
				"dealloc mv buf(%d) adr %ld size 0x%x used_flag %d\n",
				i, pbi->m_mv_BUF[i].start_adr,
				pbi->m_mv_BUF[i].size,
				pbi->m_mv_BUF[i].used_flag);
			decoder_bmmu_box_free_idx(
				pbi->bmmu_box,
				MV_BUFFER_IDX(i));
			pbi->m_mv_BUF[i].start_adr = 0;
			pbi->m_mv_BUF[i].size = 0;
			pbi->m_mv_BUF[i].used_flag = 0;
		}
		*mv_buf_index = -1;
		return;
	}
	if (debug & VP9_DEBUG_BUFMGR_MORE)
		pr_info("%s(%d): used_flag(%d)\n", __func__, i,
			pbi->m_mv_BUF[i].used_flag);

	*mv_buf_index = -1;
	if (pbi->m_mv_BUF[i].start_adr &&
		pbi->m_mv_BUF[i].used_flag)
		pbi->m_mv_BUF[i].used_flag = 0;
}

static void	put_un_used_mv_bufs(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	int i;
	for (i = 0; i < pbi->used_buf_num; ++i) {
		if ((&frame_bufs[i] != cm->prev_frame) &&
			(frame_bufs[i].buf.index != -1) &&
			(frame_bufs[i].buf.mv_buf_index >= 0)
			)
			put_mv_buf(pbi, &frame_bufs[i].buf.mv_buf_index);
	}
}

#ifdef SUPPORT_FB_DECODING
static bool mv_buf_available(struct VP9Decoder_s *pbi)
{
	int i;
	bool ret = 0;
	for (i = 0; i < MV_BUFFER_NUM; i++) {
		if (pbi->m_mv_BUF[i].start_adr &&
			pbi->m_mv_BUF[i].used_flag == 0) {
			ret = 1;
			break;
		}
	}
	return ret;
}
#endif
#endif

#ifdef SUPPORT_FB_DECODING
static void init_stage_buf(struct VP9Decoder_s *pbi)
{
	uint i;
	for (i = 0; i < STAGE_MAX_BUFFERS
		&& i < stage_buf_num; i++) {
		pbi->stage_bufs[i] =
			vmalloc(sizeof(struct stage_buf_s));
		if (pbi->stage_bufs[i] == NULL) {
			vp9_print(pbi,
				0, "%s vmalloc fail\n", __func__);
			break;
		}
		pbi->stage_bufs[i]->index = i;
	}
	pbi->used_stage_buf_num = i;
	pbi->s1_pos = 0;
	pbi->s2_pos = 0;
	pbi->s1_buf = NULL;
	pbi->s2_buf = NULL;
	pbi->s1_mv_buf_index = FRAME_BUFFERS;
	pbi->s1_mv_buf_index_pre = FRAME_BUFFERS;
	pbi->s1_mv_buf_index_pre_pre = FRAME_BUFFERS;

	if (pbi->used_stage_buf_num > 0)
		vp9_print(pbi, 0, "%s 2 stage decoding buf %d\n",
			__func__, pbi->used_stage_buf_num);
}

static void uninit_stage_buf(struct VP9Decoder_s *pbi)
{
	int i;
	for (i = 0; i < pbi->used_stage_buf_num; i++) {
		if (pbi->stage_bufs[i])
			vfree(pbi->stage_bufs[i]);
		pbi->stage_bufs[i] = NULL;
	}
	pbi->used_stage_buf_num = 0;
	pbi->s1_pos = 0;
	pbi->s2_pos = 0;
	pbi->s1_buf = NULL;
	pbi->s2_buf = NULL;
}

static int get_s1_buf(
	struct VP9Decoder_s *pbi)
{
	struct stage_buf_s *buf = NULL;
	int ret = -1;
	int buf_page_num = MAX_STAGE_PAGE_NUM;
	int next_s1_pos = pbi->s1_pos + 1;

	if (next_s1_pos >= pbi->used_stage_buf_num)
		next_s1_pos = 0;
	if (next_s1_pos == pbi->s2_pos) {
		pbi->s1_buf = NULL;
		return ret;
	}

	buf = pbi->stage_bufs[pbi->s1_pos];
	ret = decoder_mmu_box_alloc_idx(
		pbi->mmu_box,
		buf->index,
		buf_page_num,
		pbi->stage_mmu_map_addr);
	if (ret < 0) {
		vp9_print(pbi, 0,
			"%s decoder_mmu_box_alloc fail for index %d (s1_pos %d s2_pos %d)\n",
			__func__, buf->index,
			pbi->s1_pos, pbi->s2_pos);
		buf = NULL;
	} else {
		vp9_print(pbi, VP9_DEBUG_2_STAGE,
			"%s decoder_mmu_box_alloc %d page for index %d (s1_pos %d s2_pos %d)\n",
			__func__, buf_page_num, buf->index,
			pbi->s1_pos, pbi->s2_pos);
	}
	pbi->s1_buf = buf;
	return ret;
}

static void inc_s1_pos(struct VP9Decoder_s *pbi)
{
	struct stage_buf_s *buf =
		pbi->stage_bufs[pbi->s1_pos];

	int used_page_num =
#ifdef FB_DECODING_TEST_SCHEDULE
		MAX_STAGE_PAGE_NUM/2;
#else
		(READ_VREG(HEVC_ASSIST_HED_FB_W_CTL) >> 16);
#endif
	decoder_mmu_box_free_idx_tail(pbi->mmu_box,
		FRAME_BUFFERS + buf->index, used_page_num);

	pbi->s1_pos++;
	if (pbi->s1_pos >= pbi->used_stage_buf_num)
		pbi->s1_pos = 0;

	vp9_print(pbi, VP9_DEBUG_2_STAGE,
		"%s (used_page_num %d) for index %d (s1_pos %d s2_pos %d)\n",
		__func__, used_page_num, buf->index,
		pbi->s1_pos, pbi->s2_pos);
}

#define s2_buf_available(pbi) (pbi->s1_pos != pbi->s2_pos)

static int get_s2_buf(
	struct VP9Decoder_s *pbi)
{
	int ret = -1;
	struct stage_buf_s *buf = NULL;
	if (s2_buf_available(pbi)) {
		buf = pbi->stage_bufs[pbi->s2_pos];
		vp9_print(pbi, VP9_DEBUG_2_STAGE,
			"%s for index %d (s1_pos %d s2_pos %d)\n",
			__func__, buf->index,
			pbi->s1_pos, pbi->s2_pos);
		pbi->s2_buf = buf;
		ret = 0;
	}
	return ret;
}

static void inc_s2_pos(struct VP9Decoder_s *pbi)
{
	struct stage_buf_s *buf =
		pbi->stage_bufs[pbi->s2_pos];
	decoder_mmu_box_free_idx(pbi->mmu_box,
		FRAME_BUFFERS + buf->index);
	pbi->s2_pos++;
	if (pbi->s2_pos >= pbi->used_stage_buf_num)
		pbi->s2_pos = 0;
	vp9_print(pbi, VP9_DEBUG_2_STAGE,
		"%s for index %d (s1_pos %d s2_pos %d)\n",
		__func__, buf->index,
		pbi->s1_pos, pbi->s2_pos);
}

static int get_free_stage_buf_num(struct VP9Decoder_s *pbi)
{
	int num;
	if (pbi->s1_pos >= pbi->s2_pos)
		num = pbi->used_stage_buf_num -
			(pbi->s1_pos - pbi->s2_pos) - 1;
	else
		num = (pbi->s2_pos - pbi->s1_pos) - 1;
	return num;
}

#ifndef FB_DECODING_TEST_SCHEDULE
static DEFINE_SPINLOCK(fb_core_spin_lock);

static u8 is_s2_decoding_finished(struct VP9Decoder_s *pbi)
{
	/* to do: VLSI review
		completion of last LCU decoding in BACK
	 */
	return 1;
}

static void start_s1_decoding(struct VP9Decoder_s *pbi)
{
	/* to do: VLSI review
	 after parser, how to start LCU decoding in BACK
	*/
}

static void fb_reset_core(struct vdec_s *vdec, u32 mask)
{
	/* to do: VLSI review
		1. how to disconnect DMC for FRONT and BACK
		2. reset bit 13, 24, FRONT or BACK ??
	*/

	unsigned long flags;
	u32 reset_bits = 0;
	if (mask & HW_MASK_FRONT)
		WRITE_VREG(HEVC_STREAM_CONTROL, 0);
	spin_lock_irqsave(&fb_core_spin_lock, flags);
	codec_dmcbus_write(DMC_REQ_CTRL,
		codec_dmcbus_read(DMC_REQ_CTRL) & (~(1 << 4)));
	spin_unlock_irqrestore(&fb_core_spin_lock, flags);

	while (!(codec_dmcbus_read(DMC_CHAN_STS) & (1 << 4)));

	if ((mask & HW_MASK_FRONT) &&
		input_frame_based(vdec))
		WRITE_VREG(HEVC_STREAM_CONTROL, 0);

	/*
	 * 2: assist
	 * 3: parser
	 * 4: parser_state
	 * 8: dblk
	 * 11:mcpu
	 * 12:ccpu
	 * 13:ddr
	 * 14:iqit
	 * 15:ipp
	 * 17:qdct
	 * 18:mpred
	 * 19:sao
	 * 24:hevc_afifo
	 */
	if (mask & HW_MASK_FRONT) {
		reset_bits =
		(1<<3)|(1<<4)|(1<<11)|
		(1<<12)|(1<<18);
	}
	if (mask & HW_MASK_BACK) {
		reset_bits =
		(1<<8)|(1<<13)|(1<<14)|(1<<15)|
		(1<<17)|(1<<19)|(1<<24);
	}
	WRITE_VREG(DOS_SW_RESET3, reset_bits);
	WRITE_VREG(DOS_SW_RESET3, 0);


	spin_lock_irqsave(&fb_core_spin_lock, flags);
	codec_dmcbus_write(DMC_REQ_CTRL,
		codec_dmcbus_read(DMC_REQ_CTRL) | (1 << 4));
	spin_unlock_irqrestore(&fb_core_spin_lock, flags);

}
#endif

#endif

static void init_pic_list_hw(struct VP9Decoder_s *pbi);

static void update_hide_frame_timestamp(struct VP9Decoder_s *pbi)
{
	struct RefCntBuffer_s *const frame_bufs =
		pbi->common.buffer_pool->frame_bufs;
	int i;

	for (i = 0; i < pbi->used_buf_num; ++i) {
		if ((!frame_bufs[i].show_frame) &&
			(!frame_bufs[i].buf.vf_ref) &&
			(frame_bufs[i].buf.BUF_index != -1)) {
			frame_bufs[i].buf.timestamp = pbi->chunk->timestamp;
			vp9_print(pbi, VP9_DEBUG_OUT_PTS,
				"%s, update %d hide frame ts: %lld\n",
				__func__, i, frame_bufs[i].buf.timestamp);
		}
	}
}

static int get_idle_pos(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	int i;

	for (i = 0; i < pbi->used_buf_num; ++i) {
		if ((frame_bufs[i].ref_count == 0) &&
			(frame_bufs[i].buf.vf_ref == 0) &&
			!frame_bufs[i].buf.cma_alloc_addr &&
			(frame_bufs[i].buf.repeat_count == 0))
			break;
	}

	return (i != pbi->used_buf_num) ? i : -1;
}

static int v4l_get_free_fb(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	struct PIC_BUFFER_CONFIG_s *pic = NULL;
	struct PIC_BUFFER_CONFIG_s *free_pic = NULL;
	int pos = get_idle_pos(pbi);

	if (pos < 0) {
		vp9_print(pbi, 0,
			"get_idle_pos fail!\n");
		return INVALID_IDX;
	}

	pic = &frame_bufs[pos].buf;
	if (v4l_alloc_and_config_pic(pbi, pic))
		return INVALID_IDX;

	pic->BUF_index		= pos;
	pic->y_crop_width	= pbi->frame_width;
	pic->y_crop_height	= pbi->frame_height;
	free_pic		= pic;

	set_canvas(pbi, pic);
	init_pic_list_hw(pbi);

	if (free_pic) {
		frame_bufs[free_pic->BUF_index].ref_count = 1;
		if (pbi->chunk) {
			free_pic->timestamp = pbi->chunk->timestamp;
			update_hide_frame_timestamp(pbi);
		}
	}

	if (free_pic) {
		struct aml_vcodec_ctx *ctx =
			(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);

		pbi->aml_buf->state = FB_ST_DECODER;

		aml_buf_get_ref(&ctx->bm, pbi->aml_buf);
		pbi->cur_idx = pos;
		pbi->aml_buf = NULL;
	}

	if (debug & VP9_DEBUG_OUT_PTS) {
		if (free_pic) {
			pr_debug("%s, idx: %d, ts: %lld\n",
				__func__, free_pic->index, free_pic->timestamp);
		} else {
			pr_debug("%s, vp9 get free pic null\n", __func__);
			dump_pic_list(pbi);
		}
	}

	return free_pic ? free_pic->index : INVALID_IDX;
}

static void force_recycle_repeat_frame(struct VP9Decoder_s *pbi)
{
	int i;
	if (pbi->repeat_buf.used_size &&
		pbi->repeat_buf.frame_bufs[0]) {
		pbi->repeat_buf.frame_bufs[0]->buf.repeat_pic = NULL;
		pbi->repeat_buf.frame_bufs[0]->buf.repeat_count = 0;

		vp9_print(pbi, PRINT_FLAG_VDEC_DETAIL,
		"force remove the first frame slot! "
		"frame idx: %d addr: %lx used_size %d\n",
		pbi->repeat_buf.frame_bufs[0]->buf.index,
		pbi->repeat_buf.frame_bufs[0]->buf.cma_alloc_addr,
		pbi->repeat_buf.used_size);

		for (i = 0; i < pbi->repeat_buf.used_size - 1; i++)
			pbi->repeat_buf.frame_bufs[i] =
					pbi->repeat_buf.frame_bufs[i + 1];
		pbi->repeat_buf.frame_bufs[pbi->repeat_buf.used_size - 1] = NULL;
		pbi->repeat_buf.used_size--;
	}
}

static int get_free_buf_count(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	int i, free_count = 0;
	int free_slot = 0;

	if (pbi->is_used_v4l) {
		pbi->cur_idx = INVALID_IDX;
		if (pbi->aml_buf)
			return pbi->run_ready_min_buf_num;

		for (i = 0; i < pbi->used_buf_num; ++i) {
			vp9_print(pbi,
			PRINT_FLAG_VDEC_DETAIL, "%s idx %d ref_count %d vf_ref %d cma_alloc_addr = 0x%lx\n",
			__func__, i, frame_bufs[i].ref_count,
			frame_bufs[i].buf.vf_ref,
			frame_bufs[i].buf.cma_alloc_addr);
			if ((frame_bufs[i].ref_count == 0) &&
				(frame_bufs[i].buf.vf_ref == 0) &&
				!frame_bufs[i].buf.cma_alloc_addr &&
				(frame_bufs[i].buf.repeat_count == 0) &&
				(cm->cur_frame != &frame_bufs[i])) {
				free_slot++;
			}
		}

		if (free_slot < pbi->run_ready_min_buf_num) {
			vp9_print(pbi,
			PRINT_FLAG_VDEC_DETAIL, "%s not enough free_slot %d!\n",
			__func__, free_slot);
			force_recycle_repeat_frame(pbi);

			return false;
		} else if (free_slot < 2)
				force_recycle_repeat_frame(pbi);

		if (!pbi->aml_buf && !aml_buf_empty(&ctx->bm)) {
			pbi->aml_buf = aml_buf_get(&ctx->bm, BUF_USER_DEC, false);
			if (!pbi->aml_buf) {
				return false;
			}

			pbi->aml_buf->task->attach(pbi->aml_buf->task, &task_dec_ops, hw_to_vdec(pbi));
			pbi->aml_buf->state = FB_ST_DECODER;
		}

		if (pbi->aml_buf) {
			free_count++;
			vp9_print(pbi,
			PRINT_FLAG_VDEC_DETAIL, "%s get fb: 0x%lx fb idx: %d\n",
			__func__, pbi->aml_buf, pbi->aml_buf->index);
		}

		/* trigger to parse head data. */
		if (!pbi->v4l_params_parsed) {
			free_count = pbi->run_ready_min_buf_num;
		}
	} else {
		for (i = 0; i < pbi->used_buf_num; ++i) {
			if ((frame_bufs[i].ref_count == 0) &&
				(frame_bufs[i].buf.vf_ref == 0) &&
				(frame_bufs[i].buf.index != -1))
				free_count++;
		}
	}

	return free_count;
}


static int check_free_buf_count(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	int i;
	int free_slot = 0;

	for (i = 0; i < pbi->used_buf_num; ++i) {
		if ((frame_bufs[i].ref_count == 0) &&
			(frame_bufs[i].buf.vf_ref == 0) &&
			!frame_bufs[i].buf.cma_alloc_addr &&
			(cm->cur_frame != &frame_bufs[i])) {
			free_slot++;
		}
	}

	return free_slot;
}


static void decrease_ref_count(int idx, struct RefCntBuffer_s *const frame_bufs,
					struct BufferPool_s *const pool)
{
	if (idx >= 0) {
		--frame_bufs[idx].ref_count;
		/*A worker may only get a free framebuffer index when
		 *calling get_free_fb. But the private buffer is not set up
		 *until finish decoding header. So any error happens during
		 *decoding header, the frame_bufs will not have valid priv
		 *buffer.
		 */

		if (frame_bufs[idx].ref_count == 0 &&
			frame_bufs[idx].raw_frame_buffer.priv)
			vp9_release_frame_buffer
			(&frame_bufs[idx].raw_frame_buffer);
	}
}

static void generate_next_ref_frames(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *frame_bufs = cm->buffer_pool->frame_bufs;
	struct BufferPool_s *const pool = cm->buffer_pool;
	int mask, ref_index = 0;
	unsigned long flags;

	/* Generate next_ref_frame_map.*/
	lock_buffer_pool(pool, flags);
	for (mask = pbi->refresh_frame_flags; mask; mask >>= 1) {
		if (mask & 1) {
			cm->next_ref_frame_map[ref_index] = cm->new_fb_idx;
			++frame_bufs[cm->new_fb_idx].ref_count;
		} else
			cm->next_ref_frame_map[ref_index] =
				cm->ref_frame_map[ref_index];
		/* Current thread holds the reference frame.*/
		if (cm->ref_frame_map[ref_index] >= 0) {
			++frame_bufs[cm->ref_frame_map[ref_index]].ref_count;
		}
		++ref_index;
	}

	for (; ref_index < REF_FRAMES; ++ref_index) {
		cm->next_ref_frame_map[ref_index] =
			cm->ref_frame_map[ref_index];
		/* Current thread holds the reference frame.*/
		if (cm->ref_frame_map[ref_index] >= 0) {
			++frame_bufs[cm->ref_frame_map[ref_index]].ref_count;
		}
	}
	unlock_buffer_pool(pool, flags);
	return;
}

static void refresh_ref_frames(struct VP9Decoder_s *pbi)

{
	struct VP9_Common_s *const cm = &pbi->common;
	struct BufferPool_s *pool = cm->buffer_pool;
	struct RefCntBuffer_s *frame_bufs = cm->buffer_pool->frame_bufs;
	int mask, ref_index = 0;
	unsigned long flags;

	lock_buffer_pool(pool, flags);
	for (mask = pbi->refresh_frame_flags; mask; mask >>= 1) {
		const int old_idx = cm->ref_frame_map[ref_index];
		/*Current thread releases the holding of reference frame.*/
		decrease_ref_count(old_idx, frame_bufs, pool);

		/*Release the reference frame in reference map.*/
		if ((mask & 1) && old_idx >= 0)
			decrease_ref_count(old_idx, frame_bufs, pool);
		cm->ref_frame_map[ref_index] =
			cm->next_ref_frame_map[ref_index];
		++ref_index;
	}

	/*Current thread releases the holding of reference frame.*/
	for (; ref_index < REF_FRAMES && !cm->show_existing_frame;
		++ref_index) {
		const int old_idx = cm->ref_frame_map[ref_index];

		decrease_ref_count(old_idx, frame_bufs, pool);
		cm->ref_frame_map[ref_index] =
			cm->next_ref_frame_map[ref_index];
	}
	unlock_buffer_pool(pool, flags);
	return;
}

int vp9_bufmgr_process(struct VP9Decoder_s *pbi, union param_u *params)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct BufferPool_s *pool = cm->buffer_pool;
	struct RefCntBuffer_s *frame_bufs = cm->buffer_pool->frame_bufs;
	struct PIC_BUFFER_CONFIG_s *pic = NULL;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	int i;
	int ret;

	pbi->ready_for_new_data = 0;

	if ((pbi->has_keyframe == 0) &&
		(params->p.frame_type != KEY_FRAME) &&
		(!params->p.intra_only)){
		on_no_keyframe_skiped++;
		pr_info("vp9_bufmgr_process no key frame return\n");
		return -2;
	}
	pbi->has_keyframe = 1;
	on_no_keyframe_skiped = 0;

	if (cm->new_fb_idx >= 0
		&& frame_bufs[cm->new_fb_idx].ref_count == 0){
		vp9_release_frame_buffer
			(&frame_bufs[cm->new_fb_idx].raw_frame_buffer);
	}

#ifndef MV_USE_FIXED_BUF
	put_un_used_mv_bufs(pbi);
	if (debug & VP9_DEBUG_BUFMGR_DETAIL)
		dump_pic_list(pbi);
#endif
	cm->new_fb_idx = v4l_get_free_fb(pbi);
	if (cm->new_fb_idx == INVALID_IDX) {
		pr_info("get_free_fb error\r\n");
		return -1;
	}
	frame_bufs[cm->new_fb_idx].buf.v4l_buf_index = cm->new_fb_idx;
#ifndef MV_USE_FIXED_BUF
#ifdef SUPPORT_FB_DECODING
	if (pbi->used_stage_buf_num == 0) {
#endif
		if (get_mv_buf(pbi,
			&pool->frame_bufs[cm->new_fb_idx].
			buf) < 0) {
			pr_info("get_mv_buf fail\r\n");
			return -1;
		}
		if (debug & VP9_DEBUG_BUFMGR_DETAIL)
			dump_pic_list(pbi);
#ifdef SUPPORT_FB_DECODING
	}
#endif
#endif
	cm->cur_frame = &pool->frame_bufs[cm->new_fb_idx];

	pbi->cur_buf = &frame_bufs[cm->new_fb_idx];
	if (pbi->mmu_enable) {
		cm->prev_fb_idx = cm->new_fb_idx;
	}
	/*read_uncompressed_header()*/
	cm->last_frame_type = cm->frame_type;
	cm->last_intra_only = cm->intra_only;
	cm->profile = params->p.profile;
	if (cm->profile >= MAX_PROFILES) {
		pr_err("Error: Unsupported profile %d\r\n", cm->profile);
		vdec_v4l_post_error_event(ctx, DECODER_EMERGENCY_UNSUPPORT);
		return -1;
	}
	cm->show_existing_frame = params->p.show_existing_frame;
	if (cm->show_existing_frame) {
		/* Show an existing frame directly.*/
		int frame_to_show_idx = params->p.frame_to_show_idx;
		int frame_to_show;
		unsigned long flags;
		struct PIC_BUFFER_CONFIG_s *tmp_pic = NULL;
		if (frame_to_show_idx >= REF_FRAMES) {
			pr_info("frame_to_show_idx %d exceed max index\r\n",
					frame_to_show_idx);
			return -1;
		}

		frame_to_show = cm->ref_frame_map[frame_to_show_idx];
		lock_buffer_pool(pool, flags);
		if (frame_to_show < 0 ||
			frame_bufs[frame_to_show].ref_count < 1) {
			unlock_buffer_pool(pool, flags);
			pr_err
			("Error:Buffer %d does not contain a decoded frame",
			frame_to_show);
			return -1;
		}

		tmp_pic = &cm->buffer_pool->frame_bufs[frame_to_show].buf;
		tmp_pic->timestamp = pbi->chunk->timestamp;
		tmp_pic->v4l_buf_index = frame_to_show;

		if (tmp_pic->vf_ref) {
			frame_bufs[frame_to_show].buf.repeat_count ++;
			frame_bufs[frame_to_show].buf.v4l_buf_index = cm->new_fb_idx;
			frame_bufs[cm->new_fb_idx].buf.repeat_pic = &frame_bufs[frame_to_show].buf;
			pbi->repeat_buf.frame_bufs[pbi->repeat_buf.used_size] =
						&frame_bufs[frame_to_show];

			pbi->repeat_buf.used_size++;
			vp9_print(pbi, 0,
				"repeat frame_bufs %px used size %d\n",
				pbi->repeat_buf.frame_bufs[pbi->repeat_buf.used_size], pbi->repeat_buf.used_size);
		}
		ref_cnt_fb(frame_bufs, &cm->new_fb_idx, frame_to_show);
		update_hide_frame_timestamp(pbi);
		unlock_buffer_pool(pool, flags);
		pbi->refresh_frame_flags = 0;
		cm->show_frame = 1;
		cm->cur_frame->show_frame = 1;

		/* do not decode, search next start code */
		return 1;
	}
	cm->frame_type = params->p.frame_type;
	cm->show_frame = params->p.show_frame;
	cm->bit_depth = params->p.bit_depth;
	cm->error_resilient_mode = params->p.error_resilient_mode;
	cm->cur_frame->show_frame = cm->show_frame;

	if (cm->frame_type == KEY_FRAME) {
		pbi->refresh_frame_flags = (1 << REF_FRAMES) - 1;

		for (i = 0; i < REFS_PER_FRAME; ++i) {
			cm->frame_refs[i].idx = INVALID_IDX;
			cm->frame_refs[i].buf = NULL;
		}

		ret = setup_frame_size(pbi,
			cm, params,  pbi->frame_mmu_map_addr,
				print_header_info);
		if (ret)
			return -1;
		if (pbi->need_resync) {
			memset(&cm->ref_frame_map, -1,
				sizeof(cm->ref_frame_map));
			pbi->need_resync = 0;
		}
	} else {
		cm->intra_only = cm->show_frame ? 0 : params->p.intra_only;

		cm->reset_frame_context = cm->error_resilient_mode ?
			0 : params->p.reset_frame_context;
		if (print_header_info) {
			if (cm->error_resilient_mode)
				pr_info("reset to 0 error_resilient_mode\n");
		else
			pr_info(" * 2-bits reset_frame_context read : %d\n",
				cm->reset_frame_context);
		}

		if (cm->intra_only) {
			if (cm->profile > PROFILE_0) {
				/*read_bitdepth_colorspace_sampling(cm,
				 *	rb, print_header_info);
				 */
			} else {
				/*NOTE: The intra-only frame header
				 *does not include the specification
				 *of either the color format or
				 *color sub-sampling
				 *in profile 0. VP9 specifies that the default
				 *color format should be YUV 4:2:0 in this
				 *case (normative).
				 */
				cm->color_space = VPX_CS_BT_601;
				cm->subsampling_y = cm->subsampling_x = 1;
				cm->bit_depth = VPX_BITS_8;
				cm->use_highbitdepth = 0;
			}

			pbi->refresh_frame_flags =
				params->p.refresh_frame_flags;
			ret = setup_frame_size(pbi,
				cm,
				params,
				pbi->frame_mmu_map_addr,
				print_header_info);
			if (ret)
				return -1;
			if (pbi->need_resync) {
				memset(&cm->ref_frame_map, -1,
					sizeof(cm->ref_frame_map));
				pbi->need_resync = 0;
			}
		} else if (pbi->need_resync != 1) {  /* Skip if need resync */
			pbi->refresh_frame_flags =
					params->p.refresh_frame_flags;
			if (print_header_info)
				pr_info("*%d-bits refresh_frame read:0x%x\n",
					REF_FRAMES, pbi->refresh_frame_flags);
			for (i = 0; i < REFS_PER_FRAME; ++i) {
				const int ref =
					(params->p.ref_info >>
					(((REFS_PER_FRAME-i-1)*4)+1))
					& 0x7;
				const int idx =
					cm->ref_frame_map[ref];
				struct RefBuffer_s * const ref_frame =
					&cm->frame_refs[i];
				if (print_header_info)
					pr_info("*%d-bits ref[%d]read:%d\n",
						REF_FRAMES_LOG2, i, ref);
				ref_frame->idx = idx;
				ref_frame->buf = &frame_bufs[idx].buf;
				cm->ref_frame_sign_bias[LAST_FRAME + i]
				= (params->p.ref_info >>
				((REFS_PER_FRAME-i-1)*4)) & 0x1;
				if (print_header_info)
					pr_info("1bit ref_frame_sign_bias");

			}

			ret = setup_frame_size_with_refs(
				pbi,
				cm,
				params,
				pbi->frame_mmu_map_addr,
				print_header_info);
			if (ret)
				return -1;
		}
	}

	pic = get_frame_new_buffer(cm);
	if (!pic)
		return -1;

	pic->bit_depth = cm->bit_depth;
	pic->color_space = cm->color_space;
	pic->slice_type = cm->frame_type;

	if (pbi->need_resync) {
		pr_err("Error: Keyframe/intra-only frame required to reset\r\n");
		return -1;
	}
	generate_next_ref_frames(pbi);
	pbi->hold_ref_buf = 1;

	/*end read_uncompressed_header()*/
	cm->use_prev_frame_mvs = !cm->error_resilient_mode &&
			cm->width == cm->last_width &&
			cm->height == cm->last_height &&
			!cm->last_intra_only &&
			cm->last_show_frame &&
			(cm->last_frame_type != KEY_FRAME);

	if (pbi->enable_fence && cm->show_frame) {
		struct PIC_BUFFER_CONFIG_s *pic = &cm->cur_frame->buf;
		struct vdec_s *vdec = hw_to_vdec(pbi);

		/* create fence for each buffers. */
		ret = vdec_timeline_create_fence(vdec->sync);
		if (ret < 0)
			return -3;

		pic->fence		= vdec->sync->fence;
		pic->bit_depth		= cm->bit_depth;
		pic->slice_type		= cm->frame_type;
		pic->stream_offset	= pbi->pre_stream_offset;

		if (pbi->chunk) {
			pic->pts	= pbi->chunk->pts;
			pic->pts64	= pbi->chunk->pts64;
			pic->timestamp	= pbi->chunk->timestamp;
		}

		/* post video vframe. */
		prepare_display_buf(pbi, pic);
	}

	return 0;
}


void swap_frame_buffers(struct VP9Decoder_s *pbi)
{
	int ref_index = 0;
	struct VP9_Common_s *const cm = &pbi->common;
	struct BufferPool_s *const pool = cm->buffer_pool;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	unsigned long flags;
	refresh_ref_frames(pbi);
	pbi->hold_ref_buf = 0;
	cm->frame_to_show = get_frame_new_buffer(cm);

	if (cm->frame_to_show) {
		lock_buffer_pool(pool, flags);
		--frame_bufs[cm->new_fb_idx].ref_count;
		unlock_buffer_pool(pool, flags);
	}

	/*Invalidate these references until the next frame starts.*/
	for (ref_index = 0; ref_index < 3; ref_index++)
		cm->frame_refs[ref_index].idx = -1;
}

int vp9_get_raw_frame(struct VP9Decoder_s *pbi, struct PIC_BUFFER_CONFIG_s *sd)
{
	struct VP9_Common_s *const cm = &pbi->common;
	int ret = -1;

	if (pbi->ready_for_new_data == 1)
		return ret;

	pbi->ready_for_new_data = 1;

	/* no raw frame to show!!! */
	if (!cm->show_frame)
		return ret;

	/* may not be get buff in v4l2 */
	if (!cm->frame_to_show)
		return ret;

	pbi->ready_for_new_data = 1;

	*sd = *cm->frame_to_show;
	ret = 0;

	return ret;
}

int vp9_bufmgr_init(struct VP9Decoder_s *pbi, struct BuffInfo_s *buf_spec_i,
		struct buff_s *mc_buf_i) {
	struct VP9_Common_s *cm = &pbi->common;

	pbi->frame_count = 0;
	pbi->pic_count = 0;
	pbi->pre_stream_offset = 0;
	cm->buffer_pool = &pbi->vp9_buffer_pool;
	spin_lock_init(&cm->buffer_pool->lock);
	cm->prev_fb_idx = INVALID_IDX;
	cm->new_fb_idx = INVALID_IDX;
	pbi->used_4k_num = -1;
	cm->cur_fb_idx_mmu = INVALID_IDX;
	pr_debug
	("After vp9_bufmgr_init, prev_fb_idx : %d, new_fb_idx : %d\r\n",
		cm->prev_fb_idx, cm->new_fb_idx);
	pbi->need_resync = 1;
	/* Initialize the references to not point to any frame buffers.*/
	memset(&cm->ref_frame_map, -1, sizeof(cm->ref_frame_map));
	memset(&cm->next_ref_frame_map, -1, sizeof(cm->next_ref_frame_map));
	cm->current_video_frame = 0;
	pbi->ready_for_new_data = 1;

	/* private init */
	pbi->work_space_buf = buf_spec_i;
	if (!pbi->mmu_enable)
		pbi->mc_buf = mc_buf_i;

	pbi->rpm_addr = NULL;
	pbi->lmem_addr = NULL;

	pbi->use_cma_flag = 0;
	pbi->decode_idx = 0;
	pbi->slice_idx = 0;
	pbi->has_keyframe = 0;
	pbi->skip_flag = 0;
	pbi->wait_buf = 0;
	pbi->error_flag = 0;

	pbi->pts_mode = PTS_NORMAL;
	pbi->last_pts = 0;
	pbi->last_lookup_pts = 0;
	pbi->last_pts_us64 = 0;
	pbi->last_lookup_pts_us64 = 0;
	pbi->shift_byte_count = 0;
	pbi->shift_byte_count_lo = 0;
	pbi->shift_byte_count_hi = 0;
	pbi->pts_mode_switching_count = 0;
	pbi->pts_mode_recovery_count = 0;

	pbi->buf_num = 0;
	pbi->pic_num = 0;

	return 0;
}

int vp9_bufmgr_postproc(struct VP9Decoder_s *pbi)
{
	struct vdec_s *vdec = hw_to_vdec(pbi);
	struct VP9_Common_s *cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s sd;

	if (pbi->postproc_done)
		return 0;
	pbi->postproc_done = 1;
	swap_frame_buffers(pbi);
	if (!cm->show_existing_frame) {
		cm->last_show_frame = cm->show_frame;
		cm->prev_frame = cm->cur_frame;
	}
	cm->last_width = cm->width;
	cm->last_height = cm->height;

	if (cm->show_frame)
		cm->current_video_frame++;

	if (vp9_get_raw_frame(pbi, &sd) == 0) {
		sd.stream_offset = pbi->pre_stream_offset;

		if (pbi->enable_fence) {
			int i, j, used_size, ret;
			int signed_count = 0;
			struct vframe_s *signed_fence[VF_POOL_SIZE];
			struct aml_buf *buf;
			/* notify signal to wake up wq of fence. */
			vdec_timeline_increase(vdec->sync, 1);
			mutex_lock(&pbi->fence_mutex);
			used_size = pbi->fence_vf_s.used_size;
			if (used_size) {
				for (i = 0, j = 0; i < VF_POOL_SIZE && j < used_size; i++) {
					if (pbi->fence_vf_s.fence_vf[i] != NULL) {
						ret = dma_fence_get_status(pbi->fence_vf_s.fence_vf[i]->fence);
						if (ret == 1) {
							signed_fence[signed_count] = pbi->fence_vf_s.fence_vf[i];
							pbi->fence_vf_s.fence_vf[i] = NULL;
							pbi->fence_vf_s.used_size--;
							signed_count++;
						}
						j++;
					}
				}
			}
			mutex_unlock(&pbi->fence_mutex);
			for (i = 0; i < signed_count; i++) {
				if (!signed_fence[i])
					continue;
				buf = (struct aml_buf *)signed_fence[i]->v4l_mem_handle;
				vp9_recycle_dec_resource(pbi, buf);
			}
		} else {
			prepare_display_buf(pbi, &sd);
		}

		pbi->pre_stream_offset = READ_VREG(HEVC_SHIFT_BYTE_COUNT);
	}

	return 0;
}

/**************************************************

VP9 buffer management end

***************************************************/

#define HEVC_CM_BODY_START_ADDR                    0x3626
#define HEVC_CM_BODY_LENGTH                        0x3627
#define HEVC_CM_HEADER_LENGTH                      0x3629
#define HEVC_CM_HEADER_OFFSET                      0x362b

#define LOSLESS_COMPRESS_MODE

#ifdef VP9_10B_NV21
static u32 mem_map_mode = 2  /* 0:linear 1:32x32 2:64x32*/
#else
static u32 mem_map_mode; /* 0:linear 1:32x32 2:64x32 ; m8baby test1902 */
#endif
static u32 enable_mem_saving = 1;
static u32 force_w_h;

static u32 force_fps;


const u32 vp9_version = 201602101;
static u32 debug;
static u32 radr;
static u32 rval;
static u32 pop_shorts;
static u32 dbg_cmd;
static u32 dbg_skip_decode_index;

/*
 * bit 0~3, for HEVCD_IPP_AXIIF_CONFIG endian config
 * bit 8~23, for HEVC_SAO_CTRL1 endian config
 */
static u32 endian;
#define HEVC_CONFIG_BIG_ENDIAN     ((0x880 << 8) | 0x8)
#define HEVC_CONFIG_LITTLE_ENDIAN  ((0xff0 << 8) | 0xf)

#ifdef ERROR_HANDLE_DEBUG
static u32 dbg_nal_skip_flag;
		/* bit[0], skip vps; bit[1], skip sps; bit[2], skip pps */
static u32 dbg_nal_skip_count;
#endif
/*for debug*/
static u32 decode_pic_begin;
static uint slice_parse_begin;
static u32 step;
#ifdef MIX_STREAM_SUPPORT
static u32 buf_alloc_width = 4096;
static u32 buf_alloc_height = 2304;
static u32 vp9_max_pic_w = 4096;
static u32 vp9_max_pic_h = 2304;

static u32 dynamic_buf_num_margin;
#else
static u32 buf_alloc_width;
static u32 buf_alloc_height;
static u32 dynamic_buf_num_margin = 7;
#endif
static u32 buf_alloc_depth = 10;
static u32 buf_alloc_size;
/*
 *bit[0]: 0,
 *    bit[1]: 0, always release cma buffer when stop
 *    bit[1]: 1, never release cma buffer when stop
 *bit[0]: 1, when stop, release cma buffer if blackout is 1;
 *do not release cma buffer is blackout is not 1
 *
 *bit[2]: 0, when start decoding, check current displayed buffer
 *	 (only for buffer decoded by vp9) if blackout is 0
 *	 1, do not check current displayed buffer
 *
 *bit[3]: 1, if blackout is not 1, do not release current
 *			displayed cma buffer always.
 */
/* set to 1 for fast play;
 *	set to 8 for other case of "keep last frame"
 */
static u32 buffer_mode = 1;
/* buffer_mode_dbg: debug only*/
static u32 buffer_mode_dbg = 0xffff0000;
/**/

/*
 *bit 0, 1: only display I picture;
 *bit 1, 1: only decode I picture;
 */
static u32 i_only_flag;

static u32 low_latency_flag;

static u32 no_head;

static u32 max_decoding_time;
/*
 *error handling
 */
/*error_handle_policy:
 *bit 0: 0, auto skip error_skip_nal_count nals before error recovery;
 *1, skip error_skip_nal_count nals before error recovery;
 *bit 1 (valid only when bit0 == 1):
 *1, wait vps/sps/pps after error recovery;
 *bit 2 (valid only when bit0 == 0):
 *0, auto search after error recovery (vp9_recover() called);
 *1, manual search after error recovery
 *(change to auto search after get IDR: WRITE_VREG(NAL_SEARCH_CTL, 0x2))
 *
 *bit 4: 0, set error_mark after reset/recover
 *    1, do not set error_mark after reset/recover
 *bit 5: 0, check total lcu for every picture
 *    1, do not check total lcu
 *
 */

static u32 error_handle_policy;
#define MAX_BUF_NUM_NORMAL     15
#define MAX_BUF_NUM_LESS   10
static u32 max_buf_num = MAX_BUF_NUM_NORMAL;
#define MAX_BUF_NUM_SAVE_BUF  8

static u32 run_ready_min_buf_num = 2;

static DEFINE_MUTEX(vvp9_mutex);
#ifndef MULTI_INSTANCE_SUPPORT
static struct device *cma_dev;
#endif

#define HEVC_DEC_STATUS_REG       HEVC_ASSIST_SCRATCH_0
#define HEVC_RPM_BUFFER           HEVC_ASSIST_SCRATCH_1
#define HEVC_SHORT_TERM_RPS       HEVC_ASSIST_SCRATCH_2
#define VP9_ADAPT_PROB_REG        HEVC_ASSIST_SCRATCH_3
#define VP9_MMU_MAP_BUFFER        HEVC_ASSIST_SCRATCH_4
#define HEVC_PPS_BUFFER           HEVC_ASSIST_SCRATCH_5
//#define HEVC_SAO_UP               HEVC_ASSIST_SCRATCH_6
#define HEVC_STREAM_SWAP_BUFFER   HEVC_ASSIST_SCRATCH_7
#define HEVC_STREAM_SWAP_BUFFER2  HEVC_ASSIST_SCRATCH_8
#define VP9_PROB_SWAP_BUFFER      HEVC_ASSIST_SCRATCH_9
#define VP9_COUNT_SWAP_BUFFER     HEVC_ASSIST_SCRATCH_A
#define VP9_SEG_MAP_BUFFER        HEVC_ASSIST_SCRATCH_B
//#define HEVC_SCALELUT             HEVC_ASSIST_SCRATCH_D
#define HEVC_WAIT_FLAG            HEVC_ASSIST_SCRATCH_E
#define RPM_CMD_REG               HEVC_ASSIST_SCRATCH_F
#define LMEM_DUMP_ADR                 HEVC_ASSIST_SCRATCH_F
#define HEVC_STREAM_SWAP_TEST     HEVC_ASSIST_SCRATCH_L
#ifdef MULTI_INSTANCE_SUPPORT
#define HEVC_DECODE_COUNT       HEVC_ASSIST_SCRATCH_M
#define HEVC_DECODE_SIZE		HEVC_ASSIST_SCRATCH_N
#else
#define HEVC_DECODE_PIC_BEGIN_REG HEVC_ASSIST_SCRATCH_M
#define HEVC_DECODE_PIC_NUM_REG   HEVC_ASSIST_SCRATCH_N
#endif
#define DEBUG_REG1              HEVC_ASSIST_SCRATCH_G
#define DEBUG_REG2              HEVC_ASSIST_SCRATCH_H


/*
 *ucode parser/search control
 *bit 0:  0, header auto parse; 1, header manual parse
 *bit 1:  0, auto skip for noneseamless stream; 1, no skip
 *bit [3:2]: valid when bit1==0;
 *0, auto skip nal before first vps/sps/pps/idr;
 *1, auto skip nal before first vps/sps/pps
 *2, auto skip nal before first  vps/sps/pps,
 *	and not decode until the first I slice (with slice address of 0)
 *
 *3, auto skip before first I slice (nal_type >=16 && nal_type<=21)
 *bit [15:4] nal skip count (valid when bit0 == 1 (manual mode) )
 *bit [16]: for NAL_UNIT_EOS when bit0 is 0:
 *	0, send SEARCH_DONE to arm ;  1, do not send SEARCH_DONE to arm
 *bit [17]: for NAL_SEI when bit0 is 0:
 *	0, do not parse SEI in ucode; 1, parse SEI in ucode
 *bit [31:20]: used by ucode for debug purpose
 */
#define NAL_SEARCH_CTL            HEVC_ASSIST_SCRATCH_I
	/*[31:24] chip feature
		31: 0, use MBOX1; 1, use MBOX0
	*/
#define DECODE_MODE              HEVC_ASSIST_SCRATCH_J
#define DECODE_STOP_POS         HEVC_ASSIST_SCRATCH_K

#ifdef MULTI_INSTANCE_SUPPORT
#define RPM_BUF_SIZE (0x400 * 2)
#else
#define RPM_BUF_SIZE (0x80*2)
#endif
#define LMEM_BUF_SIZE (0x600 * 2)

/*mmu_vbh buf is used by HEVC_SAO_MMU_VH0_ADDR, HEVC_SAO_MMU_VH1_ADDR*/
#define VBH_BUF_SIZE_1080P 0x3000
#define VBH_BUF_SIZE_4K 0x5000
#define VBH_BUF_SIZE_8K 0xa000
#define VBH_BUF_SIZE(bufspec) (bufspec->mmu_vbh.buf_size / 2)
/*mmu_vbh_dw buf is used by HEVC_SAO_MMU_VH0_ADDR2,HEVC_SAO_MMU_VH1_ADDR2,
	HEVC_DW_VH0_ADDDR, HEVC_DW_VH1_ADDDR*/
#define DW_VBH_BUF_SIZE_1080P (VBH_BUF_SIZE_1080P * 2)
#define DW_VBH_BUF_SIZE_4K (VBH_BUF_SIZE_4K * 2)
#define DW_VBH_BUF_SIZE_8K (VBH_BUF_SIZE_8K * 2)
#define DW_VBH_BUF_SIZE(bufspec) (bufspec->mmu_vbh_dw.buf_size / 4)

/* necessary 4K page size align for t7/t3 decoder and after */
#define WORKBUF_ALIGN(addr) (ALIGN(addr, PAGE_SIZE))

#define WORK_BUF_SPEC_NUM 6
static struct BuffInfo_s amvvp9_workbuff_spec[WORK_BUF_SPEC_NUM] = {
	{
		/* 8M bytes */
		.max_width = 1920,
		.max_height = 1088,
		.ipp = {
			/* IPP work space calculation :
			 *   4096 * (Y+CbCr+Flags) = 12k, round to 16k
			 */
			.buf_size = 0x4000,
		},
		.sao_abv = {
			.buf_size = 0x30000,
		},
		.sao_vb = {
			.buf_size = 0x30000,
		},
		.short_term_rps = {
			/* SHORT_TERM_RPS - Max 64 set, 16 entry every set,
			 *   total 64x16x2 = 2048 bytes (0x800)
			 */
			.buf_size = 0x800,
		},
		.vps = {
			/* VPS STORE AREA - Max 16 VPS, each has 0x80 bytes,
			 *   total 0x0800 bytes
			 */
			.buf_size = 0x800,
		},
		.sps = {
			/* SPS STORE AREA - Max 16 SPS, each has 0x80 bytes,
			 *   total 0x0800 bytes
			 */
			.buf_size = 0x800,
		},
		.pps = {
			/* PPS STORE AREA - Max 64 PPS, each has 0x80 bytes,
			 *   total 0x2000 bytes
			 */
			.buf_size = 0x2000,
		},
		.sao_up = {
			/* SAO UP STORE AREA - Max 640(10240/16) LCU,
			 *   each has 16 bytes total 0x2800 bytes
			 */
			.buf_size = 0x2800,
		},
		.swap_buf = {
			/* 256cyclex64bit = 2K bytes 0x800
			 *   (only 144 cycles valid)
			 */
			.buf_size = 0x800,
		},
		.swap_buf2 = {
			.buf_size = 0x800,
		},
		.scalelut = {
			/* support up to 32 SCALELUT 1024x32 =
			 *   32Kbytes (0x8000)
			 */
			.buf_size = 0x8000,
		},
		.dblk_para = {
			/* DBLK -> Max 256(4096/16) LCU,
			 *each para 1024bytes(total:0x40000),
			 *data 1024bytes(total:0x40000)
			 */
			.buf_size = 0x80000,
		},
		.dblk_data = {
			.buf_size = 0x80000,
		},
		.seg_map = {
		/*4096x2304/64/64 *24 = 0xd800 Bytes*/
			.buf_size = 0xd800,
		},
		.mmu_vbh = {
			.buf_size = 0x5000, /*2*16*(more than 2304)/4, 4K*/
		},
		.mpred_above = {
			.buf_size = 0x10000, /* 2 * size of hevc*/
		},
#ifdef MV_USE_FIXED_BUF
		.mpred_mv = {/* 1080p, 0x40000 per buffer */
			.buf_size = 0x40000 * FRAME_BUFFERS,
		},
#endif
		.rpm = {
			.buf_size = RPM_BUF_SIZE,
		},
		.lmem = {
			.buf_size = 0x400 * 2,
		},
		.prob_buf = {
			.buf_size = PROB_BUF_SIZE,
		},
		.prob_cnt_buf = {
			.buf_size = COUNT_BUF_SIZE,
		},
	},
	{
		.max_width = 4096,
		.max_height = 2304,
		.ipp = {
			/* IPP work space calculation :
			 *   4096 * (Y+CbCr+Flags) = 12k, round to 16k
			 */
			.buf_size = 0x4000,
		},
		.sao_abv = {
			.buf_size = 0x30000,
		},
		.sao_vb = {
			.buf_size = 0x30000,
		},
		.short_term_rps = {
			/* SHORT_TERM_RPS - Max 64 set, 16 entry every set,
			 *   total 64x16x2 = 2048 bytes (0x800)
			 */
			.buf_size = 0x800,
		},
		.vps = {
			/* VPS STORE AREA - Max 16 VPS, each has 0x80 bytes,
			 *   total 0x0800 bytes
			 */
			.buf_size = 0x800,
		},
		.sps = {
			/* SPS STORE AREA - Max 16 SPS, each has 0x80 bytes,
			 *   total 0x0800 bytes
			 */
			.buf_size = 0x800,
		},
		.pps = {
			/* PPS STORE AREA - Max 64 PPS, each has 0x80 bytes,
			 *   total 0x2000 bytes
			 */
			.buf_size = 0x2000,
		},
		.sao_up = {
			/* SAO UP STORE AREA - Max 640(10240/16) LCU,
			 *   each has 16 bytes total 0x2800 bytes
			 */
			.buf_size = 0x2800,
		},
		.swap_buf = {
			/* 256cyclex64bit = 2K bytes 0x800
			 *   (only 144 cycles valid)
			 */
			.buf_size = 0x800,
		},
		.swap_buf2 = {
			.buf_size = 0x800,
		},
		.scalelut = {
			/* support up to 32 SCALELUT 1024x32 = 32Kbytes
			 *   (0x8000)
			 */
			.buf_size = 0x8000,
		},
		.dblk_para = {
			/* DBLK -> Max 256(4096/16) LCU,
			 *each para 1024bytes(total:0x40000),
			 *data 1024bytes(total:0x40000)
			 */
			.buf_size = 0x80000,
		},
		.dblk_data = {
			.buf_size = 0x80000,
		},
		.seg_map = {
			/*4096x2304/64/64 *24 = 0xd800 Bytes*/
			.buf_size = 0xd800,
		},
		.mmu_vbh = {
			.buf_size = 0x5000,/*2*16*(more than 2304)/4, 4K*/
		},
		.mpred_above = {
			.buf_size = 0x10000, /* 2 * size of hevc*/
		},
#ifdef MV_USE_FIXED_BUF
		.mpred_mv = {
			/* .buf_size = 0x100000*16,
			 * //4k2k , 0x100000 per buffer
			 */
			/* 4096x2304 , 0x120000 per buffer */
			.buf_size = 0x120000 * FRAME_BUFFERS,
		},
#endif
		.rpm = {
			.buf_size = RPM_BUF_SIZE,
		},
		.lmem = {
			.buf_size = 0x400 * 2,
		},
		.prob_buf = {
			.buf_size = PROB_BUF_SIZE,
		},
		.prob_cnt_buf = {
			.buf_size = COUNT_BUF_SIZE,
		},
	},
	{
		.max_width = 4096*2,
		.max_height = 2304*2,
		.ipp = {
			// IPP work space calculation : 4096 * (Y+CbCr+Flags) = 12k, round to 16k
			.buf_size = 0x4000*2,
		},
		.sao_abv = {
			.buf_size = 0x30000*2,
		},
		.sao_vb = {
			.buf_size = 0x30000*2,
		},
		.short_term_rps = {
			// SHORT_TERM_RPS - Max 64 set, 16 entry every set, total 64x16x2 = 2048 bytes (0x800)
			.buf_size = 0x800,
		},
		.vps = {
			// VPS STORE AREA - Max 16 VPS, each has 0x80 bytes, total 0x0800 bytes
			.buf_size = 0x800,
		},
		.sps = {
			// SPS STORE AREA - Max 16 SPS, each has 0x80 bytes, total 0x0800 bytes
			.buf_size = 0x800,
		},
		.pps = {
			// PPS STORE AREA - Max 64 PPS, each has 0x80 bytes, total 0x2000 bytes
			.buf_size = 0x2000,
		},
		.sao_up = {
			// SAO UP STORE AREA - Max 640(10240/16) LCU, each has 16 bytes total 0x2800 bytes
			.buf_size = 0x2800*2,
		},
		.swap_buf = {
			// 256cyclex64bit = 2K bytes 0x800 (only 144 cycles valid)
			.buf_size = 0x800,
		},
		.swap_buf2 = {
			.buf_size = 0x800,
		},
		.scalelut = {
			// support up to 32 SCALELUT 1024x32 = 32Kbytes (0x8000)
			.buf_size = 0x8000*2,
		},
		.dblk_para = {
			// DBLK -> Max 256(4096/16) LCU, each para 1024bytes(total:0x40000), data 1024bytes(total:0x40000)
			.buf_size = 0x80000*2,
		},
		.dblk_data = {
			.buf_size = 0x80000*2,
		},
		.seg_map = {
			/*4096x2304/64/64 *24 = 0xd800 Bytes*/
			.buf_size = 0xd800*4,
		},
		.mmu_vbh = {
			.buf_size = 0x5000*2, //2*16*(more than 2304)/4, 4K
		},
		.mpred_above = {
			.buf_size = 0x10000*2, /* 2 * size of hevc*/
		},
#ifdef MV_USE_FIXED_BUF
		.mpred_mv = {
			//4k2k , 0x100000 per buffer */
			/* 4096x2304 , 0x120000 per buffer */
			.buf_size = 0x120000 * FRAME_BUFFERS * 4,
		},
#endif
		.rpm = {
			.buf_size = RPM_BUF_SIZE,
		},
		.lmem = {
			.buf_size = 0x400 * 2,
		},
		.prob_buf = {
			.buf_size = PROB_BUF_SIZE,
		},
		.prob_cnt_buf = {
			.buf_size = COUNT_BUF_SIZE,
		},
	},
	{
		/* 8M bytes */
		.max_width		= 1920, //2048
		.max_height		= 1088, //1152
		.ipp			= {.buf_size = 0x1e00}, //IPP work space calculation :  4096 * (Y+CbCr+Flags) = 12k, round to 16k
		.sao_abv		= {.buf_size = 0},
		.sao_vb			= {.buf_size = 0},
		.short_term_rps		= {.buf_size = 0x800}, //SHORT_TERM_RPS - Max 64 set, 16 entry every set, total 64x16x2 = 2048 bytes (0x800)
		.vps			= {.buf_size = 0x800}, //VPS STORE AREA - Max 16 VPS, each has 0x80 bytes, total 0x0800 bytes
		.sps			= {.buf_size = 0x800}, //SPS STORE AREA - Max 16 SPS, each has 0x80 bytes, total 0x0800 bytes
		.pps			= {.buf_size = 0x2000}, //PPS STORE AREA - Max 64 PPS, each has 0x80 bytes, total 0x2000 bytes
		.sao_up			= {.buf_size = 0}, //SAO UP STORE AREA - Max 640(10240/16) LCU, each has 16 bytes total 0x2800 bytes
		.swap_buf		= {.buf_size = 0x800}, //256cyclex64bit = 2K bytes 0x800 (only 144 cycles valid)
		.swap_buf2		= {.buf_size = 0x800},
		.scalelut		= {.buf_size = 0}, //support up to 32 SCALELUT 1024x32 = 32Kbytes (0x8000)
		.dblk_para		= {.buf_size = 0x50800}, //addr_offset_lft(64)*4096 + (ctu_numb_y+1)*2048 => 64*4096+(2048/64+1)*2048 = 0x50800
		.dblk_data		= {.buf_size = 0x50800}, //addr_offset_lft(64)*4096 + (ctu_numb_y+1)*2048 => 64*4096+(2048/64+1)*2048 = 0x50800
		.seg_map		= {.buf_size = 0x3600}, //2048x1152/64/64 *24 = 0x3600 Bytes
		.mmu_vbh		= {.buf_size = VBH_BUF_SIZE_1080P}, //2*16*(more than 2304)/4, 4K
		.mpred_above 		= {.buf_size = 0x2400}, //(pic_width/64)*18*16 byte => 2048/64*18*16 = 0x2400
#ifdef MV_USE_FIXED_BUF
		.mpred_mv		= {.buf_size = 0x48200 * FRAME_BUFFERS},// 1080p, 0x40000 per buffer
#endif
		.rpm			= {.buf_size = RPM_BUF_SIZE},
		.lmem			= {.buf_size = 0x400 * 2},
		.prob_buf		= {.buf_size = PROB_BUF_SIZE,},
		.prob_cnt_buf	= {.buf_size = COUNT_BUF_SIZE,},
	},
	{
		.max_width		= 4096,
		.max_height		= 2304,
		.ipp			= {.buf_size = 0x4000}, //IPP work space calculation : 4096 * (Y+CbCr+Flags) = 12k, round to 16k
		.sao_abv		= {.buf_size = 0},
		.sao_vb			= {.buf_size = 0},
		.short_term_rps		= {.buf_size = 0x800}, // SHORT_TERM_RPS - Max 64 set, 16 entry every set, total 64x16x2 = 2048 bytes (0x800)
		.vps			= {.buf_size = 0x800}, //VPS STORE AREA - Max 16 VPS, each has 0x80 bytes, total 0x0800 bytes
		.sps			= {.buf_size = 0x800}, //SPS STORE AREA - Max 16 SPS, each has 0x80 bytes, total 0x0800 bytes
		.pps			= {.buf_size = 0x2000}, //PPS STORE AREA - Max 64 PPS, each has 0x80 bytes, total 0x2000 bytes
		.sao_up			= {.buf_size = 0}, //SAO UP STORE AREA - Max 640(10240/16) LCU, each has 16 bytes total 0x2800 bytes
		.swap_buf		= {.buf_size = 0x800}, //256cyclex64bit = 2K bytes 0x800 (only 144 cycles valid)
		.swap_buf2		= {.buf_size = 0x800},
		.scalelut		= {.buf_size = 0}, //support up to 32 SCALELUT 1024x32 = 32Kbytes = (0x8000)
		.dblk_para		= {.buf_size = 0x60800}, //addr_offset_lft(64)*4096 + (ctu_numb_y+1)*2048 => 64*4096+(4096/64+1)*2048 = 0x60800
		.dblk_data		= {.buf_size = 0x60800}, //addr_offset_lft(64)*4096 + (ctu_numb_y+1)*2048 => 64*4096+(4096/64+1)*2048 = 0x60800
		.seg_map		= {.buf_size = 0xd800}, //4096x2304/64/64 *24bytes = 0xd800 Bytes
		.mmu_vbh		= {.buf_size = VBH_BUF_SIZE_4K}, //2*16*(more than 2304)/4, 4K
		.mpred_above		= {.buf_size = 0x4800}, // (pic_width/64)*18*16 byte => 4096/64*18*16 = 0x4800
#ifdef MV_USE_FIXED_BUF
		.mpred_mv		= {.buf_size = 0x145400 * FRAME_BUFFERS}, //buf_size = 0x100000*16, 4k2k => 0x100000 per buffer 4096x2304 =>0x120000 per buffer
#endif
		.rpm			= {.buf_size = RPM_BUF_SIZE},
		.lmem			= {.buf_size = 0x400 * 2},
		.prob_buf		= {.buf_size = PROB_BUF_SIZE,},
		.prob_cnt_buf	= {.buf_size = COUNT_BUF_SIZE,},
	},
	{
		.max_width		= 4096*2,
		.max_height		= 2304*2,
		.ipp			= {.buf_size = 0x4000*2}, // IPP work space calculation : 4096 * (Y+CbCr+Flags) = 12k, round to 16k
		.sao_abv		= {.buf_size = 0},
		.sao_vb			= {.buf_size = 0},
		.short_term_rps		= {.buf_size = 0x800}, // SHORT_TERM_RPS - Max 64 set, 16 entry every set, total 64x16x2 = 2048 bytes (0x800)
		.vps			= {.buf_size = 0x800}, // VPS STORE AREA - Max 16 VPS, each has 0x80 bytes, total 0x0800 bytes
		.sps			= {.buf_size = 0x800}, // SPS STORE AREA - Max 16 SPS, each has 0x80 bytes, total 0x0800 bytes
		.pps			= {.buf_size = 0x2000}, // PPS STORE AREA - Max 64 PPS, each has 0x80 bytes, total 0x2000 bytes
		.sao_up			= {.buf_size = 0}, // SAO UP STORE AREA - Max 640(10240/16) LCU, each has 16 bytes total 0x2800 bytes
		.swap_buf		= {.buf_size = 0x800}, // 256cyclex64bit = 2K bytes 0x800 (only 144 cycles valid)
		.swap_buf2		= {.buf_size = 0x800},
		.scalelut		= {.buf_size = 0}, // support up to 32 SCALELUT 1024x32 = 32Kbytes (0x8000)
		.dblk_para		= {.buf_size = 0xa4800}, //addr_offset_lft(64)*4096 + (ctu_numb_y+1)*2048 => 64*4096+(8192/64+1)*2048 = 0x80800
		.dblk_data		= {.buf_size = 0xa4800}, //addr_offset_lft(64)*4096 + (ctu_numb_y+1)*2048 => 64*4096+(8192/64+1)*2048 = 0x80800
		.seg_map		= {.buf_size = 0x36000}, //8192*4608/64/64*24 = 0x36000 Bytes
		.mmu_vbh		= {.buf_size = VBH_BUF_SIZE_8K}, //2*16*(more than 2304)/4, 4K
		.mpred_above		= {.buf_size = 0x9000,}, // (pic_width/64)*18*16 byte => 8192/64*18*16 = 0x9000
#ifdef MV_USE_FIXED_BUF
		.mpred_mv		= {.buf_size = 0x514800 * FRAME_BUFFERS}, //4k2k => 0x100000 per buffer 4096x2304 => 0x120000 per buffer */
#endif
		.rpm			= {.buf_size = RPM_BUF_SIZE},
		.lmem			= {.buf_size = 0x400 * 2},
		.prob_buf		= {.buf_size = PROB_BUF_SIZE,},
		.prob_cnt_buf	= {.buf_size = COUNT_BUF_SIZE,},
	}
};


/*Losless compression body buffer size 4K per 64x32 (jt)*/
int  compute_losless_comp_body_size(int width, int height,
				bool is_bit_depth_10)
{
	int     width_x64;
	int     height_x32;
	int     bsize;

	width_x64 = width + 63;
	width_x64 >>= 6;
	height_x32 = height + 31;
	height_x32 >>= 5;
	bsize = (is_bit_depth_10?4096:3200)*width_x64*height_x32;
	if (debug & VP9_DEBUG_BUFMGR_MORE)
		pr_info("%s(%d,%d,%d)=>%d\n",
			__func__, width, height,
			is_bit_depth_10, bsize);

	return  bsize;
}

/* Losless compression header buffer size 32bytes per 128x64 (jt)*/
static  int  compute_losless_comp_header_size(int width, int height)
{
	int     width_x128;
	int     height_x64;
	int     hsize;

	width_x128 = width + 127;
	width_x128 >>= 7;
	height_x64 = height + 63;
	height_x64 >>= 6;

	hsize = 32 * width_x128 * height_x64;
	if (debug & VP9_DEBUG_BUFMGR_MORE)
		pr_info("%s(%d,%d)=>%d\n",
			__func__, width, height,
			hsize);

	return  hsize;
}

static void init_buff_spec(struct VP9Decoder_s *pbi,
	struct BuffInfo_s *buf_spec)
{
	void *mem_start_virt;

	buf_spec->ipp.buf_start =
		WORKBUF_ALIGN(buf_spec->start_adr);
	buf_spec->sao_abv.buf_start =
		WORKBUF_ALIGN(buf_spec->ipp.buf_start + buf_spec->ipp.buf_size);
	buf_spec->sao_vb.buf_start =
		WORKBUF_ALIGN(buf_spec->sao_abv.buf_start + buf_spec->sao_abv.buf_size);
	buf_spec->short_term_rps.buf_start =
		WORKBUF_ALIGN(buf_spec->sao_vb.buf_start + buf_spec->sao_vb.buf_size);
	buf_spec->vps.buf_start =
		WORKBUF_ALIGN(buf_spec->short_term_rps.buf_start + buf_spec->short_term_rps.buf_size);
	buf_spec->sps.buf_start =
		WORKBUF_ALIGN(buf_spec->vps.buf_start + buf_spec->vps.buf_size);
	buf_spec->pps.buf_start =
		WORKBUF_ALIGN(buf_spec->sps.buf_start + buf_spec->sps.buf_size);
	buf_spec->sao_up.buf_start =
		WORKBUF_ALIGN(buf_spec->pps.buf_start + buf_spec->pps.buf_size);
	buf_spec->swap_buf.buf_start =
		WORKBUF_ALIGN(buf_spec->sao_up.buf_start + buf_spec->sao_up.buf_size);
	buf_spec->swap_buf2.buf_start =
		WORKBUF_ALIGN(buf_spec->swap_buf.buf_start + buf_spec->swap_buf.buf_size);
	buf_spec->scalelut.buf_start =
		WORKBUF_ALIGN(buf_spec->swap_buf2.buf_start + buf_spec->swap_buf2.buf_size);
	buf_spec->dblk_para.buf_start =
		WORKBUF_ALIGN(buf_spec->scalelut.buf_start + buf_spec->scalelut.buf_size);
	buf_spec->dblk_data.buf_start =
		WORKBUF_ALIGN(buf_spec->dblk_para.buf_start + buf_spec->dblk_para.buf_size);
	buf_spec->seg_map.buf_start =
		WORKBUF_ALIGN(buf_spec->dblk_data.buf_start + buf_spec->dblk_data.buf_size);
	if (pbi == NULL || pbi->mmu_enable) {
		buf_spec->mmu_vbh.buf_start  =
			WORKBUF_ALIGN(buf_spec->seg_map.buf_start + buf_spec->seg_map.buf_size);
		buf_spec->mpred_above.buf_start =
			WORKBUF_ALIGN(buf_spec->mmu_vbh.buf_start + buf_spec->mmu_vbh.buf_size);
	} else {
		buf_spec->mpred_above.buf_start =
			WORKBUF_ALIGN(buf_spec->seg_map.buf_start + buf_spec->seg_map.buf_size);
	}
#ifdef MV_USE_FIXED_BUF
	buf_spec->mpred_mv.buf_start =
		WORKBUF_ALIGN(buf_spec->mpred_above.buf_start + buf_spec->mpred_above.buf_size);
	buf_spec->rpm.buf_start =
		WORKBUF_ALIGN(buf_spec->mpred_mv.buf_start + buf_spec->mpred_mv.buf_size);
#else
	buf_spec->rpm.buf_start =
		WORKBUF_ALIGN(buf_spec->mpred_above.buf_start + buf_spec->mpred_above.buf_size);
#endif
	buf_spec->lmem.buf_start =
		WORKBUF_ALIGN(buf_spec->rpm.buf_start + buf_spec->rpm.buf_size);
	buf_spec->prob_buf.buf_start =
		WORKBUF_ALIGN(buf_spec->lmem.buf_start + buf_spec->lmem.buf_size);
	buf_spec->prob_cnt_buf.buf_start =
		WORKBUF_ALIGN(buf_spec->prob_buf.buf_start + buf_spec->prob_buf.buf_size);
	buf_spec->end_adr =
		WORKBUF_ALIGN(buf_spec->prob_cnt_buf.buf_start + buf_spec->prob_cnt_buf.buf_size);

	if (!pbi)
		return;

	if (!vdec_secure(hw_to_vdec(pbi))) {
		mem_start_virt =
			codec_mm_phys_to_virt(buf_spec->dblk_para.buf_start);
		if (mem_start_virt) {
			memset(mem_start_virt, 0,
				buf_spec->dblk_para.buf_size);
			codec_mm_dma_flush(mem_start_virt,
				buf_spec->dblk_para.buf_size,
				DMA_TO_DEVICE);
		} else {
			mem_start_virt = codec_mm_vmap(
				buf_spec->dblk_para.buf_start,
				buf_spec->dblk_para.buf_size);
			if (mem_start_virt) {
				memset(mem_start_virt, 0,
					buf_spec->dblk_para.buf_size);
				codec_mm_dma_flush(mem_start_virt,
					buf_spec->dblk_para.buf_size,
					DMA_TO_DEVICE);
				codec_mm_unmap_phyaddr(mem_start_virt);
			} else {
				/*not virt for tvp playing,
				may need clear on ucode.*/
				pr_err("mem_start_virt failed\n");
			}
		}
	}

	if (debug) {
		pr_info("%s workspace (%x %x) size = %x\n", __func__,
			   buf_spec->start_adr, buf_spec->end_adr,
			   buf_spec->end_adr - buf_spec->start_adr);
	}

	if (debug) {
		pr_info("ipp.buf_start             :%x\n",
			   buf_spec->ipp.buf_start);
		pr_info("sao_abv.buf_start          :%x\n",
			   buf_spec->sao_abv.buf_start);
		pr_info("sao_vb.buf_start          :%x\n",
			   buf_spec->sao_vb.buf_start);
		pr_info("short_term_rps.buf_start  :%x\n",
			   buf_spec->short_term_rps.buf_start);
		pr_info("vps.buf_start             :%x\n",
			   buf_spec->vps.buf_start);
		pr_info("sps.buf_start             :%x\n",
			   buf_spec->sps.buf_start);
		pr_info("pps.buf_start             :%x\n",
			   buf_spec->pps.buf_start);
		pr_info("sao_up.buf_start          :%x\n",
			   buf_spec->sao_up.buf_start);
		pr_info("swap_buf.buf_start        :%x\n",
			   buf_spec->swap_buf.buf_start);
		pr_info("swap_buf2.buf_start       :%x\n",
			   buf_spec->swap_buf2.buf_start);
		pr_info("scalelut.buf_start        :%x\n",
			   buf_spec->scalelut.buf_start);
		pr_info("dblk_para.buf_start       :%x\n",
			   buf_spec->dblk_para.buf_start);
		pr_info("dblk_data.buf_start       :%x\n",
			   buf_spec->dblk_data.buf_start);
		pr_info("seg_map.buf_start       :%x\n",
			buf_spec->seg_map.buf_start);
	if (pbi->mmu_enable) {
		pr_info("mmu_vbh.buf_start     :%x\n",
			buf_spec->mmu_vbh.buf_start);
	}
		pr_info("mpred_above.buf_start     :%x\n",
			   buf_spec->mpred_above.buf_start);
#ifdef MV_USE_FIXED_BUF
		pr_info("mpred_mv.buf_start        :%x\n",
			   buf_spec->mpred_mv.buf_start);
#endif
		if ((debug & VP9_DEBUG_SEND_PARAM_WITH_REG) == 0) {
			pr_info("rpm.buf_start             :%x\n",
				   buf_spec->rpm.buf_start);
		}
		pr_info("prob_buf.buf_start        :%x\n",
			   buf_spec->prob_buf.buf_start);
		pr_info("prob_cnt_buf.buf_start    :%x\n",
			   buf_spec->prob_cnt_buf.buf_start);
	}
}

/* cache_util.c */
#define   THODIYIL_MCRCC_CANVAS_ALGX    4

static u32 mcrcc_cache_alg_flag = THODIYIL_MCRCC_CANVAS_ALGX;

static void mcrcc_perfcount_reset(void)
{
	if (debug & VP9_DEBUG_CACHE)
		pr_info("[cache_util.c] Entered mcrcc_perfcount_reset...\n");
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)0x1);
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)0x0);
	return;
}

static unsigned raw_mcr_cnt_total_prev;
static unsigned hit_mcr_0_cnt_total_prev;
static unsigned hit_mcr_1_cnt_total_prev;
static unsigned byp_mcr_cnt_nchcanv_total_prev;
static unsigned byp_mcr_cnt_nchoutwin_total_prev;

static void mcrcc_get_hitrate(unsigned reset_pre)
{
	unsigned delta_hit_mcr_0_cnt;
	unsigned delta_hit_mcr_1_cnt;
	unsigned delta_raw_mcr_cnt;
	unsigned delta_mcr_cnt_nchcanv;
	unsigned delta_mcr_cnt_nchoutwin;

	unsigned tmp;
	unsigned raw_mcr_cnt;
	unsigned hit_mcr_cnt;
	unsigned byp_mcr_cnt_nchoutwin;
	unsigned byp_mcr_cnt_nchcanv;
	int hitrate;
	if (reset_pre) {
		raw_mcr_cnt_total_prev = 0;
		hit_mcr_0_cnt_total_prev = 0;
		hit_mcr_1_cnt_total_prev = 0;
		byp_mcr_cnt_nchcanv_total_prev = 0;
		byp_mcr_cnt_nchoutwin_total_prev = 0;
	}
	if (debug & VP9_DEBUG_CACHE)
		pr_info("[cache_util.c] Entered mcrcc_get_hitrate...\n");
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)(0x0<<1));
	raw_mcr_cnt = READ_VREG(HEVCD_MCRCC_PERFMON_DATA);
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)(0x1<<1));
	hit_mcr_cnt = READ_VREG(HEVCD_MCRCC_PERFMON_DATA);
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)(0x2<<1));
	byp_mcr_cnt_nchoutwin = READ_VREG(HEVCD_MCRCC_PERFMON_DATA);
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)(0x3<<1));
	byp_mcr_cnt_nchcanv = READ_VREG(HEVCD_MCRCC_PERFMON_DATA);

	if (debug & VP9_DEBUG_CACHE)
		pr_info("raw_mcr_cnt_total: %d\n",
			raw_mcr_cnt);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("hit_mcr_cnt_total: %d\n",
			hit_mcr_cnt);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("byp_mcr_cnt_nchoutwin_total: %d\n",
			byp_mcr_cnt_nchoutwin);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("byp_mcr_cnt_nchcanv_total: %d\n",
			byp_mcr_cnt_nchcanv);

	delta_raw_mcr_cnt = raw_mcr_cnt -
		raw_mcr_cnt_total_prev;
	delta_mcr_cnt_nchcanv = byp_mcr_cnt_nchcanv -
		byp_mcr_cnt_nchcanv_total_prev;
	delta_mcr_cnt_nchoutwin = byp_mcr_cnt_nchoutwin -
		byp_mcr_cnt_nchoutwin_total_prev;
	raw_mcr_cnt_total_prev = raw_mcr_cnt;
	byp_mcr_cnt_nchcanv_total_prev = byp_mcr_cnt_nchcanv;
	byp_mcr_cnt_nchoutwin_total_prev = byp_mcr_cnt_nchoutwin;

	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)(0x4<<1));
	tmp = READ_VREG(HEVCD_MCRCC_PERFMON_DATA);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("miss_mcr_0_cnt_total: %d\n", tmp);
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)(0x5<<1));
	tmp = READ_VREG(HEVCD_MCRCC_PERFMON_DATA);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("miss_mcr_1_cnt_total: %d\n", tmp);
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)(0x6<<1));
	tmp = READ_VREG(HEVCD_MCRCC_PERFMON_DATA);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("hit_mcr_0_cnt_total: %d\n", tmp);
	delta_hit_mcr_0_cnt = tmp - hit_mcr_0_cnt_total_prev;
	hit_mcr_0_cnt_total_prev = tmp;
	WRITE_VREG(HEVCD_MCRCC_PERFMON_CTL, (unsigned int)(0x7<<1));
	tmp = READ_VREG(HEVCD_MCRCC_PERFMON_DATA);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("hit_mcr_1_cnt_total: %d\n", tmp);
	delta_hit_mcr_1_cnt = tmp - hit_mcr_1_cnt_total_prev;
	hit_mcr_1_cnt_total_prev = tmp;

	if (delta_raw_mcr_cnt != 0) {
		hitrate = 100 * delta_hit_mcr_0_cnt
			/ delta_raw_mcr_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("CANV0_HIT_RATE : %d\n", hitrate);
		hitrate = 100 * delta_hit_mcr_1_cnt
			/ delta_raw_mcr_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("CANV1_HIT_RATE : %d\n", hitrate);
		hitrate = 100 * delta_mcr_cnt_nchcanv
			/ delta_raw_mcr_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("NONCACH_CANV_BYP_RATE : %d\n", hitrate);
		hitrate = 100 * delta_mcr_cnt_nchoutwin
			/ delta_raw_mcr_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("CACHE_OUTWIN_BYP_RATE : %d\n", hitrate);
	}


	if (raw_mcr_cnt != 0) {
		hitrate = 100 * hit_mcr_cnt / raw_mcr_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("MCRCC_HIT_RATE : %d\n", hitrate);
		hitrate = 100 * (byp_mcr_cnt_nchoutwin + byp_mcr_cnt_nchcanv)
			/ raw_mcr_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("MCRCC_BYP_RATE : %d\n", hitrate);
	} else {
		if (debug & VP9_DEBUG_CACHE)
			pr_info("MCRCC_HIT_RATE : na\n");
		if (debug & VP9_DEBUG_CACHE)
			pr_info("MCRCC_BYP_RATE : na\n");
	}
	return;
}


static void decomp_perfcount_reset(void)
{
	if (debug & VP9_DEBUG_CACHE)
		pr_info("[cache_util.c] Entered decomp_perfcount_reset...\n");
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)0x1);
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)0x0);
	return;
}

static void decomp_get_hitrate(void)
{
	unsigned   raw_mcr_cnt;
	unsigned   hit_mcr_cnt;
	int      hitrate;
	if (debug & VP9_DEBUG_CACHE)
		pr_info("[cache_util.c] Entered decomp_get_hitrate...\n");
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)(0x0<<1));
	raw_mcr_cnt = READ_VREG(HEVCD_MPP_DECOMP_PERFMON_DATA);
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)(0x1<<1));
	hit_mcr_cnt = READ_VREG(HEVCD_MPP_DECOMP_PERFMON_DATA);

	if (debug & VP9_DEBUG_CACHE)
		pr_info("hcache_raw_cnt_total: %d\n", raw_mcr_cnt);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("hcache_hit_cnt_total: %d\n", hit_mcr_cnt);

	if (raw_mcr_cnt != 0) {
		hitrate = hit_mcr_cnt * 100 / raw_mcr_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("DECOMP_HCACHE_HIT_RATE : %d\n", hitrate);
	} else {
		if (debug & VP9_DEBUG_CACHE)
			pr_info("DECOMP_HCACHE_HIT_RATE : na\n");
	}
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)(0x2<<1));
	raw_mcr_cnt = READ_VREG(HEVCD_MPP_DECOMP_PERFMON_DATA);
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)(0x3<<1));
	hit_mcr_cnt = READ_VREG(HEVCD_MPP_DECOMP_PERFMON_DATA);

	if (debug & VP9_DEBUG_CACHE)
		pr_info("dcache_raw_cnt_total: %d\n", raw_mcr_cnt);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("dcache_hit_cnt_total: %d\n", hit_mcr_cnt);

	if (raw_mcr_cnt != 0) {
		hitrate = hit_mcr_cnt * 100 / raw_mcr_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("DECOMP_DCACHE_HIT_RATE : %d\n", hitrate);
	} else {
		if (debug & VP9_DEBUG_CACHE)
			pr_info("DECOMP_DCACHE_HIT_RATE : na\n");
	}
	return;
}

static void decomp_get_comp_rate(void)
{
	unsigned   raw_ucomp_cnt;
	unsigned   fast_comp_cnt;
	unsigned   slow_comp_cnt;
	int      comp_rate;

	if (debug & VP9_DEBUG_CACHE)
		pr_info("[cache_util.c] Entered decomp_get_comp_rate...\n");
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)(0x4<<1));
	fast_comp_cnt = READ_VREG(HEVCD_MPP_DECOMP_PERFMON_DATA);
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)(0x5<<1));
	slow_comp_cnt = READ_VREG(HEVCD_MPP_DECOMP_PERFMON_DATA);
	WRITE_VREG(HEVCD_MPP_DECOMP_PERFMON_CTL, (unsigned int)(0x6<<1));
	raw_ucomp_cnt = READ_VREG(HEVCD_MPP_DECOMP_PERFMON_DATA);

	if (debug & VP9_DEBUG_CACHE)
		pr_info("decomp_fast_comp_total: %d\n", fast_comp_cnt);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("decomp_slow_comp_total: %d\n", slow_comp_cnt);
	if (debug & VP9_DEBUG_CACHE)
		pr_info("decomp_raw_uncomp_total: %d\n", raw_ucomp_cnt);

	if (raw_ucomp_cnt != 0) {
		comp_rate = (fast_comp_cnt + slow_comp_cnt)
		* 100 / raw_ucomp_cnt;
		if (debug & VP9_DEBUG_CACHE)
			pr_info("DECOMP_COMP_RATIO : %d\n", comp_rate);
	} else {
		if (debug & VP9_DEBUG_CACHE)
			pr_info("DECOMP_COMP_RATIO : na\n");
	}
	return;
}
/* cache_util.c end */

/*
 *========================================================================
 *vp9_prob define
 *========================================================================
 */
#define VP9_PARTITION_START      0
#define VP9_PARTITION_SIZE_STEP  (3 * 4)
#define VP9_PARTITION_ONE_SIZE   (4 * VP9_PARTITION_SIZE_STEP)
#define VP9_PARTITION_KEY_START  0
#define VP9_PARTITION_P_START    VP9_PARTITION_ONE_SIZE
#define VP9_PARTITION_SIZE       (2 * VP9_PARTITION_ONE_SIZE)
#define VP9_SKIP_START           (VP9_PARTITION_START + VP9_PARTITION_SIZE)
#define VP9_SKIP_SIZE            4 /* only use 3*/
#define VP9_TX_MODE_START        (VP9_SKIP_START+VP9_SKIP_SIZE)
#define VP9_TX_MODE_8_0_OFFSET   0
#define VP9_TX_MODE_8_1_OFFSET   1
#define VP9_TX_MODE_16_0_OFFSET  2
#define VP9_TX_MODE_16_1_OFFSET  4
#define VP9_TX_MODE_32_0_OFFSET  6
#define VP9_TX_MODE_32_1_OFFSET  9
#define VP9_TX_MODE_SIZE         12
#define VP9_COEF_START           (VP9_TX_MODE_START+VP9_TX_MODE_SIZE)
#define VP9_COEF_BAND_0_OFFSET   0
#define VP9_COEF_BAND_1_OFFSET   (VP9_COEF_BAND_0_OFFSET + 3 * 3 + 1)
#define VP9_COEF_BAND_2_OFFSET   (VP9_COEF_BAND_1_OFFSET + 6 * 3)
#define VP9_COEF_BAND_3_OFFSET   (VP9_COEF_BAND_2_OFFSET + 6 * 3)
#define VP9_COEF_BAND_4_OFFSET   (VP9_COEF_BAND_3_OFFSET + 6 * 3)
#define VP9_COEF_BAND_5_OFFSET   (VP9_COEF_BAND_4_OFFSET + 6 * 3)
#define VP9_COEF_SIZE_ONE_SET    100 /* ((3 +5*6)*3 + 1 padding)*/
#define VP9_COEF_4X4_START       (VP9_COEF_START + 0 * VP9_COEF_SIZE_ONE_SET)
#define VP9_COEF_8X8_START       (VP9_COEF_START + 4 * VP9_COEF_SIZE_ONE_SET)
#define VP9_COEF_16X16_START     (VP9_COEF_START + 8 * VP9_COEF_SIZE_ONE_SET)
#define VP9_COEF_32X32_START     (VP9_COEF_START + 12 * VP9_COEF_SIZE_ONE_SET)
#define VP9_COEF_SIZE_PLANE      (2 * VP9_COEF_SIZE_ONE_SET)
#define VP9_COEF_SIZE            (4 * 2 * 2 * VP9_COEF_SIZE_ONE_SET)
#define VP9_INTER_MODE_START     (VP9_COEF_START+VP9_COEF_SIZE)
#define VP9_INTER_MODE_SIZE      24 /* only use 21 ( #*7)*/
#define VP9_INTERP_START         (VP9_INTER_MODE_START+VP9_INTER_MODE_SIZE)
#define VP9_INTERP_SIZE          8
#define VP9_INTRA_INTER_START    (VP9_INTERP_START+VP9_INTERP_SIZE)
#define VP9_INTRA_INTER_SIZE     4
#define VP9_INTERP_INTRA_INTER_START  VP9_INTERP_START
#define VP9_INTERP_INTRA_INTER_SIZE   (VP9_INTERP_SIZE + VP9_INTRA_INTER_SIZE)
#define VP9_COMP_INTER_START     \
		(VP9_INTERP_INTRA_INTER_START+VP9_INTERP_INTRA_INTER_SIZE)
#define VP9_COMP_INTER_SIZE      5
#define VP9_COMP_REF_START       (VP9_COMP_INTER_START+VP9_COMP_INTER_SIZE)
#define VP9_COMP_REF_SIZE        5
#define VP9_SINGLE_REF_START     (VP9_COMP_REF_START+VP9_COMP_REF_SIZE)
#define VP9_SINGLE_REF_SIZE      10
#define VP9_REF_MODE_START       VP9_COMP_INTER_START
#define VP9_REF_MODE_SIZE        \
		(VP9_COMP_INTER_SIZE+VP9_COMP_REF_SIZE+VP9_SINGLE_REF_SIZE)
#define VP9_IF_Y_MODE_START      (VP9_REF_MODE_START+VP9_REF_MODE_SIZE)
#define VP9_IF_Y_MODE_SIZE       36
#define VP9_IF_UV_MODE_START     (VP9_IF_Y_MODE_START+VP9_IF_Y_MODE_SIZE)
#define VP9_IF_UV_MODE_SIZE      92 /* only use 90*/
#define VP9_MV_JOINTS_START      (VP9_IF_UV_MODE_START+VP9_IF_UV_MODE_SIZE)
#define VP9_MV_JOINTS_SIZE       3
#define VP9_MV_SIGN_0_START      (VP9_MV_JOINTS_START+VP9_MV_JOINTS_SIZE)
#define VP9_MV_SIGN_0_SIZE       1
#define VP9_MV_CLASSES_0_START   (VP9_MV_SIGN_0_START+VP9_MV_SIGN_0_SIZE)
#define VP9_MV_CLASSES_0_SIZE    10
#define VP9_MV_CLASS0_0_START    (VP9_MV_CLASSES_0_START+VP9_MV_CLASSES_0_SIZE)
#define VP9_MV_CLASS0_0_SIZE     1
#define VP9_MV_BITS_0_START      (VP9_MV_CLASS0_0_START+VP9_MV_CLASS0_0_SIZE)
#define VP9_MV_BITS_0_SIZE       10
#define VP9_MV_SIGN_1_START      (VP9_MV_BITS_0_START+VP9_MV_BITS_0_SIZE)
#define VP9_MV_SIGN_1_SIZE       1
#define VP9_MV_CLASSES_1_START   \
			(VP9_MV_SIGN_1_START+VP9_MV_SIGN_1_SIZE)
#define VP9_MV_CLASSES_1_SIZE    10
#define VP9_MV_CLASS0_1_START    \
			(VP9_MV_CLASSES_1_START+VP9_MV_CLASSES_1_SIZE)
#define VP9_MV_CLASS0_1_SIZE     1
#define VP9_MV_BITS_1_START      \
			(VP9_MV_CLASS0_1_START+VP9_MV_CLASS0_1_SIZE)
#define VP9_MV_BITS_1_SIZE       10
#define VP9_MV_CLASS0_FP_0_START \
			(VP9_MV_BITS_1_START+VP9_MV_BITS_1_SIZE)
#define VP9_MV_CLASS0_FP_0_SIZE  9
#define VP9_MV_CLASS0_FP_1_START \
			(VP9_MV_CLASS0_FP_0_START+VP9_MV_CLASS0_FP_0_SIZE)
#define VP9_MV_CLASS0_FP_1_SIZE  9
#define VP9_MV_CLASS0_HP_0_START \
			(VP9_MV_CLASS0_FP_1_START+VP9_MV_CLASS0_FP_1_SIZE)
#define VP9_MV_CLASS0_HP_0_SIZE  2
#define VP9_MV_CLASS0_HP_1_START \
			(VP9_MV_CLASS0_HP_0_START+VP9_MV_CLASS0_HP_0_SIZE)
#define VP9_MV_CLASS0_HP_1_SIZE  2
#define VP9_MV_START             VP9_MV_JOINTS_START
#define VP9_MV_SIZE              72 /*only use 69*/

#define VP9_TOTAL_SIZE           (VP9_MV_START + VP9_MV_SIZE)


/*========================================================================
 *	vp9_count_mem define
 *========================================================================
 */
#define VP9_COEF_COUNT_START           0
#define VP9_COEF_COUNT_BAND_0_OFFSET   0
#define VP9_COEF_COUNT_BAND_1_OFFSET   \
			(VP9_COEF_COUNT_BAND_0_OFFSET + 3*5)
#define VP9_COEF_COUNT_BAND_2_OFFSET   \
			(VP9_COEF_COUNT_BAND_1_OFFSET + 6*5)
#define VP9_COEF_COUNT_BAND_3_OFFSET   \
			(VP9_COEF_COUNT_BAND_2_OFFSET + 6*5)
#define VP9_COEF_COUNT_BAND_4_OFFSET   \
			(VP9_COEF_COUNT_BAND_3_OFFSET + 6*5)
#define VP9_COEF_COUNT_BAND_5_OFFSET   \
			(VP9_COEF_COUNT_BAND_4_OFFSET + 6*5)
#define VP9_COEF_COUNT_SIZE_ONE_SET    165 /* ((3 +5*6)*5 */
#define VP9_COEF_COUNT_4X4_START       \
	(VP9_COEF_COUNT_START + 0*VP9_COEF_COUNT_SIZE_ONE_SET)
#define VP9_COEF_COUNT_8X8_START       \
	(VP9_COEF_COUNT_START + 4*VP9_COEF_COUNT_SIZE_ONE_SET)
#define VP9_COEF_COUNT_16X16_START     \
	(VP9_COEF_COUNT_START + 8*VP9_COEF_COUNT_SIZE_ONE_SET)
#define VP9_COEF_COUNT_32X32_START     \
	(VP9_COEF_COUNT_START + 12*VP9_COEF_COUNT_SIZE_ONE_SET)
#define VP9_COEF_COUNT_SIZE_PLANE      (2 * VP9_COEF_COUNT_SIZE_ONE_SET)
#define VP9_COEF_COUNT_SIZE            (4 * 2 * 2 * VP9_COEF_COUNT_SIZE_ONE_SET)

#define VP9_INTRA_INTER_COUNT_START    \
	(VP9_COEF_COUNT_START+VP9_COEF_COUNT_SIZE)
#define VP9_INTRA_INTER_COUNT_SIZE     (4*2)
#define VP9_COMP_INTER_COUNT_START     \
	(VP9_INTRA_INTER_COUNT_START+VP9_INTRA_INTER_COUNT_SIZE)
#define VP9_COMP_INTER_COUNT_SIZE      (5*2)
#define VP9_COMP_REF_COUNT_START       \
	(VP9_COMP_INTER_COUNT_START+VP9_COMP_INTER_COUNT_SIZE)
#define VP9_COMP_REF_COUNT_SIZE        (5*2)
#define VP9_SINGLE_REF_COUNT_START     \
	(VP9_COMP_REF_COUNT_START+VP9_COMP_REF_COUNT_SIZE)
#define VP9_SINGLE_REF_COUNT_SIZE      (10*2)
#define VP9_TX_MODE_COUNT_START        \
	(VP9_SINGLE_REF_COUNT_START+VP9_SINGLE_REF_COUNT_SIZE)
#define VP9_TX_MODE_COUNT_SIZE         (12*2)
#define VP9_SKIP_COUNT_START           \
	(VP9_TX_MODE_COUNT_START+VP9_TX_MODE_COUNT_SIZE)
#define VP9_SKIP_COUNT_SIZE            (3*2)
#define VP9_MV_SIGN_0_COUNT_START      \
	(VP9_SKIP_COUNT_START+VP9_SKIP_COUNT_SIZE)
#define VP9_MV_SIGN_0_COUNT_SIZE       (1*2)
#define VP9_MV_SIGN_1_COUNT_START      \
	(VP9_MV_SIGN_0_COUNT_START+VP9_MV_SIGN_0_COUNT_SIZE)
#define VP9_MV_SIGN_1_COUNT_SIZE       (1*2)
#define VP9_MV_BITS_0_COUNT_START      \
	(VP9_MV_SIGN_1_COUNT_START+VP9_MV_SIGN_1_COUNT_SIZE)
#define VP9_MV_BITS_0_COUNT_SIZE       (10*2)
#define VP9_MV_BITS_1_COUNT_START      \
	(VP9_MV_BITS_0_COUNT_START+VP9_MV_BITS_0_COUNT_SIZE)
#define VP9_MV_BITS_1_COUNT_SIZE       (10*2)
#define VP9_MV_CLASS0_HP_0_COUNT_START \
	(VP9_MV_BITS_1_COUNT_START+VP9_MV_BITS_1_COUNT_SIZE)
#define VP9_MV_CLASS0_HP_0_COUNT_SIZE  (2*2)
#define VP9_MV_CLASS0_HP_1_COUNT_START \
	(VP9_MV_CLASS0_HP_0_COUNT_START+VP9_MV_CLASS0_HP_0_COUNT_SIZE)
#define VP9_MV_CLASS0_HP_1_COUNT_SIZE  (2*2)
/* Start merge_tree*/
#define VP9_INTER_MODE_COUNT_START     \
	(VP9_MV_CLASS0_HP_1_COUNT_START+VP9_MV_CLASS0_HP_1_COUNT_SIZE)
#define VP9_INTER_MODE_COUNT_SIZE      (7*4)
#define VP9_IF_Y_MODE_COUNT_START      \
	(VP9_INTER_MODE_COUNT_START+VP9_INTER_MODE_COUNT_SIZE)
#define VP9_IF_Y_MODE_COUNT_SIZE       (10*4)
#define VP9_IF_UV_MODE_COUNT_START     \
	(VP9_IF_Y_MODE_COUNT_START+VP9_IF_Y_MODE_COUNT_SIZE)
#define VP9_IF_UV_MODE_COUNT_SIZE      (10*10)
#define VP9_PARTITION_P_COUNT_START    \
	(VP9_IF_UV_MODE_COUNT_START+VP9_IF_UV_MODE_COUNT_SIZE)
#define VP9_PARTITION_P_COUNT_SIZE     (4*4*4)
#define VP9_INTERP_COUNT_START         \
	(VP9_PARTITION_P_COUNT_START+VP9_PARTITION_P_COUNT_SIZE)
#define VP9_INTERP_COUNT_SIZE          (4*3)
#define VP9_MV_JOINTS_COUNT_START      \
	(VP9_INTERP_COUNT_START+VP9_INTERP_COUNT_SIZE)
#define VP9_MV_JOINTS_COUNT_SIZE       (1 * 4)
#define VP9_MV_CLASSES_0_COUNT_START   \
	(VP9_MV_JOINTS_COUNT_START+VP9_MV_JOINTS_COUNT_SIZE)
#define VP9_MV_CLASSES_0_COUNT_SIZE    (1*11)
#define VP9_MV_CLASS0_0_COUNT_START    \
	(VP9_MV_CLASSES_0_COUNT_START+VP9_MV_CLASSES_0_COUNT_SIZE)
#define VP9_MV_CLASS0_0_COUNT_SIZE     (1*2)
#define VP9_MV_CLASSES_1_COUNT_START   \
	(VP9_MV_CLASS0_0_COUNT_START+VP9_MV_CLASS0_0_COUNT_SIZE)
#define VP9_MV_CLASSES_1_COUNT_SIZE    (1*11)
#define VP9_MV_CLASS0_1_COUNT_START    \
	(VP9_MV_CLASSES_1_COUNT_START+VP9_MV_CLASSES_1_COUNT_SIZE)
#define VP9_MV_CLASS0_1_COUNT_SIZE     (1*2)
#define VP9_MV_CLASS0_FP_0_COUNT_START \
	(VP9_MV_CLASS0_1_COUNT_START+VP9_MV_CLASS0_1_COUNT_SIZE)
#define VP9_MV_CLASS0_FP_0_COUNT_SIZE  (3*4)
#define VP9_MV_CLASS0_FP_1_COUNT_START \
	(VP9_MV_CLASS0_FP_0_COUNT_START+VP9_MV_CLASS0_FP_0_COUNT_SIZE)
#define VP9_MV_CLASS0_FP_1_COUNT_SIZE  (3*4)


#define DC_PRED    0       /* Average of above and left pixels*/
#define V_PRED     1       /* Vertical*/
#define H_PRED     2       /* Horizontal*/
#define D45_PRED   3       /*Directional 45 deg = round(arctan(1/1) * 180/pi)*/
#define D135_PRED  4       /* Directional 135 deg = 180 - 45*/
#define D117_PRED  5       /* Directional 117 deg = 180 - 63*/
#define D153_PRED  6       /* Directional 153 deg = 180 - 27*/
#define D207_PRED  7       /* Directional 207 deg = 180 + 27*/
#define D63_PRED   8       /*Directional 63 deg = round(arctan(2/1) * 180/pi)*/
#define TM_PRED    9       /*True-motion*/

int clip_prob(int p)
{
	return (p > 255) ? 255 : (p < 1) ? 1 : p;
}

#define ROUND_POWER_OF_TWO(value, n) \
	(((value) + (1 << ((n) - 1))) >> (n))

#define MODE_MV_COUNT_SAT 20
static const int count_to_update_factor[MODE_MV_COUNT_SAT + 1] = {
	0, 6, 12, 19, 25, 32, 38, 44, 51, 57, 64,
	70, 76, 83, 89, 96, 102, 108, 115, 121, 128
};

void   vp9_tree_merge_probs(unsigned int *prev_prob, unsigned int *cur_prob,
	int coef_node_start, int tree_left, int tree_right, int tree_i,
	int node) {

	int prob_32, prob_res, prob_shift;
	int pre_prob, new_prob;
	int den, m_count, get_prob, factor;

	prob_32 = prev_prob[coef_node_start / 4 * 2];
	prob_res = coef_node_start & 3;
	prob_shift = prob_res * 8;
	pre_prob = (prob_32 >> prob_shift) & 0xff;

	den = tree_left + tree_right;

	if (den == 0)
		new_prob = pre_prob;
	else {
		m_count = (den < MODE_MV_COUNT_SAT) ?
			den : MODE_MV_COUNT_SAT;
		get_prob = clip_prob(
				div_r32(((int64_t)tree_left * 256 + (den >> 1)),
				den));
		/*weighted_prob*/
		factor = count_to_update_factor[m_count];
		new_prob = ROUND_POWER_OF_TWO(pre_prob * (256 - factor)
				+ get_prob * factor, 8);
	}
	cur_prob[coef_node_start / 4 * 2] = (cur_prob[coef_node_start / 4 * 2]
			& (~(0xff << prob_shift))) | (new_prob << prob_shift);
}


/*void adapt_coef_probs(void)*/
void adapt_coef_probs(int pic_count, int prev_kf, int cur_kf, int pre_fc,
	unsigned int *prev_prob, unsigned int *cur_prob, unsigned int *count)
{
	int tx_size, coef_tx_size_start, coef_count_tx_size_start;
	int plane, coef_plane_start, coef_count_plane_start;
	int type, coef_type_start, coef_count_type_start;
	int band, coef_band_start, coef_count_band_start;
	int cxt_num;
	int cxt, coef_cxt_start, coef_count_cxt_start;
	int node, coef_node_start, coef_count_node_start;

	int tree_i, tree_left, tree_right;
	int mvd_i;

	int count_sat = 24;
	int update_factor =   cur_kf ? 112 :
			prev_kf ? 128 : 112;

	int prob_32;
	int prob_res;
	int prob_shift;
	int pre_prob;

	int num, den;
	int get_prob;
	int m_count;
	int factor;

	int new_prob;

	if (debug & VP9_DEBUG_MERGE)
		pr_info("\n ##adapt_coef_probs (pre_fc : %d ,prev_kf : %d,cur_kf : %d)##\n\n",
			pre_fc, prev_kf, cur_kf);

	/*adapt_coef_probs*/
	for (tx_size = 0; tx_size < 4; tx_size++) {
		coef_tx_size_start = VP9_COEF_START
			+ tx_size * 4 * VP9_COEF_SIZE_ONE_SET;
		coef_count_tx_size_start = VP9_COEF_COUNT_START
			+ tx_size * 4 * VP9_COEF_COUNT_SIZE_ONE_SET;
		coef_plane_start = coef_tx_size_start;
		coef_count_plane_start = coef_count_tx_size_start;
		for (plane = 0; plane < 2; plane++) {
			coef_type_start = coef_plane_start;
			coef_count_type_start = coef_count_plane_start;
			for (type = 0; type < 2; type++) {
				coef_band_start = coef_type_start;
				coef_count_band_start = coef_count_type_start;
				for (band = 0; band < 6; band++) {
					if (band == 0)
						cxt_num = 3;
					else
						cxt_num = 6;
					coef_cxt_start = coef_band_start;
					coef_count_cxt_start =
						coef_count_band_start;
					for (cxt = 0; cxt < cxt_num; cxt++) {
						const int n0 =
						count[coef_count_cxt_start];
						const int n1 =
						count[coef_count_cxt_start + 1];
						const int n2 =
						count[coef_count_cxt_start + 2];
						const int neob =
						count[coef_count_cxt_start + 3];
						const int nneob =
						count[coef_count_cxt_start + 4];
						const unsigned int
						branch_ct[3][2] = {
						{ neob, nneob },
						{ n0, n1 + n2 },
						{ n1, n2 }
						};
						coef_node_start =
							coef_cxt_start;
						for
						(node = 0; node < 3; node++) {
							prob_32 =
							prev_prob[
							coef_node_start
							/ 4 * 2];
							prob_res =
							coef_node_start & 3;
							prob_shift =
							prob_res * 8;
							pre_prob =
							(prob_32 >> prob_shift)
							& 0xff;

							/*get_binary_prob*/
							num =
							branch_ct[node][0];
							den =
							branch_ct[node][0] +
							 branch_ct[node][1];
							m_count = (den <
							count_sat)
							? den : count_sat;

							get_prob =
							(den == 0) ? 128u :
							clip_prob(
							div_r32(((int64_t)
							num * 256
							+ (den >> 1)),
							den));

							factor =
							update_factor * m_count
							/ count_sat;
							new_prob =
							ROUND_POWER_OF_TWO
							(pre_prob *
							(256 - factor) +
							get_prob * factor, 8);

							cur_prob[coef_node_start
							/ 4 * 2] =
							(cur_prob
							[coef_node_start
							/ 4 * 2] & (~(0xff <<
							prob_shift))) |
							(new_prob <<
							prob_shift);

							coef_node_start += 1;
						}

						coef_cxt_start =
							coef_cxt_start + 3;
						coef_count_cxt_start =
							coef_count_cxt_start
							+ 5;
					}
					if (band == 0) {
						coef_band_start += 10;
						coef_count_band_start += 15;
					} else {
						coef_band_start += 18;
						coef_count_band_start += 30;
					}
				}
				coef_type_start += VP9_COEF_SIZE_ONE_SET;
				coef_count_type_start +=
					VP9_COEF_COUNT_SIZE_ONE_SET;
			}
			coef_plane_start += 2 * VP9_COEF_SIZE_ONE_SET;
			coef_count_plane_start +=
					2 * VP9_COEF_COUNT_SIZE_ONE_SET;
		}
	}

	if (cur_kf == 0) {
		/*mode_mv_merge_probs - merge_intra_inter_prob*/
		for (coef_count_node_start = VP9_INTRA_INTER_COUNT_START;
		coef_count_node_start < (VP9_MV_CLASS0_HP_1_COUNT_START +
		VP9_MV_CLASS0_HP_1_COUNT_SIZE);	coef_count_node_start += 2) {

		if (coef_count_node_start ==
			VP9_INTRA_INTER_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_intra_inter_prob\n");
			coef_node_start = VP9_INTRA_INTER_START;
		} else if (coef_count_node_start ==
			VP9_COMP_INTER_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_comp_inter_prob\n");
			coef_node_start = VP9_COMP_INTER_START;
		} else if (coef_count_node_start ==
			VP9_TX_MODE_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_tx_mode_probs\n");
			coef_node_start = VP9_TX_MODE_START;
		} else if (coef_count_node_start ==
			VP9_SKIP_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_skip_probs\n");
			coef_node_start = VP9_SKIP_START;
		} else if (coef_count_node_start ==
			VP9_MV_SIGN_0_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_sign_0\n");
			coef_node_start = VP9_MV_SIGN_0_START;
		} else if (coef_count_node_start ==
			VP9_MV_SIGN_1_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_sign_1\n");
			coef_node_start = VP9_MV_SIGN_1_START;
		} else if (coef_count_node_start ==
			VP9_MV_BITS_0_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_bits_0\n");
			coef_node_start = VP9_MV_BITS_0_START;
		} else if (coef_count_node_start ==
			VP9_MV_BITS_1_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_bits_1\n");
			coef_node_start = VP9_MV_BITS_1_START;
		} else if (coef_count_node_start ==
				VP9_MV_CLASS0_HP_0_COUNT_START) {
			if (debug & VP9_DEBUG_MERGE)
				pr_info(" # merge_class0_hp\n");
			coef_node_start = VP9_MV_CLASS0_HP_0_START;
		}

		den = count[coef_count_node_start] +
			count[coef_count_node_start + 1];

		prob_32 = prev_prob[coef_node_start / 4 * 2];
		prob_res = coef_node_start & 3;
		prob_shift = prob_res * 8;
		pre_prob = (prob_32 >> prob_shift) & 0xff;

		if (den == 0)
			new_prob = pre_prob;
		else {
			m_count = (den < MODE_MV_COUNT_SAT) ? den : MODE_MV_COUNT_SAT;
			get_prob =
				clip_prob(
				div_r32(((int64_t)count[coef_count_node_start]
				* 256 + (den >> 1)),
				den));
			/*weighted_prob*/
			factor = count_to_update_factor[m_count];
			new_prob =
				ROUND_POWER_OF_TWO(pre_prob * (256 - factor)
				+ get_prob * factor, 8);
		}
		cur_prob[coef_node_start / 4 * 2] =
			(cur_prob[coef_node_start / 4 * 2] &
			(~(0xff << prob_shift)))
			| (new_prob << prob_shift);

		coef_node_start = coef_node_start + 1;
	}
	if (debug & VP9_DEBUG_MERGE)
		pr_info(" # merge_vp9_inter_mode_tree\n");
	coef_node_start = VP9_INTER_MODE_START;
	coef_count_node_start = VP9_INTER_MODE_COUNT_START;
	for (tree_i = 0; tree_i < 7; tree_i++) {
		for (node = 0; node < 3; node++) {
			switch (node) {
			case 2:
				tree_left =
				count[coef_count_node_start + 1];
				tree_right =
				count[coef_count_node_start + 3];
				break;
			case 1:
				tree_left =
				count[coef_count_node_start + 0];
				tree_right =
				count[coef_count_node_start + 1]
				+ count[coef_count_node_start + 3];
				break;
			default:
				tree_left =
				count[coef_count_node_start + 2];
				tree_right =
				count[coef_count_node_start + 0]
				+ count[coef_count_node_start + 1]
				+ count[coef_count_node_start + 3];
				break;

			}

			vp9_tree_merge_probs(prev_prob, cur_prob,
				coef_node_start, tree_left, tree_right,
				tree_i, node);

			coef_node_start = coef_node_start + 1;
		}
		coef_count_node_start = coef_count_node_start + 4;
	}
	if (debug & VP9_DEBUG_MERGE)
		pr_info(" # merge_vp9_intra_mode_tree\n");
	coef_node_start = VP9_IF_Y_MODE_START;
	coef_count_node_start = VP9_IF_Y_MODE_COUNT_START;
	for (tree_i = 0; tree_i < 14; tree_i++) {
		for (node = 0; node < 9; node++) {
			switch (node) {
			case 8:
				tree_left =
				count[coef_count_node_start+D153_PRED];
				tree_right =
				count[coef_count_node_start+D207_PRED];
				break;
			case 7:
				tree_left =
				count[coef_count_node_start+D63_PRED];
				tree_right =
				count[coef_count_node_start+D207_PRED] +
				count[coef_count_node_start+D153_PRED];
				break;
			case 6:
				tree_left =
				count[coef_count_node_start + D45_PRED];
				tree_right =
				count[coef_count_node_start+D207_PRED] +
				count[coef_count_node_start+D153_PRED] +
				count[coef_count_node_start+D63_PRED];
				break;
			case 5:
				tree_left =
				count[coef_count_node_start+D135_PRED];
				tree_right =
				count[coef_count_node_start+D117_PRED];
				break;
			case 4:
				tree_left =
				count[coef_count_node_start+H_PRED];
				tree_right =
				count[coef_count_node_start+D117_PRED] +
				count[coef_count_node_start+D135_PRED];
				break;
			case 3:
				tree_left =
				count[coef_count_node_start+H_PRED] +
				count[coef_count_node_start+D117_PRED] +
				count[coef_count_node_start+D135_PRED];
				tree_right =
				count[coef_count_node_start+D45_PRED] +
				count[coef_count_node_start+D207_PRED] +
				count[coef_count_node_start+D153_PRED] +
				count[coef_count_node_start+D63_PRED];
				break;
			case 2:
				tree_left =
				count[coef_count_node_start+V_PRED];
				tree_right =
				count[coef_count_node_start+H_PRED] +
				count[coef_count_node_start+D117_PRED] +
				count[coef_count_node_start+D135_PRED] +
				count[coef_count_node_start+D45_PRED] +
				count[coef_count_node_start+D207_PRED] +
				count[coef_count_node_start+D153_PRED] +
				count[coef_count_node_start+D63_PRED];
				break;
			case 1:
				tree_left =
				count[coef_count_node_start+TM_PRED];
				tree_right =
				count[coef_count_node_start+V_PRED] +
				count[coef_count_node_start+H_PRED] +
				count[coef_count_node_start+D117_PRED] +
				count[coef_count_node_start+D135_PRED] +
				count[coef_count_node_start+D45_PRED] +
				count[coef_count_node_start+D207_PRED] +
				count[coef_count_node_start+D153_PRED] +
				count[coef_count_node_start+D63_PRED];
				break;
			default:
				tree_left =
				count[coef_count_node_start+DC_PRED];
				tree_right =
				count[coef_count_node_start+TM_PRED] +
				count[coef_count_node_start+V_PRED] +
				count[coef_count_node_start+H_PRED] +
				count[coef_count_node_start+D117_PRED] +
				count[coef_count_node_start+D135_PRED] +
				count[coef_count_node_start+D45_PRED] +
				count[coef_count_node_start+D207_PRED] +
				count[coef_count_node_start+D153_PRED] +
				count[coef_count_node_start+D63_PRED];
				break;

				}

			vp9_tree_merge_probs(prev_prob, cur_prob,
				coef_node_start, tree_left, tree_right,
				tree_i, node);

			coef_node_start = coef_node_start + 1;
		}
		coef_count_node_start = coef_count_node_start + 10;
	}

	if (debug & VP9_DEBUG_MERGE)
		pr_info(" # merge_vp9_partition_tree\n");
	coef_node_start = VP9_PARTITION_P_START;
	coef_count_node_start = VP9_PARTITION_P_COUNT_START;
	for (tree_i = 0; tree_i < 16; tree_i++) {
		for (node = 0; node < 3; node++) {
			switch (node) {
			case 2:
				tree_left =
				count[coef_count_node_start + 2];
				tree_right =
				count[coef_count_node_start + 3];
				break;
			case 1:
				tree_left =
				count[coef_count_node_start + 1];
				tree_right =
				count[coef_count_node_start + 2] +
				count[coef_count_node_start + 3];
				break;
			default:
				tree_left =
				count[coef_count_node_start + 0];
				tree_right =
				count[coef_count_node_start + 1] +
				count[coef_count_node_start + 2] +
				count[coef_count_node_start + 3];
				break;

			}

			vp9_tree_merge_probs(prev_prob, cur_prob,
				coef_node_start,
				tree_left, tree_right, tree_i, node);

			coef_node_start = coef_node_start + 1;
		}
		coef_count_node_start = coef_count_node_start + 4;
	}

	if (debug & VP9_DEBUG_MERGE)
		pr_info(" # merge_vp9_switchable_interp_tree\n");
	coef_node_start = VP9_INTERP_START;
	coef_count_node_start = VP9_INTERP_COUNT_START;
	for (tree_i = 0; tree_i < 4; tree_i++) {
		for (node = 0; node < 2; node++) {
			switch (node) {
			case 1:
				tree_left =
				count[coef_count_node_start + 1];
				tree_right =
				count[coef_count_node_start + 2];
				break;
			default:
				tree_left =
				count[coef_count_node_start + 0];
				tree_right =
				count[coef_count_node_start + 1] +
				count[coef_count_node_start + 2];
				break;

			}

			vp9_tree_merge_probs(prev_prob, cur_prob,
				coef_node_start,
				tree_left, tree_right, tree_i, node);

			coef_node_start = coef_node_start + 1;
		}
		coef_count_node_start = coef_count_node_start + 3;
	}

	if (debug & VP9_DEBUG_MERGE)
		pr_info("# merge_vp9_mv_joint_tree\n");
	coef_node_start = VP9_MV_JOINTS_START;
	coef_count_node_start = VP9_MV_JOINTS_COUNT_START;
	for (tree_i = 0; tree_i < 1; tree_i++) {
		for (node = 0; node < 3; node++) {
			switch (node) {
			case 2:
				tree_left =
				count[coef_count_node_start + 2];
				tree_right =
				count[coef_count_node_start + 3];
				break;
			case 1:
				tree_left =
				count[coef_count_node_start + 1];
				tree_right =
				count[coef_count_node_start + 2] +
				count[coef_count_node_start + 3];
				break;
			default:
				tree_left =
				count[coef_count_node_start + 0];
				tree_right =
				count[coef_count_node_start + 1] +
				count[coef_count_node_start + 2] +
				count[coef_count_node_start + 3];
				break;
			}

			vp9_tree_merge_probs(prev_prob, cur_prob,
				coef_node_start,
				tree_left, tree_right, tree_i, node);

			coef_node_start = coef_node_start + 1;
		}
		coef_count_node_start = coef_count_node_start + 4;
	}

	for (mvd_i = 0; mvd_i < 2; mvd_i++) {
		if (debug & VP9_DEBUG_MERGE)
			pr_info(" # merge_vp9_mv_class_tree [%d]  -\n", mvd_i);
		coef_node_start =
			mvd_i ? VP9_MV_CLASSES_1_START : VP9_MV_CLASSES_0_START;
		coef_count_node_start =
			mvd_i ? VP9_MV_CLASSES_1_COUNT_START
			: VP9_MV_CLASSES_0_COUNT_START;
		tree_i = 0;
		for (node = 0; node < 10; node++) {
			switch (node) {
			case 9:
				tree_left =
				count[coef_count_node_start + 9];
				tree_right =
				count[coef_count_node_start + 10];
				break;
			case 8:
				tree_left =
				count[coef_count_node_start + 7];
				tree_right =
				count[coef_count_node_start + 8];
				break;
			case 7:
				tree_left =
				count[coef_count_node_start + 7] +
				count[coef_count_node_start + 8];
				tree_right =
				count[coef_count_node_start + 9] +
				count[coef_count_node_start + 10];
				break;
			case 6:
				tree_left =
				count[coef_count_node_start + 6];
				tree_right =
				count[coef_count_node_start + 7] +
				count[coef_count_node_start + 8] +
				count[coef_count_node_start + 9] +
				count[coef_count_node_start + 10];
				break;
			case 5:
				tree_left =
				count[coef_count_node_start + 4];
				tree_right =
				count[coef_count_node_start + 5];
				break;
			case 4:
				tree_left =
				count[coef_count_node_start + 4] +
				count[coef_count_node_start + 5];
				tree_right =
				count[coef_count_node_start + 6] +
				count[coef_count_node_start + 7] +
				count[coef_count_node_start + 8] +
				count[coef_count_node_start + 9] +
				count[coef_count_node_start + 10];
				break;
			case 3:
				tree_left =
				count[coef_count_node_start + 2];
				tree_right =
				count[coef_count_node_start + 3];
				break;
			case 2:
				tree_left =
				count[coef_count_node_start + 2] +
				count[coef_count_node_start + 3];
				tree_right =
				count[coef_count_node_start + 4] +
				count[coef_count_node_start + 5] +
				count[coef_count_node_start + 6] +
				count[coef_count_node_start + 7] +
				count[coef_count_node_start + 8] +
				count[coef_count_node_start + 9] +
				count[coef_count_node_start + 10];
				break;
			case 1:
				tree_left =
				count[coef_count_node_start + 1];
				tree_right =
				count[coef_count_node_start + 2] +
				count[coef_count_node_start + 3] +
				count[coef_count_node_start + 4] +
				count[coef_count_node_start + 5] +
				count[coef_count_node_start + 6] +
				count[coef_count_node_start + 7] +
				count[coef_count_node_start + 8] +
				count[coef_count_node_start + 9] +
				count[coef_count_node_start + 10];
				break;
			default:
				tree_left =
				count[coef_count_node_start + 0];
				tree_right =
				count[coef_count_node_start + 1] +
				count[coef_count_node_start + 2] +
				count[coef_count_node_start + 3] +
				count[coef_count_node_start + 4] +
				count[coef_count_node_start + 5] +
				count[coef_count_node_start + 6] +
				count[coef_count_node_start + 7] +
				count[coef_count_node_start + 8] +
				count[coef_count_node_start + 9] +
				count[coef_count_node_start + 10];
				break;

			}

			vp9_tree_merge_probs(prev_prob, cur_prob,
				coef_node_start, tree_left, tree_right,
				tree_i, node);

			coef_node_start = coef_node_start + 1;
		}

		if (debug & VP9_DEBUG_MERGE)
			pr_info(" # merge_vp9_mv_class0_tree [%d]  -\n", mvd_i);
		coef_node_start =
			mvd_i ? VP9_MV_CLASS0_1_START : VP9_MV_CLASS0_0_START;
		coef_count_node_start =
			mvd_i ? VP9_MV_CLASS0_1_COUNT_START :
			VP9_MV_CLASS0_0_COUNT_START;
		tree_i = 0;
		node = 0;
		tree_left = count[coef_count_node_start + 0];
		tree_right = count[coef_count_node_start + 1];

		vp9_tree_merge_probs(prev_prob, cur_prob, coef_node_start,
			tree_left, tree_right, tree_i, node);
		if (debug & VP9_DEBUG_MERGE)
			pr_info(" # merge_vp9_mv_fp_tree_class0_fp [%d]  -\n",
				mvd_i);
		coef_node_start =
			mvd_i ? VP9_MV_CLASS0_FP_1_START :
			VP9_MV_CLASS0_FP_0_START;
		coef_count_node_start =
			mvd_i ? VP9_MV_CLASS0_FP_1_COUNT_START :
			VP9_MV_CLASS0_FP_0_COUNT_START;
		for (tree_i = 0; tree_i < 3; tree_i++) {
			for (node = 0; node < 3; node++) {
				switch (node) {
				case 2:
					tree_left =
					count[coef_count_node_start + 2];
					tree_right =
					count[coef_count_node_start + 3];
					break;
				case 1:
					tree_left =
					count[coef_count_node_start + 1];
					tree_right =
					count[coef_count_node_start + 2]
					+ count[coef_count_node_start + 3];
					break;
				default:
					tree_left =
					count[coef_count_node_start + 0];
					tree_right =
					count[coef_count_node_start + 1]
					+ count[coef_count_node_start + 2]
					+ count[coef_count_node_start + 3];
					break;

				}

				vp9_tree_merge_probs(prev_prob, cur_prob,
					coef_node_start, tree_left, tree_right,
					tree_i, node);

				coef_node_start = coef_node_start + 1;
			}
			coef_count_node_start = coef_count_node_start + 4;
		}

	} /* for mvd_i (mvd_y or mvd_x)*/
}

}

static void uninit_mmu_buffers(struct VP9Decoder_s *pbi)
{
#ifndef MV_USE_FIXED_BUF
	dealloc_mv_bufs(pbi);
#endif
	if (pbi->mmu_box) {
		decoder_mmu_box_free(pbi->mmu_box);
		pbi->mmu_box = NULL;
	}

	if (pbi->bmmu_box) {
		decoder_bmmu_box_free(pbi->bmmu_box);
		pbi->bmmu_box = NULL;
	}
}

static int calc_luc_quantity(u32 w, u32 h)
{
	int lcu_size = 64; /*fixed 64*/
	int pic_width_64 = (w + 63) & (~0x3f);
	int pic_height_32 = (h + 31) & (~0x1f);
	int pic_width_lcu  = (pic_width_64 % lcu_size) ?
		pic_width_64 / lcu_size  + 1 : pic_width_64 / lcu_size;
	int pic_height_lcu = (pic_height_32 % lcu_size) ?
		pic_height_32 / lcu_size + 1 : pic_height_32 / lcu_size;

	return pic_width_lcu * pic_height_lcu;
}

/* return in MB */
static int vp9_max_mmu_buf_size(int max_w, int max_h)
{
	int buf_size = 48;

	if ((max_w * max_h > 1280*736) &&
		(max_w * max_h <= 1920*1088)) {
		buf_size = 12;
	} else if ((max_w * max_h > 0) &&
		(max_w * max_h <= 1280*736)) {
		buf_size = 4;
	}

	return buf_size;
}

static void vp9_put_video_frame(void *vdec_ctx, struct vframe_s *vf)
{
	vvp9_vf_put(vf, vdec_ctx);
}

static void vp9_get_video_frame(void *vdec_ctx, struct vframe_s *vf)
{
	memcpy(vf, vvp9_vf_get(vdec_ctx), sizeof(struct vframe_s));
}

static struct task_ops_s task_dec_ops = {
	.type		= TASK_TYPE_DEC,
	.get_vframe	= vp9_get_video_frame,
	.put_vframe	= vp9_put_video_frame,
};

static int v4l_alloc_and_config_pic(struct VP9Decoder_s *pbi,
	struct PIC_BUFFER_CONFIG_s *pic)
{
	int i = pic->index;
	int dw_mode = get_double_write_mode_init(pbi);
	int lcu_total = calc_luc_quantity(pbi->frame_width, pbi->frame_height);
#ifdef MV_USE_FIXED_BUF
	u32 mpred_mv_end = pbi->work_space_buf->mpred_mv.buf_start +
		pbi->work_space_buf->mpred_mv.buf_size;
	int mv_size = cal_mv_buf_size(pbi, pbi->frame_width, pbi->frame_height);
#endif
	struct aml_buf *aml_buf = pbi->aml_buf;

	if (!aml_buf) {
		vp9_print(pbi, 0, "[ERR]aml_buf is NULL!\n");
		return -1;
	}

	if (pbi->mmu_enable) {
		pbi->m_BUF[i].header_addr = aml_buf->fbc->haddr;
		if (debug & VP9_DEBUG_BUFMGR_MORE) {
			pr_info("MMU header_adr %d: %lx\n",
				i, pbi->m_BUF[i].header_addr);
		}
	}


#ifdef MV_USE_FIXED_BUF
	if ((pbi->work_space_buf->mpred_mv.buf_start +
		((i + 1) * mv_size))
		<= mpred_mv_end) {
#endif
	pbi->m_BUF[i].v4l_ref_buf_addr = (ulong)aml_buf;
	pic->cma_alloc_addr = aml_buf->planes[0].addr;

	if (pbi->mmu_enable &&
		!pbi->afbc_buf_table[aml_buf->fbc->index].used) {
		if (!vdec_secure(hw_to_vdec(pbi)))
			vdec_mm_dma_flush(aml_buf->fbc->haddr,
					aml_buf->fbc->hsize);
		pbi->afbc_buf_table[aml_buf->fbc->index].fb = pbi->m_BUF[i].v4l_ref_buf_addr;
		pbi->afbc_buf_table[aml_buf->fbc->index].used = 1;
		vp9_print(pbi, VP9_DEBUG_BUFMGR,
				"fb(afbc_index: %d) fb: 0x%lx, i: %d!\n",
				aml_buf->fbc->index, pbi->m_BUF[i].v4l_ref_buf_addr, i);
	}

	if (aml_buf->num_planes == 1) {
		pbi->m_BUF[i].start_adr = aml_buf->planes[0].addr;
		pbi->m_BUF[i].luma_size = aml_buf->planes[0].offset;
		pbi->m_BUF[i].size = aml_buf->planes[0].length;
		aml_buf->planes[0].bytes_used = aml_buf->planes[0].length;
		pic->dw_y_adr = pbi->m_BUF[i].start_adr;
		pic->dw_u_v_adr = pic->dw_y_adr + pbi->m_BUF[i].luma_size;
		pic->luma_size = aml_buf->planes[0].offset;
		pic->chroma_size = aml_buf->planes[0].length - aml_buf->planes[0].offset;
	} else if (aml_buf->num_planes == 2) {
		pbi->m_BUF[i].start_adr = aml_buf->planes[0].addr;
		pbi->m_BUF[i].size = aml_buf->planes[0].length;
		pbi->m_BUF[i].chroma_addr = aml_buf->planes[1].addr;
		pbi->m_BUF[i].chroma_size = aml_buf->planes[1].length;
		aml_buf->planes[0].bytes_used = aml_buf->planes[0].length;
		aml_buf->planes[1].bytes_used = aml_buf->planes[1].length;
		pic->dw_y_adr = pbi->m_BUF[i].start_adr;
		pic->dw_u_v_adr = pbi->m_BUF[i].chroma_addr;
		pic->luma_size = aml_buf->planes[0].length;
		pic->chroma_size = aml_buf->planes[1].length;
	}

	/* config frame buffer */
	if (pbi->mmu_enable)
		pic->header_adr = pbi->m_BUF[i].header_addr;

	pic->BUF_index		= i;
	pic->lcu_total		= lcu_total;
	pic->mc_canvas_y	= pic->index;
	pic->mc_canvas_u_v	= pic->index;

	if (dw_mode & 0x10) {
		pic->mc_canvas_y = (pic->index << 1);
		pic->mc_canvas_u_v = (pic->index << 1) + 1;
	}

#ifdef MV_USE_FIXED_BUF
	pic->mpred_mv_wr_start_addr =
		pbi->work_space_buf->mpred_mv.buf_start +
		(pic->index * mv_size);
	pic->mv_size = mv_size;
#endif
	if (debug) {
		pr_info("%s index %d BUF_index %d ",
			__func__, pic->index,
			pic->BUF_index);
		pr_info("comp_body_size %x comp_buf_size %x ",
			pic->comp_body_size,
			pic->buf_size);
		pr_info("mpred_mv_wr_start_adr %ld\n",
			pic->mpred_mv_wr_start_addr);
		pr_info("dw_y_adr %d, pic_config->dw_u_v_adr =%d\n",
			pic->dw_y_adr,
			pic->dw_u_v_adr);
	}
#ifdef MV_USE_FIXED_BUF
	}
#endif

	return 0;
}


static void init_pic_list(struct VP9Decoder_s *pbi)
{
	int i;
	struct VP9_Common_s *cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s *pic_config;
	struct vdec_s *vdec = hw_to_vdec(pbi);

	for (i = 0; i < pbi->used_buf_num; i++) {
		pic_config = &cm->buffer_pool->frame_bufs[i].buf;
		pic_config->index = i;
		pic_config->BUF_index = -1;
		pic_config->mv_buf_index = -1;
		if (vdec->parallel_dec == 1) {
			pic_config->y_canvas_index = -1;
			pic_config->uv_canvas_index = -1;
		}
		pic_config->y_crop_width = pbi->init_pic_w;
		pic_config->y_crop_height = pbi->init_pic_h;
		pic_config->double_write_mode = get_double_write_mode(pbi);
	}
	for (; i < pbi->used_buf_num; i++) {
		pic_config = &cm->buffer_pool->frame_bufs[i].buf;
		pic_config->index = -1;
		pic_config->BUF_index = -1;
		pic_config->mv_buf_index = -1;
		if (vdec->parallel_dec == 1) {
			pic_config->y_canvas_index = -1;
			pic_config->uv_canvas_index = -1;
		}
	}
	pr_debug("%s ok, used_buf_num = %d\n", __func__, pbi->used_buf_num);
}

static void init_pic_list_hw(struct VP9Decoder_s *pbi)
{
	int i;
	struct VP9_Common_s *cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s *pic_config;
	WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR,
		(0x1 << 1) | (0x1 << 2));

	for (i = 0; i < pbi->used_buf_num; i++) {
		pic_config = &cm->buffer_pool->frame_bufs[i].buf;
		if (pic_config->index < 0)
			break;

		if (pbi->mmu_enable && ((pic_config->double_write_mode & 0x10) == 0)) {
			WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_DATA,
				pic_config->header_adr >> 5);
		} else {
			WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_DATA,
				pic_config->dw_y_adr >> 5);
		}
#ifndef LOSLESS_COMPRESS_MODE
		WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_DATA,
			pic_config->header_adr >> 5);
#else
		if (pic_config->double_write_mode & 0x10) {
				WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_DATA,
				pic_config->dw_u_v_adr >> 5);
		}
#endif
	}
	WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 0x1);

	/*Zero out canvas registers in IPP -- avoid simulation X*/
	WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR,
			(0 << 8) | (0 << 1) | 1);
	for (i = 0; i < 32; i++)
		WRITE_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR, 0);
}

static void dump_pic_list(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s *pic_config;
	int i;
	for (i = 0; i < FRAME_BUFFERS; i++) {
		pic_config = &cm->buffer_pool->frame_bufs[i].buf;
		vp9_print(pbi, 0,
			"Buf(%d) index %d mv_buf_index %d ref_count %d vf_ref %d dec_idx %d slice_type %d w/h %d/%d adr: %lx\n",
			i,
			pic_config->index,
#ifndef MV_USE_FIXED_BUF
			pic_config->mv_buf_index,
#else
			-1,
#endif
			cm->buffer_pool->
			frame_bufs[i].ref_count,
			pic_config->vf_ref,
			pic_config->decode_idx,
			pic_config->slice_type,
			pic_config->y_crop_width,
			pic_config->y_crop_height,
			pic_config->cma_alloc_addr
			);
	}
	return;
}

static int config_pic_size(struct VP9Decoder_s *pbi, unsigned short bit_depth)
{
#ifdef LOSLESS_COMPRESS_MODE
	unsigned int data32;
#endif
	int losless_comp_header_size, losless_comp_body_size;
	struct VP9_Common_s *cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s *cur_pic_config = &cm->cur_frame->buf;

	frame_width = cur_pic_config->y_crop_width;
	frame_height = cur_pic_config->y_crop_height;
	cur_pic_config->bit_depth = bit_depth;
	cur_pic_config->double_write_mode = get_double_write_mode(pbi);
	losless_comp_header_size =
		compute_losless_comp_header_size(cur_pic_config->y_crop_width,
		cur_pic_config->y_crop_height);
	losless_comp_body_size =
		compute_losless_comp_body_size(cur_pic_config->y_crop_width,
		cur_pic_config->y_crop_height, (bit_depth == VPX_BITS_10));
	cur_pic_config->comp_body_size = losless_comp_body_size;
#ifdef LOSLESS_COMPRESS_MODE
	data32 = READ_VREG(HEVC_SAO_CTRL5);
	if (bit_depth == VPX_BITS_10)
		data32 &= ~(1 << 9);
	else
		data32 |= (1 << 9);

	WRITE_VREG(HEVC_SAO_CTRL5, data32);

	if (pbi->mmu_enable) {
		/*bit[4] : paged_mem_mode*/
		WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, (0x1 << 4));
	} else {
	/*bit[3] smem mdoe*/
		if (bit_depth == VPX_BITS_10)
			WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, (0 << 3));
		else
			WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, (1 << 3));
	}
	if ((get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_SM1) ||
		(get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXHD2))
		WRITE_VREG(HEVCD_MPP_DECOMP_CTL2, (losless_comp_body_size >> 5));
	/*WRITE_VREG(HEVCD_MPP_DECOMP_CTL3,(0xff<<20) | (0xff<<10) | 0xff);*/
	WRITE_VREG(HEVC_CM_BODY_LENGTH, losless_comp_body_size);
	WRITE_VREG(HEVC_CM_HEADER_OFFSET, losless_comp_body_size);
	WRITE_VREG(HEVC_CM_HEADER_LENGTH, losless_comp_header_size);
	if (get_double_write_mode(pbi) & 0x10)
		WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, 0x1 << 31);
#else
	WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, 0x1 << 31);
#endif
	return 0;
}

static int config_mc_buffer(struct VP9Decoder_s *pbi, unsigned short bit_depth)
{
	int i;
	struct VP9_Common_s *cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s *cur_pic_config = &cm->cur_frame->buf;
	uint8_t scale_enable = 0;

	if (debug&VP9_DEBUG_BUFMGR_MORE)
		pr_info("config_mc_buffer entered .....\n");

	WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR,
			(0 << 8) | (0 << 1) | 1);
	for (i = 0; i < REFS_PER_FRAME; ++i) {
		struct PIC_BUFFER_CONFIG_s *pic_config = cm->frame_refs[i].buf;
		if (!pic_config)
			continue;
		WRITE_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR,
		(pic_config->mc_canvas_u_v << 16)
		| (pic_config->mc_canvas_u_v << 8)
		| pic_config->mc_canvas_y);
		if (debug & VP9_DEBUG_BUFMGR_MORE)
			pr_info("refid %x mc_canvas_u_v %x mc_canvas_y %x\n",
				i, pic_config->mc_canvas_u_v,
				pic_config->mc_canvas_y);
	}
	WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR,
			(16 << 8) | (0 << 1) | 1);
	for (i = 0; i < REFS_PER_FRAME; ++i) {
		struct PIC_BUFFER_CONFIG_s *pic_config = cm->frame_refs[i].buf;
		if (!pic_config)
			continue;
		WRITE_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR,
			(pic_config->mc_canvas_u_v << 16)
			| (pic_config->mc_canvas_u_v << 8)
			| pic_config->mc_canvas_y);
	}

	/*auto_inc start index:0 field:0*/
	WRITE_VREG(VP9D_MPP_REFINFO_TBL_ACCCONFIG, 0x1 << 2);
	/*index 0:last 1:golden 2:altref*/
	for (i = 0; i < REFS_PER_FRAME; i++) {
		int ref_pic_body_size;
		struct PIC_BUFFER_CONFIG_s *pic_config = cm->frame_refs[i].buf;
		if (!pic_config)
			continue;
		WRITE_VREG(VP9D_MPP_REFINFO_DATA, pic_config->y_crop_width);
		WRITE_VREG(VP9D_MPP_REFINFO_DATA, pic_config->y_crop_height);

	if (pic_config->y_crop_width != cur_pic_config->y_crop_width ||
		pic_config->y_crop_height != cur_pic_config->y_crop_height) {
		scale_enable |= (1 << i);
	}
	ref_pic_body_size =
		compute_losless_comp_body_size(pic_config->y_crop_width,
		pic_config->y_crop_height, (bit_depth == VPX_BITS_10));
	WRITE_VREG(VP9D_MPP_REFINFO_DATA,
		(pic_config->y_crop_width << 14)
		/ cur_pic_config->y_crop_width);
	WRITE_VREG(VP9D_MPP_REFINFO_DATA,
		(pic_config->y_crop_height << 14)
		/ cur_pic_config->y_crop_height);
	if (pbi->mmu_enable)
		WRITE_VREG(VP9D_MPP_REFINFO_DATA, 0);
	else
		WRITE_VREG(VP9D_MPP_REFINFO_DATA, ref_pic_body_size >> 5);
	}
	WRITE_VREG(VP9D_MPP_REF_SCALE_ENBL, scale_enable);
	return 0;
}

static void clear_mpred_hw(struct VP9Decoder_s *pbi)
{
	unsigned int data32;

	data32 = READ_VREG(HEVC_MPRED_CTRL4);
	data32 &=  (~(1 << 6));
	WRITE_VREG(HEVC_MPRED_CTRL4, data32);

	data32 = READ_VREG(HEVC_MPRED_CTRL0);
	data32 &= (~(3 << 9));
	WRITE_VREG(HEVC_MPRED_CTRL0, data32);
}

static void config_mpred_hw(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s *cur_pic_config = &cm->cur_frame->buf;
	struct PIC_BUFFER_CONFIG_s *last_frame_pic_config =
						&cm->prev_frame->buf;

	unsigned int data32;
	int     mpred_curr_lcu_x;
	int     mpred_curr_lcu_y;
	int     mpred_mv_rd_end_addr;

	mpred_mv_rd_end_addr = last_frame_pic_config->mpred_mv_wr_start_addr
		+ last_frame_pic_config->mv_size;

	data32 = READ_VREG(HEVC_MPRED_CURR_LCU);
	mpred_curr_lcu_x   = data32 & 0xffff;
	mpred_curr_lcu_y   = (data32 >> 16) & 0xffff;

	if (debug & VP9_DEBUG_BUFMGR)
		pr_info("cur pic_config index %d  col pic_config index %d\n",
			cur_pic_config->index, last_frame_pic_config->index);
	WRITE_VREG(HEVC_MPRED_CTRL3, 0x24122412);
	WRITE_VREG(HEVC_MPRED_ABV_START_ADDR,
			pbi->work_space_buf->mpred_above.buf_start);

	data32 = READ_VREG(HEVC_MPRED_CTRL4);

	data32 &=  (~(1 << 6));
	data32 |= (cm->use_prev_frame_mvs << 6);
	WRITE_VREG(HEVC_MPRED_CTRL4, data32);

	WRITE_VREG(HEVC_MPRED_MV_WR_START_ADDR,
			cur_pic_config->mpred_mv_wr_start_addr);
	WRITE_VREG(HEVC_MPRED_MV_WPTR, cur_pic_config->mpred_mv_wr_start_addr);

	WRITE_VREG(HEVC_MPRED_MV_RD_START_ADDR,
			last_frame_pic_config->mpred_mv_wr_start_addr);
	WRITE_VREG(HEVC_MPRED_MV_RPTR,
			last_frame_pic_config->mpred_mv_wr_start_addr);
	/*data32 = ((pbi->lcu_x_num - pbi->tile_width_lcu)*MV_MEM_UNIT);*/
	/*WRITE_VREG(HEVC_MPRED_MV_WR_ROW_JUMP,data32);*/
	/*WRITE_VREG(HEVC_MPRED_MV_RD_ROW_JUMP,data32);*/
	WRITE_VREG(HEVC_MPRED_MV_RD_END_ADDR, mpred_mv_rd_end_addr);

}

static void config_sao_hw(struct VP9Decoder_s *pbi, union param_u *params)
{
	struct VP9_Common_s *cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s *pic_config = &cm->cur_frame->buf;

	unsigned int data32;
	struct aml_vcodec_ctx * v4l2_ctx = pbi->v4l2_ctx;

	if (get_double_write_mode(pbi)) {
		WRITE_VREG(HEVC_SAO_Y_START_ADDR, pic_config->dw_y_adr);
		WRITE_VREG(HEVC_SAO_C_START_ADDR, pic_config->dw_u_v_adr);
		WRITE_VREG(HEVC_SAO_Y_WPTR, pic_config->dw_y_adr);
		WRITE_VREG(HEVC_SAO_C_WPTR, pic_config->dw_u_v_adr);
	} else {
		WRITE_VREG(HEVC_SAO_Y_START_ADDR, 0xffffffff);
		WRITE_VREG(HEVC_SAO_C_START_ADDR, 0xffffffff);
	}
	if (pbi->mmu_enable)
		WRITE_VREG(HEVC_CM_HEADER_START_ADDR, pic_config->header_adr);

	WRITE_VREG(HEVC_SAO_Y_LENGTH, pic_config->luma_size);
	WRITE_VREG(HEVC_SAO_C_LENGTH, pic_config->chroma_size);
	if (debug & PRINT_FLAG_V4L_DETAIL) {
		pr_info("[%d] config pic, id: %d, Y:(%x, %d) C:(%x, %d).\n",
			v4l2_ctx->id, pic_config->index,
			pic_config->dw_y_adr, pic_config->luma_size,
			pic_config->dw_u_v_adr, pic_config->chroma_size);
	}

#ifdef VP9_10B_NV21
#ifdef DOS_PROJECT
	data32 = READ_VREG(HEVC_SAO_CTRL1);
	data32 &= (~0x3000);
	/*[13:12] axi_aformat, 0-Linear, 1-32x32, 2-64x32*/
	data32 |= (pbi->mem_map_mode << 12);
	data32 &= (~0x3);
	data32 |= 0x1; /* [1]:dw_disable [0]:cm_disable*/
	WRITE_VREG(HEVC_SAO_CTRL1, data32);
	/*[23:22] dw_v1_ctrl [21:20] dw_v0_ctrl [19:18] dw_h1_ctrl
	 *	[17:16] dw_h0_ctrl
	 */
	data32 = READ_VREG(HEVC_SAO_CTRL5);
	/*set them all 0 for H265_NV21 (no down-scale)*/
	data32 &= ~(0xff << 16);
	WRITE_VREG(HEVC_SAO_CTRL5, data32);
	data32 = READ_VREG(HEVCD_IPP_AXIIF_CONFIG);
	data32 &= (~0x30);
	/*[5:4] address_format 00:linear 01:32x32 10:64x32*/
	data32 |= (pbi->mem_map_mode << 4);
	WRITE_VREG(HEVCD_IPP_AXIIF_CONFIG, data32);
#else
	/*m8baby test1902*/
	data32 = READ_VREG(HEVC_SAO_CTRL1);
	data32 &= (~0x3000);
	/*[13:12] axi_aformat, 0-Linear, 1-32x32, 2-64x32*/
	data32 |= (pbi->mem_map_mode << 12);
	data32 &= (~0xff0);
	/*data32 |= 0x670;*/ /*Big-Endian per 64-bit*/
	data32 |= 0x880;  /*.Big-Endian per 64-bit */
	data32 &= (~0x3);
	data32 |= 0x1; /*[1]:dw_disable [0]:cm_disable*/
	WRITE_VREG(HEVC_SAO_CTRL1, data32);
	/* [23:22] dw_v1_ctrl [21:20] dw_v0_ctrl
	 *[19:18] dw_h1_ctrl [17:16] dw_h0_ctrl
	 */
	data32 = READ_VREG(HEVC_SAO_CTRL5);
	/* set them all 0 for H265_NV21 (no down-scale)*/
	data32 &= ~(0xff << 16);
	WRITE_VREG(HEVC_SAO_CTRL5, data32);

	data32 = READ_VREG(HEVCD_IPP_AXIIF_CONFIG);
	data32 &= (~0x30);
	/*[5:4] address_format 00:linear 01:32x32 10:64x32*/
	data32 |= (pbi->mem_map_mode << 4);
	data32 &= (~0xF);
	data32 |= 0x8; /*Big-Endian per 64-bit*/
	WRITE_VREG(HEVCD_IPP_AXIIF_CONFIG, data32);
#endif
#else
	data32 = READ_VREG(HEVC_SAO_CTRL1);
	data32 &= (~(3 << 14));
	if (is_hevc_align32(pbi->mem_map_mode)) {
		data32 |= (1 << 14);
	} else {
		data32 |= (2 << 14); /* line align with 64*/
	}

	data32 &= (~0x3000);
	/* [13:12] axi_aformat, 0-Linear, 1-32x32, 2-64x32 */
	data32 |= (pbi->mem_map_mode << 12);

	data32 &= (~0xff0);
	data32 |= ((pbi->endian >> 8) & 0xfff);	/* Big-Endian per 64-bit */
	data32 &= (~0x3); 		/*[1]:dw_disable [0]:cm_disable*/
	if (get_double_write_mode(pbi) == 0)
		data32 |= 0x2; /*disable double write*/
	else if (get_double_write_mode(pbi) & 0x10)
		data32 |= 0x1; /*disable cm*/
	 if  (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) { /* >= G12A dw write control */
		unsigned int data;
		data = READ_VREG(HEVC_DBLK_CFGB);
		data &= (~0x300); /*[8]:first write enable (compress)  [9]:double write enable (uncompress)*/
		if (get_double_write_mode(pbi) == 0)
			data |= (0x1 << 8); /*enable first write*/
		else if (get_double_write_mode(pbi) & 0x10)
			data |= (0x1 << 9); /*double write only*/
		else
			data |= ((0x1 << 8)  |(0x1 << 9));
		WRITE_VREG(HEVC_DBLK_CFGB, data);
	}

	/* swap uv */
	if ((v4l2_ctx->q_data[AML_Q_DATA_DST].fmt->fourcc == V4L2_PIX_FMT_NV21) ||
		(v4l2_ctx->q_data[AML_Q_DATA_DST].fmt->fourcc == V4L2_PIX_FMT_NV21M))
		data32 &= ~(1 << 8); /* NV21 */
	else
		data32 |= (1 << 8); /* NV12 */

	/*
	*  [31:24] ar_fifo1_axi_thread
	*  [23:16] ar_fifo0_axi_thread
	*  [15:14] axi_linealign, 0-16bytes, 1-32bytes, 2-64bytes
	*  [13:12] axi_aformat, 0-Linear, 1-32x32, 2-64x32
	*  [11:08] axi_lendian_C
	*  [07:04] axi_lendian_Y
	*  [3]     reserved
	*  [2]     clk_forceon
	*  [1]     dw_disable:disable double write output
	*  [0]     cm_disable:disable compress output
	*/
	WRITE_VREG(HEVC_SAO_CTRL1, data32);

	if (get_double_write_mode(pbi) & 0x10) {
		/* [23:22] dw_v1_ctrl
		 *[21:20] dw_v0_ctrl
		 *[19:18] dw_h1_ctrl
		 *[17:16] dw_h0_ctrl
		 */
		data32 = READ_VREG(HEVC_SAO_CTRL5);
		/*set them all 0 for H265_NV21 (no down-scale)*/
		data32 &= ~(0xff << 16);
		WRITE_VREG(HEVC_SAO_CTRL5, data32);
	} else {
		if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_T7) &&
			(get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_TXHD2))
			WRITE_VREG(HEVC_SAO_CTRL26, 0);

		data32 = READ_VREG(HEVC_SAO_CTRL5);
		data32 &= (~(0xff << 16));
		if (get_double_write_mode(pbi) == 8 ||
			get_double_write_mode(pbi) == 9) {
			data32 |= (0xff << 16);
			WRITE_VREG(HEVC_SAO_CTRL26, 0xf);
		} else if (get_double_write_mode(pbi) == 2 ||
			get_double_write_mode(pbi) == 3)
			data32 |= (0xff << 16);
		else if (get_double_write_mode(pbi) == 4)
			data32 |= (0x33 << 16);
		WRITE_VREG(HEVC_SAO_CTRL5, data32);
	}

	data32 = READ_VREG(HEVCD_IPP_AXIIF_CONFIG);
	data32 &= (~0x30);
	/* [5:4]    -- address_format 00:linear 01:32x32 10:64x32 */
	data32 |= (pbi->mem_map_mode << 4);
	data32 &= (~0xf);
	data32 |= (pbi->endian & 0xf);  /* valid only when double write only */

	/* swap uv */
	if ((v4l2_ctx->q_data[AML_Q_DATA_DST].fmt->fourcc == V4L2_PIX_FMT_NV21) ||
		(v4l2_ctx->q_data[AML_Q_DATA_DST].fmt->fourcc == V4L2_PIX_FMT_NV21M))
		data32 |= (1 << 12); /* NV21 */
	else
		data32 &= ~(1 << 12); /* NV12 */

	data32 &= (~(3 << 8));
	if (is_hevc_align32(pbi->mem_map_mode)) {
		data32 |= (1 << 8);
	} else {
		data32 |= (2 << 8); /* line align with 64 for dw only */
	}
	/*
	* [3:0]   little_endian
	* [5:4]   address_format 00:linear 01:32x32 10:64x32
	* [7:6]   reserved
	* [9:8]   Linear_LineAlignment 00:16byte 01:32byte 10:64byte
	* [11:10] reserved
	* [12]    CbCr_byte_swap
	* [31:13] reserved
	*/
	WRITE_VREG(HEVCD_IPP_AXIIF_CONFIG, data32);
#endif
}

static void vp9_config_work_space_hw(struct VP9Decoder_s *pbi, u32 mask)
{
	struct BuffInfo_s *buf_spec = pbi->work_space_buf;
	unsigned int data32;
	int losless_comp_header_size, losless_comp_body_size;

	if (debug && pbi->init_flag == 0)
		pr_info("%s w %d h %d %x %x %x %x %x %x %x %x %x %x %x %x\n",
			__func__,
			buf_spec->max_width,
			buf_spec->max_height,
			buf_spec->ipp.buf_start,
			buf_spec->start_adr,
			buf_spec->short_term_rps.buf_start,
			buf_spec->vps.buf_start,
			buf_spec->sps.buf_start,
			buf_spec->pps.buf_start,
			buf_spec->sao_up.buf_start,
			buf_spec->swap_buf.buf_start,
			buf_spec->swap_buf2.buf_start,
			buf_spec->scalelut.buf_start,
			buf_spec->dblk_para.buf_start,
			buf_spec->dblk_data.buf_start);

	if (mask & HW_MASK_FRONT) {
		if ((debug & VP9_DEBUG_SEND_PARAM_WITH_REG) == 0)
			WRITE_VREG(HEVC_RPM_BUFFER, (u32)pbi->rpm_phy_addr);

		WRITE_VREG(HEVC_SHORT_TERM_RPS,
			buf_spec->short_term_rps.buf_start);
		WRITE_VREG(HEVC_PPS_BUFFER, buf_spec->pps.buf_start);
		WRITE_VREG(HEVC_STREAM_SWAP_BUFFER,
			buf_spec->swap_buf.buf_start);
		WRITE_VREG(HEVC_STREAM_SWAP_BUFFER2,
			buf_spec->swap_buf2.buf_start);
		WRITE_VREG(LMEM_DUMP_ADR, (u32)pbi->lmem_phy_addr);
	}

	if (mask & HW_MASK_BACK) {
#ifdef LOSLESS_COMPRESS_MODE
		losless_comp_header_size =
			compute_losless_comp_header_size(pbi->init_pic_w,
			pbi->init_pic_h);
		losless_comp_body_size =
			compute_losless_comp_body_size(pbi->init_pic_w,
			pbi->init_pic_h, buf_alloc_depth == 10);
#endif
		WRITE_VREG(HEVCD_IPP_LINEBUFF_BASE,
			buf_spec->ipp.buf_start);
		if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) {
			/* cfg_addr_adp*/
			WRITE_VREG(HEVC_DBLK_CFGE, buf_spec->dblk_para.buf_start);
			if (debug & VP9_DEBUG_BUFMGR_MORE)
				pr_info("Write HEVC_DBLK_CFGE\n");
		}
		/* cfg_p_addr */
		WRITE_VREG(HEVC_DBLK_CFG4, buf_spec->dblk_para.buf_start);

		/* cfg_d_addr */
		WRITE_VREG(HEVC_DBLK_CFG5, buf_spec->dblk_data.buf_start);

		if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1) &&
			(get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_TXHD2)) {
			if (buf_spec->max_width <= 4096 && buf_spec->max_height <= 2304)
				WRITE_VREG(HEVC_DBLK_CFG3, 0x404010); //default value
			else
				WRITE_VREG(HEVC_DBLK_CFG3, 0x808020); // make left storage 2 x 4k]
			vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE,
				"HEVC_DBLK_CFG3 = %x\n", READ_VREG(HEVC_DBLK_CFG3));
		}
#ifdef LOSLESS_COMPRESS_MODE
		if (pbi->mmu_enable) {
			/*bit[4] : paged_mem_mode*/
			WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, (0x1 << 4));
			if ((get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_SM1) ||
				(get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXHD2))
				WRITE_VREG(HEVCD_MPP_DECOMP_CTL2, 0);
		} else {
			/*bit[3] smem mdoe*/
			/*else WRITE_VREG(P_HEVCD_MPP_DECOMP_CTL1, (1<<3));*/
			/*bit[3] smem mdoe*/
			WRITE_VREG(HEVCD_MPP_DECOMP_CTL2,
				(losless_comp_body_size >> 5));
		}
		/*8-bit mode */
		WRITE_VREG(HEVC_CM_BODY_LENGTH, losless_comp_body_size);
		WRITE_VREG(HEVC_CM_HEADER_OFFSET, losless_comp_body_size);
		WRITE_VREG(HEVC_CM_HEADER_LENGTH, losless_comp_header_size);

		if (get_double_write_mode(pbi) & 0x10)
			WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, 0x1 << 31);
#else
		WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, 0x1 << 31);
#endif

		if (pbi->mmu_enable) {
			WRITE_VREG(HEVC_SAO_MMU_VH0_ADDR, buf_spec->mmu_vbh.buf_start);
			WRITE_VREG(HEVC_SAO_MMU_VH1_ADDR, buf_spec->mmu_vbh.buf_start
					+ VBH_BUF_SIZE(buf_spec));

			/* use HEVC_CM_HEADER_START_ADDR */
			data32 = READ_VREG(HEVC_SAO_CTRL5);
			data32 |= (1<<10);
			WRITE_VREG(HEVC_SAO_CTRL5, data32);
		}

		/* config mpred axi burst threshold */
		WRITE_VREG(HEVC_MPRED_CTRL3, 0x24122412);

#ifdef CO_MV_COMPRESS
		if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_T7) &&
			(get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_TXHD2)) {
			data32 = READ_VREG(HEVC_MPRED_CTRL4);
			data32 |=  (1 << 1);
			WRITE_VREG(HEVC_MPRED_CTRL4, data32);
		}
#endif
		WRITE_VREG(VP9_SEG_MAP_BUFFER, buf_spec->seg_map.buf_start);

		WRITE_VREG(LMEM_DUMP_ADR, (u32)pbi->lmem_phy_addr);
		WRITE_VREG(VP9_PROB_SWAP_BUFFER, pbi->prob_buffer_phy_addr);
		WRITE_VREG(VP9_COUNT_SWAP_BUFFER, pbi->count_buffer_phy_addr);
		if (pbi->mmu_enable) {
			if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A)
				WRITE_VREG(HEVC_ASSIST_MMU_MAP_ADDR, pbi->frame_mmu_map_phy_addr);
			else
				WRITE_VREG(VP9_MMU_MAP_BUFFER, pbi->frame_mmu_map_phy_addr);
		}
	}
}


#ifdef VP9_LPF_LVL_UPDATE
/*
 * Defines, declarations, sub-functions for vp9 de-block loop
	filter Thr/Lvl table update
 * - struct segmentation is for loop filter only (removed something)
 * - function "vp9_loop_filter_init" and "vp9_loop_filter_frame_init" will
	be instantiated in C_Entry
 * - vp9_loop_filter_init run once before decoding start
 * - vp9_loop_filter_frame_init run before every frame decoding start
 * - set video format to VP9 is in vp9_loop_filter_init
 */
#define MAX_LOOP_FILTER 63
#define MAX_REF_LF_DELTAS 4
#define MAX_MODE_LF_DELTAS 2
#define SEGMENT_DELTADATA   0
#define SEGMENT_ABSDATA     1
#define MAX_SEGMENTS     8
/*.#define SEG_TREE_PROBS   (MAX_SEGMENTS-1)*/
/*no use for loop filter, if this struct for common use, pls add it back*/
/*#define PREDICTION_PROBS 3*/
/* no use for loop filter, if this struct for common use, pls add it back*/

enum SEG_LVL_FEATURES {
	SEG_LVL_ALT_Q = 0, /*Use alternate Quantizer ....*/
	SEG_LVL_ALT_LF = 1, /*Use alternate loop filter value...*/
	SEG_LVL_REF_FRAME = 2, /*Optional Segment reference frame*/
	SEG_LVL_SKIP = 3,  /*Optional Segment (0,0) + skip mode*/
	SEG_LVL_MAX = 4    /*Number of features supported*/
};

struct segmentation {
	uint8_t enabled;
	uint8_t update_map;
	uint8_t update_data;
	uint8_t abs_delta;
	uint8_t temporal_update;

	/*no use for loop filter, if this struct
	 *for common use, pls add it back
	 */
	/*vp9_prob tree_probs[SEG_TREE_PROBS]; */
	/* no use for loop filter, if this struct
	 *	for common use, pls add it back
	 */
	/*vp9_prob pred_probs[PREDICTION_PROBS];*/

	int16_t feature_data[MAX_SEGMENTS][SEG_LVL_MAX];
	unsigned int feature_mask[MAX_SEGMENTS];
};

struct loop_filter_thresh {
	uint8_t mblim;
	uint8_t lim;
	uint8_t hev_thr;
};

struct loop_filter_info_n {
	struct loop_filter_thresh lfthr[MAX_LOOP_FILTER + 1];
	uint8_t lvl[MAX_SEGMENTS][MAX_REF_FRAMES][MAX_MODE_LF_DELTAS];
};

struct loopfilter {
	int filter_level;

	int sharpness_level;
	int last_sharpness_level;

	uint8_t mode_ref_delta_enabled;
	uint8_t mode_ref_delta_update;

	/*0 = Intra, Last, GF, ARF*/
	signed char ref_deltas[MAX_REF_LF_DELTAS];
	signed char last_ref_deltas[MAX_REF_LF_DELTAS];

	/*0 = ZERO_MV, MV*/
	signed char mode_deltas[MAX_MODE_LF_DELTAS];
	signed char last_mode_deltas[MAX_MODE_LF_DELTAS];
};

static int vp9_clamp(int value, int low, int high)
{
	return value < low ? low : (value > high ? high : value);
}

int segfeature_active(struct segmentation *seg, int segment_id,
			enum SEG_LVL_FEATURES feature_id) {
	return seg->enabled &&
		(seg->feature_mask[segment_id] & (1 << feature_id));
}

int get_segdata(struct segmentation *seg, int segment_id,
				enum SEG_LVL_FEATURES feature_id) {
	return seg->feature_data[segment_id][feature_id];
}

static void vp9_update_sharpness(struct loop_filter_info_n *lfi,
					int sharpness_lvl)
{
	int lvl;
	/*For each possible value for the loop filter fill out limits*/
	for (lvl = 0; lvl <= MAX_LOOP_FILTER; lvl++) {
		/*Set loop filter parameters that control sharpness.*/
		int block_inside_limit = lvl >> ((sharpness_lvl > 0) +
					(sharpness_lvl > 4));

		if (sharpness_lvl > 0) {
			if (block_inside_limit > (9 - sharpness_lvl))
				block_inside_limit = (9 - sharpness_lvl);
		}

		if (block_inside_limit < 1)
			block_inside_limit = 1;

		lfi->lfthr[lvl].lim = (uint8_t)block_inside_limit;
		lfi->lfthr[lvl].mblim = (uint8_t)(2 * (lvl + 2) +
				block_inside_limit);
	}
}

/*instantiate this function once when decode is started*/
void vp9_loop_filter_init(struct VP9Decoder_s *pbi)
{
	struct loop_filter_info_n *lfi = pbi->lfi;
	struct loopfilter *lf = pbi->lf;
	struct segmentation *seg_4lf = pbi->seg_4lf;
	int i;
	unsigned int data32;

	memset(lfi, 0, sizeof(struct loop_filter_info_n));
	memset(lf, 0, sizeof(struct loopfilter));
	memset(seg_4lf, 0, sizeof(struct segmentation));
	lf->sharpness_level = 0; /*init to 0 */
	/*init limits for given sharpness*/
	vp9_update_sharpness(lfi, lf->sharpness_level);
	lf->last_sharpness_level = lf->sharpness_level;

	/*Write to register*/
	for (i = 0; i < 32; i++) {
		unsigned int thr;

		thr = ((lfi->lfthr[i * 2 + 1].lim & 0x3f)<<8) |
			(lfi->lfthr[i * 2 + 1].mblim & 0xff);
		thr = (thr<<16) | ((lfi->lfthr[i*2].lim & 0x3f)<<8) |
			(lfi->lfthr[i * 2].mblim & 0xff);
		WRITE_VREG(HEVC_DBLK_CFG9, thr);
	}

	/*video format is VP9*/
	if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1) &&
		(get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_TXHD2)) {
		data32 = (0x3 << 14) | // (dw fifo thres r and b)
		(0x3 << 12) | // (dw fifo thres r or b)
		(0x3 << 10) | // (dw fifo thres not r/b)
		(0x3 << 8) | // 1st/2nd write both enable
		(0x1 << 0); // vp9 video format
		if (get_double_write_mode(pbi) == 0x10)
			 data32 &= (~0x100);
	} else if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) {
		data32 = (0x57 << 8) |  /*1st/2nd write both enable*/
			(0x1  << 0); /*vp9 video format*/
		if (get_double_write_mode(pbi) == 0x10)
			 data32 &= (~0x100);
	} else
		data32 = 0x40400001;

	WRITE_VREG(HEVC_DBLK_CFGB, data32);
	if (debug & VP9_DEBUG_BUFMGR_MORE)
		pr_info("[DBLK DEBUG] CFGB : 0x%x\n", data32);
}
	/* perform this function per frame*/
void vp9_loop_filter_frame_init(struct segmentation *seg,
		struct loop_filter_info_n *lfi, struct loopfilter *lf,
		int default_filt_lvl) {
	int i;
	int seg_id;
	/*n_shift is the multiplier for lf_deltas
	 *the multiplier is 1 for when filter_lvl is between 0 and 31;
	 *2 when filter_lvl is between 32 and 63
	 */
	const int scale = 1 << (default_filt_lvl >> 5);

	/*update limits if sharpness has changed*/
	if (lf->last_sharpness_level != lf->sharpness_level) {
		vp9_update_sharpness(lfi, lf->sharpness_level);
		lf->last_sharpness_level = lf->sharpness_level;

	/*Write to register*/
	for (i = 0; i < 32; i++) {
		unsigned int thr;

		thr = ((lfi->lfthr[i * 2 + 1].lim & 0x3f) << 8)
			| (lfi->lfthr[i * 2 + 1].mblim & 0xff);
		thr = (thr << 16) | ((lfi->lfthr[i * 2].lim & 0x3f) << 8)
			| (lfi->lfthr[i * 2].mblim & 0xff);
		WRITE_VREG(HEVC_DBLK_CFG9, thr);
		}
	}

	for (seg_id = 0; seg_id < MAX_SEGMENTS; seg_id++) {/*MAX_SEGMENTS = 8*/
		int lvl_seg = default_filt_lvl;

		if (segfeature_active(seg, seg_id, SEG_LVL_ALT_LF)) {
			const int data = get_segdata(seg, seg_id,
						SEG_LVL_ALT_LF);
			lvl_seg = vp9_clamp(seg->abs_delta == SEGMENT_ABSDATA ?
				data : default_filt_lvl + data,
				0, MAX_LOOP_FILTER);
#ifdef DBG_LF_PRINT
	pr_info("segfeature_active!!!seg_id=%d,lvl_seg=%d\n", seg_id, lvl_seg);
#endif
	}

	if (!lf->mode_ref_delta_enabled) {
		/*we could get rid of this if we assume that deltas are set to
		 *zero when not in use; encoder always uses deltas
		 */
		memset(lfi->lvl[seg_id], lvl_seg, sizeof(lfi->lvl[seg_id]));
	} else {
		int ref, mode;
		const int intra_lvl = lvl_seg +	lf->ref_deltas[INTRA_FRAME]
					* scale;
#ifdef DBG_LF_PRINT
		pr_info("LF_PRINT:vp9_loop_filter_frame_init,seg_id=%d\n", seg_id);
		pr_info("ref_deltas[INTRA_FRAME]=%d\n", lf->ref_deltas[INTRA_FRAME]);
#endif
		lfi->lvl[seg_id][INTRA_FRAME][0] =
				vp9_clamp(intra_lvl, 0, MAX_LOOP_FILTER);

		for (ref = LAST_FRAME; ref < MAX_REF_FRAMES; ++ref) {
			/* LAST_FRAME = 1, MAX_REF_FRAMES = 4*/
			for (mode = 0; mode < MAX_MODE_LF_DELTAS; ++mode) {
				/*MAX_MODE_LF_DELTAS = 2*/
				const int inter_lvl =
					lvl_seg + lf->ref_deltas[ref] * scale
					+ lf->mode_deltas[mode] * scale;
				lfi->lvl[seg_id][ref][mode] =
					vp9_clamp(inter_lvl, 0,
					MAX_LOOP_FILTER);
				}
			}
		}
	}

#ifdef DBG_LF_PRINT
	/*print out thr/lvl table per frame*/
	for (i = 0; i <= MAX_LOOP_FILTER; i++) {
		pr_info("LF_PRINT:(%d)thr=%d,blim=%d,lim=%d\n",
			i, lfi->lfthr[i].hev_thr, lfi->lfthr[i].mblim,
			lfi->lfthr[i].lim);
	}
	for (seg_id = 0; seg_id < MAX_SEGMENTS; seg_id++) {
		pr_info("LF_PRINT:lvl(seg_id=%d)(mode=0,%d,%d,%d,%d)\n",
			seg_id, lfi->lvl[seg_id][0][0],
			lfi->lvl[seg_id][1][0], lfi->lvl[seg_id][2][0],
			lfi->lvl[seg_id][3][0]);
		pr_info("i(mode=1,%d,%d,%d,%d)\n", lfi->lvl[seg_id][0][1],
			lfi->lvl[seg_id][1][1], lfi->lvl[seg_id][2][1],
			lfi->lvl[seg_id][3][1]);
	}
#endif

	/*Write to register */
	for (i = 0; i < 16; i++) {
		unsigned int level;

		level = ((lfi->lvl[i >> 1][3][i & 1] & 0x3f) << 24) |
			((lfi->lvl[i >> 1][2][i & 1] & 0x3f) << 16) |
			((lfi->lvl[i >> 1][1][i & 1] & 0x3f) << 8) |
			(lfi->lvl[i >> 1][0][i & 1] & 0x3f);
		if (!default_filt_lvl)
			level = 0;
		WRITE_VREG(HEVC_DBLK_CFGA, level);
	}
}
/* VP9_LPF_LVL_UPDATE */
#endif

static void vp9_init_decoder_hw(struct VP9Decoder_s *pbi, u32 mask)
{
	unsigned int data32;
	int i;
	const unsigned short parser_cmd[PARSER_CMD_NUMBER] = {
		0x0401, 0x8401, 0x0800, 0x0402, 0x9002, 0x1423,
		0x8CC3, 0x1423, 0x8804, 0x9825, 0x0800, 0x04FE,
		0x8406, 0x8411, 0x1800, 0x8408, 0x8409, 0x8C2A,
		0x9C2B, 0x1C00, 0x840F, 0x8407, 0x8000, 0x8408,
		0x2000, 0xA800, 0x8410, 0x04DE, 0x840C, 0x840D,
		0xAC00, 0xA000, 0x08C0, 0x08E0, 0xA40E, 0xFC00,
		0x7C00
	};

	if (mask & HW_MASK_FRONT) {
		data32 = READ_VREG(HEVC_PARSER_INT_CONTROL);
		/* set bit 31~29 to 3 if HEVC_STREAM_FIFO_CTL[29] is 1 */
		data32 &= ~(7 << 29);
		data32 |= (3 << 29);
		data32 = data32 |
		(1 << 24) |/*stream_buffer_empty_int_amrisc_enable*/
		(1 << 22) |/*stream_fifo_empty_int_amrisc_enable*/
		(1 << 7) |/*dec_done_int_cpu_enable*/
		(1 << 4) |/*startcode_found_int_cpu_enable*/
		(0 << 3) |/*startcode_found_int_amrisc_enable*/
		(1 << 0)    /*parser_int_enable*/
		;
#ifdef SUPPORT_FB_DECODING
#ifndef FB_DECODING_TEST_SCHEDULE
		/*fed_fb_slice_done_int_cpu_enable*/
		if (pbi->used_stage_buf_num > 0)
			data32 |= (1 << 10);
#endif
#endif
		WRITE_VREG(HEVC_PARSER_INT_CONTROL, data32);

		data32 = READ_VREG(HEVC_SHIFT_STATUS);
		data32 = data32 |
			(0 << 1) |/*emulation_check_off VP9 do not have emulation*/
			(1 << 0)/*startcode_check_on*/
			;
		WRITE_VREG(HEVC_SHIFT_STATUS, data32);
		WRITE_VREG(HEVC_SHIFT_CONTROL,
			(0 << 14) | /*disable_start_code_protect*/
			(1 << 10) | /*length_zero_startcode_en for VP9*/
			(1 << 9) | /*length_valid_startcode_en for VP9*/
			(3 << 6) | /*sft_valid_wr_position*/
			(2 << 4) | /*emulate_code_length_sub_1*/
			(3 << 1) | /*start_code_length_sub_1
			VP9 use 0x00000001 as startcode (4 Bytes)*/
			(1 << 0)   /*stream_shift_enable*/
			);

		WRITE_VREG(HEVC_CABAC_CONTROL,
			(1 << 0)/*cabac_enable*/
			);

		WRITE_VREG(HEVC_PARSER_CORE_CONTROL,
			(1 << 0)/* hevc_parser_core_clk_en*/
			);


		WRITE_VREG(HEVC_DEC_STATUS_REG, 0);
	}

	if (mask & HW_MASK_BACK) {
		/*Initial IQIT_SCALELUT memory
		-- just to avoid X in simulation*/
		if (is_rdma_enable())
			rdma_back_end_work(pbi->rdma_phy_adr, RDMA_SIZE);
		else {
			WRITE_VREG(HEVC_IQIT_SCALELUT_WR_ADDR, 0);/*cfg_p_addr*/
			for (i = 0; i < 1024; i++)
				WRITE_VREG(HEVC_IQIT_SCALELUT_DATA, 0);
		}
	}

	if (mask & HW_MASK_FRONT) {
		u32 decode_mode;
#ifdef ENABLE_SWAP_TEST
		WRITE_VREG(HEVC_STREAM_SWAP_TEST, 100);
#else
		WRITE_VREG(HEVC_STREAM_SWAP_TEST, 0);
#endif
#ifdef MULTI_INSTANCE_SUPPORT
		if (!pbi->m_ins_flag) {
			if (pbi->low_latency_flag)
				decode_mode = DECODE_MODE_SINGLE_LOW_LATENCY;
			else
				decode_mode = DECODE_MODE_SINGLE;
		} else if (vdec_frame_based(hw_to_vdec(pbi)))
			decode_mode = pbi->no_head ?
				DECODE_MODE_MULTI_FRAMEBASE_NOHEAD :
				DECODE_MODE_MULTI_FRAMEBASE;
		else
			decode_mode = DECODE_MODE_MULTI_STREAMBASE;
#ifdef SUPPORT_FB_DECODING
#ifndef FB_DECODING_TEST_SCHEDULE
		if (pbi->used_stage_buf_num > 0)
			decode_mode |= (0x01 << 24);
#endif
#endif
		WRITE_VREG(DECODE_MODE, decode_mode);
		WRITE_VREG(HEVC_DECODE_SIZE, 0);
		WRITE_VREG(HEVC_DECODE_COUNT, 0);
#else
		WRITE_VREG(DECODE_MODE, DECODE_MODE_SINGLE);
		WRITE_VREG(HEVC_DECODE_PIC_BEGIN_REG, 0);
		WRITE_VREG(HEVC_DECODE_PIC_NUM_REG, 0x7fffffff); /*to remove*/
#endif
		/*Send parser_cmd*/
		WRITE_VREG(HEVC_PARSER_CMD_WRITE, (1 << 16) | (0 << 0));
		for (i = 0; i < PARSER_CMD_NUMBER; i++)
			WRITE_VREG(HEVC_PARSER_CMD_WRITE, parser_cmd[i]);
		WRITE_VREG(HEVC_PARSER_CMD_SKIP_0, PARSER_CMD_SKIP_CFG_0);
		WRITE_VREG(HEVC_PARSER_CMD_SKIP_1, PARSER_CMD_SKIP_CFG_1);
		WRITE_VREG(HEVC_PARSER_CMD_SKIP_2, PARSER_CMD_SKIP_CFG_2);


		WRITE_VREG(HEVC_PARSER_IF_CONTROL,
			/*  (1 << 8) |*/ /*sao_sw_pred_enable*/
			(1 << 5) | /*parser_sao_if_en*/
			(1 << 2) | /*parser_mpred_if_en*/
			(1 << 0) /*parser_scaler_if_en*/
			);
	}

	if (mask & HW_MASK_BACK) {
		WRITE_VREG(HEVCD_IPP_TOP_CNTL,
			(0 << 1) | /*enable ipp*/
			(1 << 0)   /*software reset ipp and mpp*/
			);
		WRITE_VREG(HEVCD_IPP_TOP_CNTL,
			(1 << 1) | /*enable ipp*/
			(0 << 0)   /*software reset ipp and mpp*/
			);
		if (get_double_write_mode(pbi) & 0x10) {
			/*Enable NV21 reference read mode for MC*/
			WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, 0x1 << 31);
		}

		/*Initialize mcrcc and decomp perf counters*/
		if (mcrcc_cache_alg_flag &&
			pbi->init_flag == 0) {
			mcrcc_perfcount_reset();
			decomp_perfcount_reset();
		}
	}
	return;
}


#ifdef CONFIG_HEVC_CLK_FORCED_ON
static void config_vp9_clk_forced_on(void)
{
	unsigned int rdata32;
	/*IQIT*/
	rdata32 = READ_VREG(HEVC_IQIT_CLK_RST_CTRL);
	WRITE_VREG(HEVC_IQIT_CLK_RST_CTRL, rdata32 | (0x1 << 2));

	/* DBLK*/
	rdata32 = READ_VREG(HEVC_DBLK_CFG0);
	WRITE_VREG(HEVC_DBLK_CFG0, rdata32 | (0x1 << 2));

	/* SAO*/
	rdata32 = READ_VREG(HEVC_SAO_CTRL1);
	WRITE_VREG(HEVC_SAO_CTRL1, rdata32 | (0x1 << 2));

	/*MPRED*/
	rdata32 = READ_VREG(HEVC_MPRED_CTRL1);
	WRITE_VREG(HEVC_MPRED_CTRL1, rdata32 | (0x1 << 24));

	/* PARSER*/
	rdata32 = READ_VREG(HEVC_STREAM_CONTROL);
	WRITE_VREG(HEVC_STREAM_CONTROL, rdata32 | (0x1 << 15));
	rdata32 = READ_VREG(HEVC_SHIFT_CONTROL);
	WRITE_VREG(HEVC_SHIFT_CONTROL, rdata32 | (0x1 << 15));
	rdata32 = READ_VREG(HEVC_CABAC_CONTROL);
	WRITE_VREG(HEVC_CABAC_CONTROL, rdata32 | (0x1 << 13));
	rdata32 = READ_VREG(HEVC_PARSER_CORE_CONTROL);
	WRITE_VREG(HEVC_PARSER_CORE_CONTROL, rdata32 | (0x1 << 15));
	rdata32 = READ_VREG(HEVC_PARSER_INT_CONTROL);
	WRITE_VREG(HEVC_PARSER_INT_CONTROL, rdata32 | (0x1 << 15));
	rdata32 = READ_VREG(HEVC_PARSER_IF_CONTROL);
	WRITE_VREG(HEVC_PARSER_IF_CONTROL,
			rdata32 | (0x1 << 6) | (0x1 << 3) | (0x1 << 1));

	/*IPP*/
	rdata32 = READ_VREG(HEVCD_IPP_DYNCLKGATE_CONFIG);
	WRITE_VREG(HEVCD_IPP_DYNCLKGATE_CONFIG, rdata32 | 0xffffffff);

	/* MCRCC*/
	rdata32 = READ_VREG(HEVCD_MCRCC_CTL1);
	WRITE_VREG(HEVCD_MCRCC_CTL1, rdata32 | (0x1 << 3));
}
#endif


#ifdef MCRCC_ENABLE
static void dump_hit_rate(struct VP9Decoder_s *pbi)
{
	if (debug & VP9_DEBUG_CACHE_HIT_RATE) {
		mcrcc_get_hitrate(pbi->m_ins_flag);
		decomp_get_hitrate();
		decomp_get_comp_rate();
	}
}

static void  config_mcrcc_axi_hw(struct VP9Decoder_s *pbi)
{
	unsigned int rdata32;
	unsigned short is_inter;
	WRITE_VREG(HEVCD_MCRCC_CTL1, 0x2);/* reset mcrcc*/
	is_inter = ((pbi->common.frame_type != KEY_FRAME) &&
			(!pbi->common.intra_only)) ? 1 : 0;
	if (!is_inter) { /* I-PIC*/
		/*remove reset -- disables clock*/
		WRITE_VREG(HEVCD_MCRCC_CTL1, 0x0);
		return;
	}

	if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1) &&
		(get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_TXHD2)) {
		mcrcc_get_hitrate(pbi->m_ins_flag);
		decomp_get_hitrate();
		decomp_get_comp_rate();
	}

	WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR,
		(0 << 8) | (1 << 1) | 0);
	rdata32 = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
	rdata32 = rdata32 & 0xffff;
	rdata32 = rdata32 | (rdata32 << 16);
	WRITE_VREG(HEVCD_MCRCC_CTL2, rdata32);
	/*Programme canvas1 */
	rdata32 = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
	rdata32 = rdata32 & 0xffff;
	rdata32 = rdata32 | (rdata32 << 16);
	WRITE_VREG(HEVCD_MCRCC_CTL3, rdata32);
	/*enable mcrcc progressive-mode*/
	WRITE_VREG(HEVCD_MCRCC_CTL1, 0xff0);
}

static void  config_mcrcc_axi_hw_new(struct VP9Decoder_s *pbi)
{
	u32 curr_picnum = -1;
	u32 lastref_picnum = -1;
	u32 goldenref_picnum = -1;
	u32 altref_picnum = -1;

	u32 lastref_delta_picnum;
	u32 goldenref_delta_picnum;
	u32 altref_delta_picnum;

	u32 rdata32;

	u32 lastcanvas;
	u32 goldencanvas;
	u32 altrefcanvas;

	u16 is_inter;
	u16 lastref_inref;
	u16 goldenref_inref;
	u16 altref_inref;

	u32 refcanvas_array[3], utmp;
	int deltapicnum_array[3], tmp;

	struct VP9_Common_s *cm = &pbi->common;
	struct PIC_BUFFER_CONFIG_s *cur_pic_config
		= &cm->cur_frame->buf;
	curr_picnum = cur_pic_config->decode_idx;
	if (cm->frame_refs[0].buf)
		lastref_picnum = cm->frame_refs[0].buf->decode_idx;
	if (cm->frame_refs[1].buf)
		goldenref_picnum = cm->frame_refs[1].buf->decode_idx;
	if (cm->frame_refs[2].buf)
		altref_picnum = cm->frame_refs[2].buf->decode_idx;

	lastref_delta_picnum = (lastref_picnum >= curr_picnum) ?
		(lastref_picnum - curr_picnum) : (curr_picnum - lastref_picnum);
	goldenref_delta_picnum = (goldenref_picnum >= curr_picnum) ?
		(goldenref_picnum - curr_picnum) :
		(curr_picnum - goldenref_picnum);
	altref_delta_picnum =
		(altref_picnum >= curr_picnum) ?
		(altref_picnum - curr_picnum) : (curr_picnum - altref_picnum);

	lastref_inref = (cm->frame_refs[0].idx != INVALID_IDX) ? 1 : 0;
	goldenref_inref = (cm->frame_refs[1].idx != INVALID_IDX) ? 1 : 0;
	altref_inref = (cm->frame_refs[2].idx != INVALID_IDX) ? 1 : 0;

	if (debug & VP9_DEBUG_CACHE)
		pr_info("%s--0--lastref_inref:%d goldenref_inref:%d altref_inref:%d\n",
			__func__, lastref_inref, goldenref_inref, altref_inref);

	WRITE_VREG(HEVCD_MCRCC_CTL1, 0x2); /* reset mcrcc */

	is_inter = ((pbi->common.frame_type != KEY_FRAME)
		&& (!pbi->common.intra_only)) ? 1 : 0;

	if (!is_inter) { /* I-PIC */
		/* remove reset -- disables clock */
		WRITE_VREG(HEVCD_MCRCC_CTL1, 0x0);
		return;
	}

	if (!pbi->m_ins_flag)
		dump_hit_rate(pbi);

	WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, (0 << 8) | (1<<1) | 0);
	lastcanvas = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
	goldencanvas = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
	altrefcanvas = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR);

	if (debug & VP9_DEBUG_CACHE)
		pr_info("[test.c] lastref_canv:%x goldenref_canv:%x altref_canv:%x\n",
			lastcanvas, goldencanvas, altrefcanvas);

	altref_inref = ((altref_inref == 1) &&
			(altrefcanvas != (goldenref_inref
			? goldencanvas : 0xffffffff)) &&
			(altrefcanvas != (lastref_inref ?
			lastcanvas : 0xffffffff))) ? 1 : 0;
	goldenref_inref = ((goldenref_inref == 1) &&
		(goldencanvas != (lastref_inref ?
		lastcanvas : 0xffffffff))) ? 1 : 0;
	if (debug & VP9_DEBUG_CACHE)
		pr_info("[test.c]--1--lastref_inref:%d goldenref_inref:%d altref_inref:%d\n",
			lastref_inref, goldenref_inref, altref_inref);

	altref_delta_picnum = altref_inref ? altref_delta_picnum : 0x7fffffff;
	goldenref_delta_picnum = goldenref_inref ?
		goldenref_delta_picnum : 0x7fffffff;
	lastref_delta_picnum = lastref_inref ?
		lastref_delta_picnum : 0x7fffffff;
	if (debug & VP9_DEBUG_CACHE)
		pr_info("[test.c]--1--lastref_delta_picnum:%d goldenref_delta_picnum:%d altref_delta_picnum:%d\n",
			lastref_delta_picnum, goldenref_delta_picnum,
			altref_delta_picnum);
	/*ARRAY SORT HERE DELTA/CANVAS ARRAY SORT -- use DELTA*/

	refcanvas_array[0] = lastcanvas;
	refcanvas_array[1] = goldencanvas;
	refcanvas_array[2] = altrefcanvas;

	deltapicnum_array[0] = lastref_delta_picnum;
	deltapicnum_array[1] = goldenref_delta_picnum;
	deltapicnum_array[2] = altref_delta_picnum;

	/* sort0 : 2-to-1 */
	if (deltapicnum_array[2] < deltapicnum_array[1]) {
		utmp = refcanvas_array[2];
		refcanvas_array[2] = refcanvas_array[1];
		refcanvas_array[1] = utmp;
		tmp = deltapicnum_array[2];
		deltapicnum_array[2] = deltapicnum_array[1];
		deltapicnum_array[1] = tmp;
	}
	/* sort1 : 1-to-0 */
	if (deltapicnum_array[1] < deltapicnum_array[0]) {
		utmp = refcanvas_array[1];
		refcanvas_array[1] = refcanvas_array[0];
		refcanvas_array[0] = utmp;
		tmp = deltapicnum_array[1];
		deltapicnum_array[1] = deltapicnum_array[0];
		deltapicnum_array[0] = tmp;
	}
	/* sort2 : 2-to-1 */
	if (deltapicnum_array[2] < deltapicnum_array[1]) {
		utmp = refcanvas_array[2];  refcanvas_array[2] =
			refcanvas_array[1]; refcanvas_array[1] =  utmp;
		tmp  = deltapicnum_array[2]; deltapicnum_array[2] =
			deltapicnum_array[1]; deltapicnum_array[1] = tmp;
	}
	if (mcrcc_cache_alg_flag ==
		THODIYIL_MCRCC_CANVAS_ALGX) { /*09/15/2017*/
		/* lowest delta_picnum */
		rdata32 = refcanvas_array[0];
		rdata32 = rdata32 & 0xffff;
		rdata32 = rdata32 | (rdata32 << 16);
		WRITE_VREG(HEVCD_MCRCC_CTL2, rdata32);

		/* 2nd-lowest delta_picnum */
		rdata32 = refcanvas_array[1];
		rdata32 = rdata32 & 0xffff;
		rdata32 = rdata32 | (rdata32 << 16);
		WRITE_VREG(HEVCD_MCRCC_CTL3, rdata32);
	} else {
		/* previous version -- LAST/GOLDEN ALWAYS -- before 09/13/2017*/
		WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR,
			(0 << 8) | (1<<1) | 0);
		rdata32 = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
		rdata32 = rdata32 & 0xffff;
		rdata32 = rdata32 | (rdata32 << 16);
		WRITE_VREG(HEVCD_MCRCC_CTL2, rdata32);

		/* Programme canvas1 */
		rdata32 = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
		rdata32 = rdata32 & 0xffff;
		rdata32 = rdata32 | (rdata32 << 16);
		WRITE_VREG(HEVCD_MCRCC_CTL3, rdata32);
	}

	WRITE_VREG(HEVCD_MCRCC_CTL1, 0xff0); /* enable mcrcc progressive-mode */
	return;
}

#endif


static void free_lf_buf(struct VP9Decoder_s *pbi)
{
	if (pbi->lfi)
		vfree(pbi->lfi);
	if (pbi->lf)
		vfree(pbi->lf);
	if (pbi->seg_4lf)
		vfree(pbi->seg_4lf);
	pbi->lfi = NULL;
	pbi->lf = NULL;
	pbi->seg_4lf = NULL;
}

static int alloc_lf_buf(struct VP9Decoder_s *pbi)
{
	pbi->lfi = vmalloc(sizeof(struct loop_filter_info_n));
	pbi->lf = vmalloc(sizeof(struct loopfilter));
	pbi->seg_4lf = vmalloc(sizeof(struct segmentation));
	if (pbi->lfi == NULL || pbi->lf == NULL || pbi->seg_4lf == NULL) {
		free_lf_buf(pbi);
		pr_err("[test.c] vp9_loop_filter init malloc error!!!\n");
		return -1;
	}
	return 0;
}

static void vp9_local_uninit(struct VP9Decoder_s *pbi)
{
	pbi->rpm_ptr = NULL;
	pbi->lmem_ptr = NULL;
	if (pbi->rpm_addr) {
		decoder_dma_free_coherent(pbi->rpm_mem_handle,
					RPM_BUF_SIZE,
					pbi->rpm_addr,
					pbi->rpm_phy_addr);
		pbi->rpm_addr = NULL;
	}
	if (pbi->lmem_addr) {
		if (pbi->lmem_phy_addr)
			decoder_dma_free_coherent(pbi->lmem_phy_handle,
				LMEM_BUF_SIZE, pbi->lmem_addr,
				pbi->lmem_phy_addr);
		pbi->lmem_addr = NULL;
	}
	if (is_vp9_adapt_prob_hw_mode()) {
		pbi->prob_buffer_phy_addr = 0;
		pbi->count_buffer_phy_addr = 0;
		pbi->prob_buffer_addr = NULL;
		pbi->count_buffer_addr = NULL;
	} else {
		if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) &&
			(vdec_secure(hw_to_vdec(pbi)))) {
			tee_vp9_prob_free((u32)pbi->prob_buffer_phy_addr);
			pbi->prob_buffer_phy_addr = 0;
			pbi->count_buffer_phy_addr = 0;
			pbi->prob_buffer_addr = NULL;
			pbi->count_buffer_addr = NULL;
		} else {
			if (pbi->prob_buffer_addr) {
				if (pbi->prob_buffer_phy_addr)
					decoder_dma_free_coherent(pbi->prob_buf_handle,
						PROB_BUF_SIZE, pbi->prob_buffer_addr,
						pbi->prob_buffer_phy_addr);

				pbi->prob_buffer_addr = NULL;
			}
			if (pbi->count_buffer_addr) {
				if (pbi->count_buffer_phy_addr)
					decoder_dma_free_coherent(pbi->count_buf_handle,
						COUNT_BUF_SIZE, pbi->count_buffer_addr,
						pbi->count_buffer_phy_addr);

				pbi->count_buffer_addr = NULL;
			}
		}
	}
	if (pbi->mmu_enable) {
		u32 mmu_map_size = vvp9_frame_mmu_map_size(pbi);
		if (pbi->frame_mmu_map_addr) {
			if (pbi->frame_mmu_map_phy_addr)
				decoder_dma_free_coherent(pbi->frame_mmu_map_handle,
					mmu_map_size,
					pbi->frame_mmu_map_addr,
					pbi->frame_mmu_map_phy_addr);
			pbi->frame_mmu_map_addr = NULL;
		}
	}
#ifdef SUPPORT_FB_DECODING
	if (pbi->stage_mmu_map_addr) {
		if (pbi->stage_mmu_map_phy_addr)
			decoder_dma_free_coherent(pbi->stage_mmu_map_handle,
				STAGE_MMU_MAP_SIZE * STAGE_MAX_BUFFERS,
				pbi->stage_mmu_map_addr,
					pbi->stage_mmu_map_phy_addr);
		pbi->stage_mmu_map_addr = NULL;
	}

	uninit_stage_buf(pbi);
#endif

#ifdef VP9_LPF_LVL_UPDATE
	free_lf_buf(pbi);
#endif
	if (pbi->gvs)
		vfree(pbi->gvs);
	pbi->gvs = NULL;
}

static bool is_support_4k_vp9(void)
{
	if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXHD2)
		return false;
	if (hevc_is_support_4k())
		return true;
	return false;
}

static int vp9_local_init(struct VP9Decoder_s *pbi)
{
	int ret = -1;
	/*int losless_comp_header_size, losless_comp_body_size;*/

	struct BuffInfo_s *cur_buf_info = NULL;

	memset(&pbi->param, 0, sizeof(union param_u));
	memset(&pbi->common, 0, sizeof(struct VP9_Common_s));
#ifdef MULTI_INSTANCE_SUPPORT
	cur_buf_info = &pbi->work_space_buf_store;
	if (force_bufspec) {
		memcpy(cur_buf_info, &amvvp9_workbuff_spec[force_bufspec & 0xf],
		sizeof(struct BuffInfo_s));
		pr_info("force buffer spec %d\n", force_bufspec & 0xf);
	} else {
		if ((get_cpu_major_id() <= AM_MESON_CPU_MAJOR_ID_TM2 && !is_cpu_tm2_revb()) ||
			(get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXHD2)) {
			if (is_support_4k_vp9()) {
				if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1) {
					memcpy(cur_buf_info, &amvvp9_workbuff_spec[2],	/* 8k */
					sizeof(struct BuffInfo_s));
				} else
					memcpy(cur_buf_info, &amvvp9_workbuff_spec[1],	/* 4k */
					sizeof(struct BuffInfo_s));
			} else
				memcpy(cur_buf_info, &amvvp9_workbuff_spec[0],/* 1080p */
				sizeof(struct BuffInfo_s));
		} else { //get_cpu_major_id() > AM_MESON_CPU_MAJOR_ID_TM2 || is_cpu_tm2_revb()
			if (is_support_4k_vp9()) {
				memcpy(cur_buf_info, &amvvp9_workbuff_spec[5],	/* 8k */
				sizeof(struct BuffInfo_s));
			} else
				memcpy(cur_buf_info, &amvvp9_workbuff_spec[3],/* 1080p */
				sizeof(struct BuffInfo_s));
		}
	}

	cur_buf_info->start_adr = pbi->buf_start;
	if (!pbi->mmu_enable)
		pbi->mc_buf_spec.buf_end = pbi->buf_start + pbi->buf_size;

#else
/*! MULTI_INSTANCE_SUPPORT*/
	if ((get_cpu_major_id() <= AM_MESON_CPU_MAJOR_ID_TM2 && !is_cpu_tm2_revb()) ||
		(get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXHD2)) {
		if (is_support_4k_vp9()) {
			if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1)
				cur_buf_info = &amvvp9_workbuff_spec[2];/* 8k work space */
			else
				cur_buf_info = &amvvp9_workbuff_spec[1];/* 4k2k work space */
		} else
			cur_buf_info = &amvvp9_workbuff_spec[0];/* 1080p work space */
	} else { //get_cpu_major_id() > AM_MESON_CPU_MAJOR_ID_TM2 || is_cpu_tm2_revb()
		if (is_support_4k_vp9()) {
			cur_buf_info = &amvvp9_workbuff_spec[5];/* 8k work space */
		} else
			cur_buf_info = &amvvp9_workbuff_spec[3];/* 1080p work space */
	}

#endif

	init_buff_spec(pbi, cur_buf_info);
	vp9_bufmgr_init(pbi, cur_buf_info, NULL);

	if (!is_support_4k_vp9()
		&& (buf_alloc_width > 1920 &&  buf_alloc_height > 1088)) {
		buf_alloc_width = 1920;
		buf_alloc_height = 1088;
		if (pbi->max_pic_w > 1920 && pbi->max_pic_h > 1088) {
			pbi->max_pic_w = 1920;
			pbi->max_pic_h = 1088;
		}
	} else if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1) &&
			(get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_TXHD2)) {
		buf_alloc_width = 8192;
		buf_alloc_height = 4608;
	}
	pbi->init_pic_w = pbi->max_pic_w ? pbi->max_pic_w :
		(buf_alloc_width ? buf_alloc_width :
		(pbi->vvp9_amstream_dec_info.width ?
		pbi->vvp9_amstream_dec_info.width :
		pbi->work_space_buf->max_width));
	pbi->init_pic_h = pbi->max_pic_h ? pbi->max_pic_h :
		(buf_alloc_height ? buf_alloc_height :
		(pbi->vvp9_amstream_dec_info.height ?
		pbi->vvp9_amstream_dec_info.height :
		pbi->work_space_buf->max_height));

	/* video is not support unaligned with 64 in tl1
	** vdec canvas mode will be linear when dump yuv is set
	*/
	if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) &&
		(pbi->double_write_mode != 0) &&
		(((pbi->max_pic_w % 64) != 0) ||
		(pbi->vvp9_amstream_dec_info.width % 64) != 0)) {
		if (hw_to_vdec(pbi)->canvas_mode !=
			CANVAS_BLKMODE_LINEAR)
			pbi->mem_map_mode = 2;
		else {
			pbi->mem_map_mode = 0;
			pr_info("vdec blkmod linear, force mem_map_mode 0\n");
		}
	}

	pbi->pts_unstable = ((unsigned long)(pbi->vvp9_amstream_dec_info.param)
		& 0x40) >> 6;

	if ((debug & VP9_DEBUG_SEND_PARAM_WITH_REG) == 0) {
		pbi->rpm_addr = decoder_dma_alloc_coherent(&pbi->rpm_mem_handle,
				RPM_BUF_SIZE, &pbi->rpm_phy_addr, "VP9_RPM_BUF");
		if (pbi->rpm_addr == NULL) {
			pr_err("%s: failed to alloc rpm buffer\n", __func__);
			return -1;
		}

		pbi->rpm_ptr = pbi->rpm_addr;
	}

	pbi->lmem_addr = decoder_dma_alloc_coherent(&pbi->lmem_phy_handle,
			LMEM_BUF_SIZE, &pbi->lmem_phy_addr, "VP9_LMEM_BUF");
	if (pbi->lmem_addr == NULL) {
		pr_err("%s: failed to alloc lmem buffer\n", __func__);
		return -1;
	}
	pbi->lmem_ptr = pbi->lmem_addr;

	if (is_vp9_adapt_prob_hw_mode()) {
		pbi->prob_buffer_phy_addr = cur_buf_info->prob_buf.buf_start;
		pbi->count_buffer_phy_addr = cur_buf_info->prob_cnt_buf.buf_start;
		pbi->prob_buffer_addr = NULL;
		pbi->count_buffer_addr = NULL;
	} else {
		if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) &&
			(vdec_secure(hw_to_vdec(pbi)))) {
			u32 prob_addr, id;
			id = tee_vp9_prob_malloc(&prob_addr);
			if (prob_addr <= 0)
				pr_err("%s, tee[%d] malloc prob buf failed\n", __func__, id);
			else {
				pbi->prob_buffer_phy_addr = prob_addr;
				pbi->count_buffer_phy_addr = pbi->prob_buffer_phy_addr + PROB_BUF_SIZE;
			}
			pbi->prob_buffer_addr = NULL;
			pbi->count_buffer_addr = NULL;
		} else {
			pbi->prob_buffer_addr = decoder_dma_alloc_coherent(&pbi->prob_buf_handle,
						PROB_BUF_SIZE, &pbi->prob_buffer_phy_addr, "VP9_PROB_BUF");
			if (pbi->prob_buffer_addr == NULL) {
				pr_err("%s: failed to alloc prob_buffer\n", __func__);
				return -1;
			}
			memset(pbi->prob_buffer_addr, 0, PROB_BUF_SIZE);
			pbi->count_buffer_addr = decoder_dma_alloc_coherent(&pbi->count_buf_handle,
						COUNT_BUF_SIZE, &pbi->count_buffer_phy_addr, "VP9_COUNT_BUF");
			if (pbi->count_buffer_addr == NULL) {
				pr_err("%s: failed to alloc count_buffer\n", __func__);
				return -1;
			}
			memset(pbi->count_buffer_addr, 0, COUNT_BUF_SIZE);
		}
	}

	if (pbi->mmu_enable) {
		u32 mmu_map_size = vvp9_frame_mmu_map_size(pbi);
		pbi->frame_mmu_map_addr =
			decoder_dma_alloc_coherent(&pbi->frame_mmu_map_handle,
				mmu_map_size,
				&pbi->frame_mmu_map_phy_addr, "VP9_MMU_BUF");
		if (pbi->frame_mmu_map_addr == NULL) {
			pr_err("%s: failed to alloc count_buffer\n", __func__);
			return -1;
		}
		memset(pbi->frame_mmu_map_addr, 0, mmu_map_size);
	}
#ifdef SUPPORT_FB_DECODING
	if (pbi->m_ins_flag && stage_buf_num > 0) {
		pbi->stage_mmu_map_addr =
			decoder_dma_alloc_coherent(&pbi->stage_mmu_map_handle,
				STAGE_MMU_MAP_SIZE * STAGE_MAX_BUFFERS,
				&pbi->stage_mmu_map_phy_addr, "VP9_STAGE_MMU_BUF");
		if (pbi->stage_mmu_map_addr == NULL) {
			pr_err("%s: failed to alloc count_buffer\n", __func__);
			return -1;
		}
		memset(pbi->stage_mmu_map_addr,
			0, STAGE_MMU_MAP_SIZE * STAGE_MAX_BUFFERS);

		init_stage_buf(pbi);
	}
#endif

	ret = 0;
	return ret;
}

/********************************************
 *  Mailbox command
 ********************************************/
#define CMD_FINISHED               0
#define CMD_ALLOC_VIEW             1
#define CMD_FRAME_DISPLAY          3
#define CMD_DEBUG                  10

#define DECODE_BUFFER_NUM_MAX    32
#define DISPLAY_BUFFER_NUM       6

#define video_domain_addr(adr) (adr&0x7fffffff)
#define DECODER_WORK_SPACE_SIZE 0x800000

#define spec2canvas(x)  \
	(((x)->uv_canvas_index << 16) | \
	 ((x)->uv_canvas_index << 8)  | \
	 ((x)->y_canvas_index << 0))


static void set_canvas(struct VP9Decoder_s *pbi,
	struct PIC_BUFFER_CONFIG_s *pic_config)
{
	struct vdec_s *vdec = hw_to_vdec(pbi);
	int canvas_w = ALIGN(pic_config->y_crop_width, 64)/4;
	int canvas_h = ALIGN(pic_config->y_crop_height, 32)/4;
	int blkmode = pbi->mem_map_mode;
	/*CANVAS_BLKMODE_64X32*/
	if	(pic_config->double_write_mode) {
		canvas_w = pic_config->y_crop_width	/
				get_double_write_ratio(
					pic_config->double_write_mode);
		canvas_h = pic_config->y_crop_height /
				get_double_write_ratio(
					pic_config->double_write_mode);

		/* sao ctrl1 reg alignline with 64, align with 64 */
		if (is_hevc_align32(pbi->mem_map_mode))
			canvas_w = ALIGN(canvas_w, 32);
		else
			canvas_w = ALIGN(canvas_w, 64);
		canvas_h = ALIGN(canvas_h, 32);

		if (vdec->parallel_dec == 1) {
			if (pic_config->y_canvas_index == -1)
				pic_config->y_canvas_index =
					vdec->get_canvas_ex(CORE_MASK_HEVC, vdec->id);
			if (pic_config->uv_canvas_index == -1)
				pic_config->uv_canvas_index =
					vdec->get_canvas_ex(CORE_MASK_HEVC, vdec->id);
		} else {
			pic_config->y_canvas_index = 128 + pic_config->index * 2;
			pic_config->uv_canvas_index = 128 + pic_config->index * 2 + 1;
		}

		config_cav_lut_ex(pic_config->y_canvas_index,
			pic_config->dw_y_adr, canvas_w, canvas_h,
			CANVAS_ADDR_NOWRAP, blkmode, 0, VDEC_HEVC);
		config_cav_lut_ex(pic_config->uv_canvas_index,
			pic_config->dw_u_v_adr,	canvas_w, canvas_h,
			CANVAS_ADDR_NOWRAP, blkmode, 0, VDEC_HEVC);

#ifdef MULTI_INSTANCE_SUPPORT
		pic_config->canvas_config[0].phy_addr = pic_config->dw_y_adr;
		pic_config->canvas_config[0].width = canvas_w;
		pic_config->canvas_config[0].height = canvas_h;
		pic_config->canvas_config[0].block_mode = blkmode;
		pic_config->canvas_config[0].endian = 0;

		pic_config->canvas_config[1].phy_addr = pic_config->dw_u_v_adr;
		pic_config->canvas_config[1].width = canvas_w;
		pic_config->canvas_config[1].height = canvas_h;
		pic_config->canvas_config[1].block_mode = blkmode;
		pic_config->canvas_config[1].endian = 0;
#endif
	}
}

static void v4l_vp9_report_hdr10p_data(struct VP9Decoder_s *pbi, struct PIC_BUFFER_CONFIG_s *pic,
	u8 *hdr10p_data_buf, u32 data_size)
{
	struct aml_vcodec_ctx *ctx =
		(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	struct sei_usd_param_s usd_rep;
	void *tmp_buf = NULL;

	memset(&usd_rep, 0, sizeof(struct sei_usd_param_s));

	if (kfifo_is_full(&ctx->dec_intf.aux_done)) {
		vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE,
			"%s, aux data fifo is full\n", __func__);
		return;
	}

	tmp_buf = vzalloc(HDR10P_BUF_SIZE);
	if (tmp_buf == NULL) {
		vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE,
			"%s, Alloc buf for aux_data out fail\n", __func__);
		return;
	}

	memcpy(tmp_buf, hdr10p_data_buf, data_size);
	usd_rep.data_size = data_size;
	usd_rep.v_addr = tmp_buf;
	if (debug & VP9_DEBUG_BUFMGR_MORE) {
		int i;
		vp9_print(pbi, 0,
			"%s AUX data: (size %d)\n", __func__, data_size);
		for (i = 0; i < data_size; i++) {
			vp9_print(pbi, 0, "%02x ", ((u8 *)usd_rep.v_addr)[i]);
			if (((i + 1) & 0xf) == 0)
				vp9_print(pbi, 0, "\n");
		}
		vp9_print(pbi, 0, "\n");
	}

	usd_rep.meta_data.timestamp = pic->timestamp;
	usd_rep.meta_data.vpts_valid = 1;
	usd_rep.meta_data.video_format = VFORMAT_VP9;
	usd_rep.meta_data.records_in_que =
		kfifo_len(&ctx->dec_intf.aux_done) + 1; /* +1 : current one's */
	ctx->dec_intf.decinfo_event_report(ctx, AML_DECINFO_EVENT_HDR10P, &usd_rep);
	vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE,
			"%s, HDR10p ready event report\n", __func__);
}

static void set_frame_info(struct VP9Decoder_s *pbi, struct vframe_s *vf, struct PIC_BUFFER_CONFIG_s *pic)
{
	unsigned int ar;
	struct aml_vcodec_ctx * ctx = pbi->v4l2_ctx;
	int vf_dur = vdec_get_vf_dur();

	vf->duration = vf_dur ? vf_dur : pbi->frame_dur;
	vf->duration_pulldown = 0;
	vf->flag = 0;
	vf->prop.master_display_colour = pbi->vf_dp;
	vf->signal_type = pbi->video_signal_type;
	if (vf->compWidth && vf->compHeight)
		pbi->frame_ar = vf->compHeight * 0x100 / vf->compWidth;
	ar = min_t(u32, pbi->frame_ar, DISP_RATIO_ASPECT_RATIO_MAX);
	vf->ratio_control = (ar << DISP_RATIO_ASPECT_RATIO_BIT);

	if (pbi->vf_dp.present_flag) {
		struct aml_vdec_hdr_infos hdr;

		memset(&hdr, 0, sizeof(hdr));
		hdr.signal_type = vf->signal_type;
		hdr.color_parms = pbi->vf_dp;

		hdr.color_parms.luminance[0] = hdr.color_parms.luminance[0] / 10000;

		vdec_v4l_set_hdr_infos(ctx, &hdr);
	}

	if ((pbi->chunk != NULL) && (pbi->chunk->hdr10p_data_buf != NULL) && (pbi->chunk->hdr10p_data_size > 0) &&
		(pbi->chunk->hdr10p_data_size < HDR10P_BUF_SIZE)) {
		ctx->aux_infos.bind_hdr10p_buffer(ctx, &pic->hdr10p_data_buf);
		if (pic->hdr10p_data_buf != NULL) {
			memcpy(pic->hdr10p_data_buf, pbi->chunk->hdr10p_data_buf,
				pbi->chunk->hdr10p_data_size);
			pic->hdr10p_data_size = pbi->chunk->hdr10p_data_size;
			if (debug & VP9_DEBUG_BUFMGR_MORE) {
				int i = 0;
				vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE,
					"hdr10p data: (size %d)\n",
					pbi->chunk->hdr10p_data_size);
				for (i = 0; i < pbi->chunk->hdr10p_data_size; i++) {
					vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE,
						"%02x ", pbi->chunk->hdr10p_data_buf[i]);
					if (((i + 1) & 0xf) == 0)
						vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE, "\n");
				}
				vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE, "\n");
			}
			vf->hdr10p_data_buf = pic->hdr10p_data_buf;
			vf->hdr10p_data_size = pic->hdr10p_data_size;
			v4l_vp9_report_hdr10p_data(pbi, pic, pic->hdr10p_data_buf, pic->hdr10p_data_size);
			set_meta_data_to_vf(vf, UVM_META_DATA_HDR10P_DATA, pbi->v4l2_ctx);
		} else {
			vp9_print(pbi, 0, "bind_hdr10p_buffer fail\n");
		}
	} else {
		vf->hdr10p_data_buf = NULL;
		vf->hdr10p_data_size = 0;
	}

	vf->sidebind_type = pbi->sidebind_type;
	vf->sidebind_channel_id = pbi->sidebind_channel_id;
	vf->codec_vfmt = VFORMAT_VP9;
}

static int vvp9_vf_states(struct vframe_states *states, void *op_arg)
{
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)op_arg;

	states->vf_pool_size = VF_POOL_SIZE;
	states->buf_free_num = kfifo_len(&pbi->newframe_q);
	states->buf_avail_num = kfifo_len(&pbi->display_q);

	if (step == 2)
		states->buf_avail_num = 0;
	return 0;
}

static struct vframe_s *vvp9_vf_peek(void *op_arg)
{
	struct vframe_s *vf[2] = {0, 0};
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)op_arg;

	if (step == 2)
		return NULL;

	if (kfifo_out_peek(&pbi->display_q, (void *)&vf, 2)) {
		if (vf[1]) {
			vf[0]->next_vf_pts_valid = true;
			vf[0]->next_vf_pts = vf[1]->pts;
		} else
			vf[0]->next_vf_pts_valid = false;
		return vf[0];
	}

	return NULL;
}

static struct vframe_s *vvp9_vf_get(void *op_arg)
{
	struct vframe_s *vf;
	struct vdec_s *vdec = op_arg;
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)vdec->private;

	if (step == 2)
		return NULL;
	else if (step == 1)
		step = 2;

	if (kfifo_get(&pbi->display_q, &vf)) {
		struct vframe_s *next_vf = NULL;
		uint8_t index = vf->index & 0xff;
		ATRACE_COUNTER(pbi->trace.disp_q_name, kfifo_len(&pbi->display_q));
		if (index < pbi->used_buf_num ||
			(vf->type & VIDTYPE_V4L_EOS)) {
			vf->index_disp = atomic_read(&pbi->vf_get_count);
			vf->omx_index = atomic_read(&pbi->vf_get_count);
			atomic_add(1, &pbi->vf_get_count);

			if (debug & VP9_DEBUG_BUFMGR)
				pr_info("%s idx: %d, type 0x%x w/h %d/%d, pts %d, %lld, ts: %lld\n",
					__func__, index, vf->type,
					vf->width, vf->height,
					vf->pts,
					vf->pts_us64,
					vf->timestamp);

			if (kfifo_peek(&pbi->display_q, &next_vf) && next_vf) {
				vf->next_vf_pts_valid = true;
				vf->next_vf_pts = next_vf->pts;
			} else
				vf->next_vf_pts_valid = false;

			kfifo_put(&pbi->newframe_q, (const struct vframe_s *)vf);
			ATRACE_COUNTER(pbi->trace.new_q_name, kfifo_len(&pbi->newframe_q));

			return vf;
		}
	}

	return NULL;
}

static void vp9_recycle_dec_resource(void *priv,
						struct aml_buf *aml_buf)
{
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)priv;
	struct vframe_s *vf = &aml_buf->vframe;
	uint8_t index;

	if (pbi->enable_fence && vf->fence) {
		int ret, i;

		mutex_lock(&pbi->fence_mutex);
		ret = dma_fence_get_status(vf->fence);
		if (ret == 0) {
			for (i = 0; i < VF_POOL_SIZE; i++) {
				if (pbi->fence_vf_s.fence_vf[i] == NULL) {
					pbi->fence_vf_s.fence_vf[i] = vf;
					pbi->fence_vf_s.used_size++;
					mutex_unlock(&pbi->fence_mutex);
					return;
				}
			}
		}
		mutex_unlock(&pbi->fence_mutex);
	}

	index = vf->index & 0xff;

	if (pbi->enable_fence && vf->fence) {
		vdec_fence_put(vf->fence);
		vf->fence = NULL;
	}


	return;
}

static void vvp9_vf_put(struct vframe_s *vf, void *op_arg)
{
	struct vdec_s *vdec = op_arg;
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)vdec->private;
	struct aml_vcodec_ctx *ctx = pbi->v4l2_ctx;
	struct aml_buf *aml_buf;

	if (vf == (&pbi->vframe_dummy))
		return;

	if (!vf)
		return;

	aml_buf = (struct aml_buf *)vf->v4l_mem_handle;
	aml_buf_put_ref(&ctx->bm, aml_buf);
	vp9_recycle_dec_resource(pbi, aml_buf);

#ifdef MULTI_INSTANCE_SUPPORT
	vdec_up(vdec);
#endif
}

static int vvp9_event_cb(int type, void *data, void *private_data)
{
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)private_data;

	if (type & VFRAME_EVENT_RECEIVER_RESET) {

	} else if (type & VFRAME_EVENT_RECEIVER_REQ_STATE) {
		struct provider_state_req_s *req =
			(struct provider_state_req_s *)data;
		if (req->req_type == REQ_STATE_SECURE)
			req->req_result[0] = vdec_secure(hw_to_vdec(pbi));
		else
			req->req_result[0] = 0xffffffff;
	}

	return 0;
}

void inc_vf_ref(struct VP9Decoder_s *pbi, int index)
{
	struct VP9_Common_s *cm = &pbi->common;

	cm->buffer_pool->frame_bufs[index].buf.vf_ref++;

	if (debug & VP9_DEBUG_BUFMGR_MORE)
		pr_info("%s index = %d new vf_ref = %d\r\n",
			__func__, index,
			cm->buffer_pool->frame_bufs[index].buf.vf_ref);
}

static int frame_duration_adapt(struct VP9Decoder_s *pbi, struct vframe_s *vf, u32 valid)
{
	u32 old_duration, pts_duration = 0;
	u32 pts = vf->pts;

	if (pbi->get_frame_dur == true)
		return true;

	pbi->frame_cnt_window++;
	if (!(pbi->vp9_first_pts_ready == 1)) {
		if (valid) {
			pbi->pts1 = pts;
			pbi->frame_cnt_window = 0;
			pbi->duration_from_pts_done = 0;
			pbi->vp9_first_pts_ready = 1;
		} else {
			return false;
		}
	} else {
		if (pts < pbi->pts1) {
			if (pbi->frame_cnt_window > FRAME_CNT_WINDOW_SIZE) {
				pbi->pts1 = pts;
				pbi->frame_cnt_window = 0;
			}
		}

		if (valid && (pbi->frame_cnt_window > FRAME_CNT_WINDOW_SIZE) &&
			(pts > pbi->pts1) && (pbi->duration_from_pts_done == 0)) {
				old_duration = pbi->frame_dur;
				pbi->pts2 = pts;
				pts_duration = (((pbi->pts2 - pbi->pts1) * 16) /
					(pbi->frame_cnt_window * 15));

			if (close_to(pts_duration, old_duration, 2000)) {
				pbi->frame_dur = pts_duration;
				if ((debug & VP9_DEBUG_OUT_PTS) != 0)
					pr_info("use calc duration %d\n", pts_duration);
			}

			if (pbi->duration_from_pts_done == 0) {
				if (close_to(pts_duration, old_duration, RATE_CORRECTION_THRESHOLD)) {
					pbi->duration_from_pts_done = 1;
				} else {
					if (!close_to(pts_duration,
						 old_duration, 1000) &&
						!close_to(pts_duration,
						pbi->frame_dur, 1000) &&
						close_to(pts_duration,
						pbi->last_duration, 200)) {
						/* frame_dur must
						 *  wrong,recover it.
						 */
						pbi->frame_dur = pts_duration;
					}
					pbi->pts1 = pbi->pts2;
					pbi->frame_cnt_window = 0;
					pbi->duration_from_pts_done = 0;
				}
			}
			pbi->last_duration = pts_duration;
		}
	}
	return true;
}

static void v4l_vp9_update_frame_info(struct VP9Decoder_s *pbi, struct vframe_s *vf,
	struct PIC_BUFFER_CONFIG_s *pic_config)
{
	struct aml_vcodec_ctx *ctx = pbi->v4l2_ctx;
	struct dec_frame_info_s frm_info = {0};

	memcpy(&(frm_info.qos), &(pbi->vframe_qos), sizeof(struct vframe_qos_s));
	if (input_frame_based(hw_to_vdec(pbi)))
		frm_info.frame_size = pic_config->frame_size2;

	frm_info.offset = pic_config->stream_offset;
	frm_info.num = pic_config->decode_idx;
	frm_info.type = pic_config->slice_type;
	frm_info.error_flag = pic_config->error_mark;
	frm_info.decode_time_cost = pic_config->hw_decode_time;
	frm_info.pic_height = pbi->frame_width;
	frm_info.pic_width = pbi->frame_width;
	frm_info.signal_type = pbi->video_signal_type;
	frm_info.bitrate = pbi->gvs->bit_rate;
	frm_info.status = pbi->gvs->status;
	frm_info.ratio_control = pbi->gvs->ratio_control;

	if (vf) {
		frm_info.ext_signal_type = vf->ext_signal_type;
		frm_info.vf_type = vf->type;
		frm_info.timestamp = vf->timestamp;
		frm_info.pts = vf->pts;
		frm_info.pts_us64 = vf->pts_us64;
	}
	ctx->dec_intf.decinfo_event_report(ctx, AML_DECINFO_EVENT_FRAME, &frm_info);
}

static inline void pbi_update_gvs(struct VP9Decoder_s *pbi)
{
	if (pbi->gvs->frame_height != frame_height) {
		pbi->gvs->frame_width = frame_width;
		pbi->gvs->frame_height = frame_height;
	}
	if (pbi->gvs->frame_dur != pbi->frame_dur) {
		pbi->gvs->frame_dur = pbi->frame_dur;
		if (pbi->frame_dur != 0)
			pbi->gvs->frame_rate = ((96000 * 10 / pbi->frame_dur) % 10) < 5 ?
					96000 / pbi->frame_dur : (96000 / pbi->frame_dur +1);
		else
			pbi->gvs->frame_rate = -1;
	}
	pbi->gvs->status = pbi->stat | pbi->fatal_error;
}

static int prepare_display_buf(struct VP9Decoder_s *pbi,
				struct PIC_BUFFER_CONFIG_s *pic_config)
{
	struct vframe_s *vf = NULL;
	struct vdec_s *pvdec = hw_to_vdec(pbi);
	int stream_offset = pic_config->stream_offset;
	unsigned short slice_type = pic_config->slice_type;
	struct aml_vcodec_ctx * v4l2_ctx = pbi->v4l2_ctx;
	struct aml_buf *aml_buf = NULL;
	ulong nv_order = VIDTYPE_VIU_NV21;
	u32 pts_valid = 0, pts_us64_valid = 0;
	u32 pts_save;
	u64 pts_us64_save;
	u32 frame_size = 0;
	int i = 0;


	if (debug & VP9_DEBUG_BUFMGR)
		pr_info("%s index = %d\r\n", __func__, pic_config->index);
	if (kfifo_get(&pbi->newframe_q, &vf) == 0) {
		pr_info("fatal error, no available buffer slot.");
		return -1;
	}

	/* swap uv */
	if ((v4l2_ctx->cap_pix_fmt == V4L2_PIX_FMT_NV12) ||
		(v4l2_ctx->cap_pix_fmt == V4L2_PIX_FMT_NV12M))
		nv_order = VIDTYPE_VIU_NV12;

	if (pic_config->double_write_mode)
		set_canvas(pbi, pic_config);

	display_frame_count[pbi->index]++;
	if (vf) {
		if (!force_pts_unstable) {
			if ((pic_config->pts == 0) || ((pic_config->pts <= pbi->last_pts) &&
				(pic_config->pts64 <= pbi->last_pts_us64))) {
				for (i = (FRAME_BUFFERS - 1); i > 0; i--) {
					if ((pbi->last_pts == pbi->frame_mode_pts_save[i]) ||
						(pbi->last_pts_us64 == pbi->frame_mode_pts64_save[i])) {
						pic_config->pts = pbi->frame_mode_pts_save[i - 1];
						pic_config->pts64 = pbi->frame_mode_pts64_save[i - 1];
						break;
					}
				}
				if ((i == 0) || (pic_config->pts <= pbi->last_pts)) {
					vp9_print(pbi, VP9_DEBUG_OUT_PTS,
						"no found pts %d, set 0. %d, %d\n",
						i, pic_config->pts, pbi->last_pts);
					pic_config->pts = 0;
					pic_config->pts64 = 0;
				}
			}
		}

		aml_buf = index_to_aml_buf(pbi, pic_config->v4l_buf_index);
		vf->v4l_mem_handle = (ulong)aml_buf;
		vf->src_fmt.dv_id = v4l2_ctx->dv_id;

		if (!aml_buf) {
			kfifo_put(&pbi->newframe_q, (const struct vframe_s *)vf);
			vp9_print(pbi, 0,
			"[ERR]: v4l_ref_buf_addr(index: %d) is NULL!\n",
			__func__, pic_config->v4l_buf_index);

			return -1;
		}

		if (pbi->enable_fence) {
			/* fill fence information. */
			if (pbi->fence_usage == FENCE_USE_FOR_DRIVER)
				vf->fence	= pic_config->fence;
		}

#ifdef MULTI_INSTANCE_SUPPORT
		if (vdec_frame_based(pvdec)) {
			vf->pts = pic_config->pts;
			vf->pts_us64 = pic_config->pts64;

			if (v4l_bitstream_id_enable)
				vf->timestamp = pic_config->timestamp;
			else
				vf->timestamp = pic_config->pts64;

			if (vf->pts != 0 || vf->pts_us64 != 0) {
				pts_valid = 1;
				pts_us64_valid = 1;
			} else {
				pts_valid = 0;
				pts_us64_valid = 0;
			}
		} else
#endif
		/* if (pts_lookup_offset(PTS_TYPE_VIDEO,
		 *   stream_offset, &vf->pts, 0) != 0) {
		 */
		if ((pvdec->vbuf.no_parser == 0) || (pvdec->vbuf.use_ptsserv)) {
			if (pts_lookup_offset_us64
				(PTS_TYPE_VIDEO, stream_offset, &vf->pts,
				&frame_size, 0,
				&vf->pts_us64) != 0) {
#ifdef DEBUG_PTS
				pbi->pts_missed++;
#endif
				vf->pts = 0;
				vf->pts_us64 = 0;
				pts_valid = 0;
				pts_us64_valid = 0;
			} else {
#ifdef DEBUG_PTS
				pbi->pts_hit++;
#endif
				pts_valid = 1;
				pts_us64_valid = 1;
			}
		}

		fill_frame_info(pbi, pic_config, frame_size, vf->pts);

		pts_save = vf->pts;
		pts_us64_save = vf->pts_us64;

		frame_duration_adapt(pbi, vf, pts_valid);
		if (pbi->duration_from_pts_done) {
			pbi->pts_mode = PTS_NONE_REF_USE_DURATION;
		} else {
			if (pts_valid || pts_us64_valid)
				pbi->pts_mode = PTS_NORMAL;
		}

		if ((pbi->pts_mode == PTS_NORMAL) && (vf->pts != 0)
			&& pbi->get_frame_dur) {
			int pts_diff = (int)vf->pts - pbi->last_lookup_pts;

			if (pts_diff < 0) {
				pbi->pts_mode_switching_count++;
				pbi->pts_mode_recovery_count = 0;

				if (pbi->pts_mode_switching_count >=
					PTS_MODE_SWITCHING_THRESHOLD) {
					pbi->pts_mode =
						PTS_NONE_REF_USE_DURATION;
					pr_info
					("HEVC: switch to n_d mode.\n");
				}

			} else {
				int p = PTS_MODE_SWITCHING_RECOVERY_THRESHOLD;

				pbi->pts_mode_recovery_count++;
				if (pbi->pts_mode_recovery_count > p) {
					pbi->pts_mode_switching_count = 0;
					pbi->pts_mode_recovery_count = 0;
				}
			}
		}

		if (vf->pts != 0)
			pbi->last_lookup_pts = vf->pts;

		if ((pbi->pts_mode == PTS_NONE_REF_USE_DURATION)
			&& (slice_type != KEY_FRAME))
			vf->pts = pbi->last_pts + DUR2PTS(pbi->frame_dur);
		pbi->last_pts = vf->pts;

		if (vf->pts_us64 != 0)
			pbi->last_lookup_pts_us64 = vf->pts_us64;

		if ((pbi->pts_mode == PTS_NONE_REF_USE_DURATION)
			&& (slice_type != KEY_FRAME)) {
			vf->pts_us64 =
				pbi->last_pts_us64 +
				(DUR2PTS(pbi->frame_dur) * 100 / 9);
		}
		pbi->last_pts_us64 = vf->pts_us64;

		if (pbi->pts_mode == PTS_NONE_REF_USE_DURATION) {
			vf->disp_pts = vf->pts;
			vf->disp_pts_us64 = vf->pts_us64;
			vf->pts = pts_save;
			vf->pts_us64 = pts_us64_save;
		} else {
			vf->disp_pts = 0;
			vf->disp_pts_us64 = 0;
		}

		vf->index = 0xff00 | pic_config->v4l_buf_index;

		if (pic_config->double_write_mode & 0x10) {
			/* double write only */
			vf->compBodyAddr = 0;
			vf->compHeadAddr = 0;
		} else {
			if (pbi->mmu_enable) {
				vf->compBodyAddr = 0;
				vf->compHeadAddr = pic_config->header_adr;
			}
			vf->canvas0Addr = vf->canvas1Addr = 0;
		}
		if (pic_config->double_write_mode) {
			vf->type = VIDTYPE_PROGRESSIVE |
				VIDTYPE_VIU_FIELD;
			vf->type |= nv_order;
			if (!v4l2_ctx->no_fbc_output &&
				(pic_config->double_write_mode == 3) &&
				(!IS_8K_SIZE(pic_config->y_crop_width,
				pic_config->y_crop_height))) {
				vf->type |= VIDTYPE_COMPRESS;
				if (pbi->mmu_enable)
					vf->type |= VIDTYPE_SCATTER;
			}
			if (!v4l2_ctx->no_fbc_output &&
				pic_config->double_write_mode != 16 &&
				(!IS_8K_SIZE(pic_config->y_crop_width,
				pic_config->y_crop_height)))
				vf->type |= VIDTYPE_COMPRESS | VIDTYPE_SCATTER;
#ifdef MULTI_INSTANCE_SUPPORT
			if (pbi->m_ins_flag) {
				vf->canvas0Addr = vf->canvas1Addr = -1;
				vf->plane_num = 2;
				vf->canvas0_config[0] =
					pic_config->canvas_config[0];
				vf->canvas0_config[1] =
					pic_config->canvas_config[1];
				vf->canvas1_config[0] =
					pic_config->canvas_config[0];
				vf->canvas1_config[1] =
					pic_config->canvas_config[1];

			} else
#endif
				vf->canvas0Addr = vf->canvas1Addr =
					spec2canvas(pic_config);
		} else {
			vf->canvas0Addr = vf->canvas1Addr = 0;
			vf->type = VIDTYPE_COMPRESS | VIDTYPE_VIU_FIELD;
			if (pbi->mmu_enable)
				vf->type |= VIDTYPE_SCATTER;
		}

		switch (pic_config->bit_depth) {
		case VPX_BITS_8:
			vf->bitdepth = BITDEPTH_Y8 |
				BITDEPTH_U8 | BITDEPTH_V8;
			break;
		case VPX_BITS_10:
		case VPX_BITS_12:
			vf->bitdepth = BITDEPTH_Y10 |
				BITDEPTH_U10 | BITDEPTH_V10;
			break;
		default:
			vf->bitdepth = BITDEPTH_Y10 |
				BITDEPTH_U10 | BITDEPTH_V10;
			break;
		}
		if ((vf->type & VIDTYPE_COMPRESS) == 0)
			vf->bitdepth =
				BITDEPTH_Y8 | BITDEPTH_U8 | BITDEPTH_V8;
		if (pic_config->bit_depth == VPX_BITS_8)
			vf->bitdepth |= BITDEPTH_SAVING_MODE;

		vf->width = pic_config->y_crop_width /
			get_double_write_ratio(
				pic_config->double_write_mode);
		vf->height = pic_config->y_crop_height /
			get_double_write_ratio(
				pic_config->double_write_mode);
		if (force_w_h != 0) {
			vf->width = (force_w_h >> 16) & 0xffff;
			vf->height = force_w_h & 0xffff;
		}
		vf->compWidth = pic_config->y_crop_width;
		vf->compHeight = pic_config->y_crop_height;

		vf->type_original = vf->type;
		set_frame_info(pbi, vf, pic_config);

		if (pbi->high_bandwidth_flag) {
			vf->flag |= VFRAME_FLAG_HIGH_BANDWIDTH;
		}

		if (force_fps & 0x100) {
			u32 rate = force_fps & 0xff;

			if (rate)
				vf->duration = 96000/rate;
			else
				vf->duration = 0;
		}
		if (vdec_stream_based(pvdec) && (!pvdec->vbuf.use_ptsserv)) {
			vf->pts_us64 = stream_offset;
			vf->pts = 0;
		}
		if ((debug & VP9_DEBUG_OUT_PTS) != 0) {
			pr_info
			("VP9 dec out pts: pts_mode=%d,dur=%d,pts(%d,%lld,%lld)(%d,%lld)\n",
			pbi->pts_mode, pbi->frame_dur, vf->pts,
			vf->pts_us64, vf->timestamp, pts_save,
			pts_us64_save);
		}
		if (!(pic_config->y_crop_width == 196
		&& pic_config->y_crop_height == 196
		&& (debug & VP9_DEBUG_NO_TRIGGER_FRAME) == 0
		&& (get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_TXLX))) {
			struct vdec_info tmp4x;

			inc_vf_ref(pbi, pic_config->v4l_buf_index);
			vdec_vframe_ready(pvdec, vf);
			if (pic_config->v4l_buf_index != pic_config->BUF_index)	{
				struct PIC_BUFFER_CONFIG_s *dst_pic =
					&pbi->common.buffer_pool->frame_bufs[pic_config->v4l_buf_index].buf;
				struct PIC_BUFFER_CONFIG_s *src_pic =
					&pbi->common.buffer_pool->frame_bufs[pic_config->BUF_index].buf;
				struct vdec_ge2d_info ge2d_info;

				ge2d_info.dst_vf = vf;
				ge2d_info.src_canvas0Addr = ge2d_info.src_canvas1Addr = 0;
				if (dst_pic->double_write_mode) {
#ifdef MULTI_INSTANCE_SUPPORT
					if (pbi->m_ins_flag) {
						vf->canvas0Addr = vf->canvas1Addr = -1;
						ge2d_info.src_canvas0Addr = ge2d_info.src_canvas1Addr = -1;
						vf->plane_num = 2;
						vf->canvas0_config[0] =
							dst_pic->canvas_config[0];
						vf->canvas0_config[1] =
							dst_pic->canvas_config[1];
						vf->canvas1_config[0] =
							dst_pic->canvas_config[0];
						vf->canvas1_config[1] =
							dst_pic->canvas_config[1];
						ge2d_info.src_canvas0_config[0] =
							src_pic->canvas_config[0];
						ge2d_info.src_canvas0_config[1] =
							src_pic->canvas_config[1];
						ge2d_info.src_canvas1_config[0] =
							src_pic->canvas_config[0];
						ge2d_info.src_canvas1_config[1] =
							src_pic->canvas_config[1];
					} else
#endif
					{
						vf->canvas0Addr = vf->canvas1Addr =
							spec2canvas(dst_pic);
						ge2d_info.src_canvas0Addr = ge2d_info.src_canvas1Addr =
							spec2canvas(src_pic);
					}
				}

				if (!pbi->ge2d) {
					int mode = nv_order == VIDTYPE_VIU_NV21 ? GE2D_MODE_CONVERT_NV21 : GE2D_MODE_CONVERT_NV12;
					mode |= GE2D_MODE_CONVERT_LE;
					vdec_ge2d_init(&pbi->ge2d, mode);
				}
				vdec_ge2d_copy_data(pbi->ge2d, &ge2d_info);
			}
			decoder_do_frame_check(pvdec, vf);
			kfifo_put(&pbi->display_q, (const struct vframe_s *)vf);
			ATRACE_COUNTER(pbi->trace.pts_name, vf->timestamp);
			ATRACE_COUNTER(pbi->trace.new_q_name, kfifo_len(&pbi->newframe_q));
			ATRACE_COUNTER(pbi->trace.disp_q_name, kfifo_len(&pbi->display_q));
			atomic_add(1, &pbi->vf_pre_count);
			pbi_update_gvs(pbi);
			/*count info*/
			if (pbi->chunk)
				pbi->curr_pic_offset += pbi->chunk->size;
			else
				pbi->curr_pic_offset = stream_offset;
			vdec_count_info(pbi->gvs, 0, pbi->curr_pic_offset);

			if (stream_offset) {
				if (slice_type == KEY_FRAME) {
					pbi->gvs->i_decoded_frames++;
				} else if (slice_type == INTER_FRAME) {
					pbi->gvs->p_decoded_frames++;
				} else if (slice_type == FRAME_TYPES) {
					pbi->gvs->b_decoded_frames++;
				}
			}
			memcpy(&tmp4x, pbi->gvs, sizeof(struct vdec_info));
			tmp4x.bit_depth_luma = pbi->vp9_param.p.bit_depth;
			tmp4x.bit_depth_chroma = pbi->vp9_param.p.bit_depth;
			tmp4x.double_write_mode = pic_config->double_write_mode;
			vdec_fill_vdec_frame(pvdec, &pbi->vframe_qos, &tmp4x,
				vf, pic_config->hw_decode_time);
			pvdec->vdec_fps_detec(pvdec->id);
			v4l_vp9_update_frame_info(pbi, vf, pic_config);

			if ((v4l2_ctx->no_fbc_output &&
				(v4l2_ctx->picinfo.bitdepth != 0 &&
				v4l2_ctx->picinfo.bitdepth != 8)) ||
				v4l2_ctx->enable_di_post)
				v4l2_ctx->fbc_transcode_and_set_vf(v4l2_ctx,
					aml_buf, vf);

			if (without_display_mode == 0) {
				if (v4l2_ctx->is_stream_off) {
					vvp9_vf_put(vvp9_vf_get(pvdec), pvdec);
				} else {
					aml_buf_set_vframe(aml_buf, vf);
					vdec_tracing(&v4l2_ctx->vtr, VTRACE_DEC_PIC_0, aml_buf->index);
					aml_buf_done(&v4l2_ctx->bm, aml_buf, BUF_USER_DEC);
				}
			} else
				vvp9_vf_put(vvp9_vf_get(pvdec), pvdec);
		} else {
			pbi->stat |= VP9_TRIGGER_FRAME_DONE;
			hevc_source_changed(VFORMAT_VP9, 196, 196, 30);
			pr_debug("[%s %d] drop trigger frame width %d height %d  state 0x%x\n",
				__func__, __LINE__, vf->width,
				vf->height, pbi->stat);
		}
	}

	return 0;
}

static bool is_available_buffer(struct VP9Decoder_s *pbi);

static int notify_v4l_eos(struct vdec_s *vdec)
{
	struct VP9Decoder_s *hw = (struct VP9Decoder_s *)vdec->private;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(hw->v4l2_ctx);
	struct vframe_s *vf = &hw->vframe_dummy;
	struct aml_buf *aml_buf = NULL;
	int index = INVALID_IDX;
	ulong expires;

	expires = jiffies + msecs_to_jiffies(2000);
	while (!is_available_buffer(hw)) {
		if (time_after(jiffies, expires)) {
			pr_err("[%d] VP9 isn't enough buff for notify eos.\n", ctx->id);
			return 0;
		}
		usleep_range(500, 1000);
	}

	index = v4l_get_free_fb(hw);
	if (INVALID_IDX == index) {
		pr_err("[%d] VP9 EOS get free buff fail.\n", ctx->id);
		return 0;
	}

	aml_buf = index_to_aml_buf(hw, index);

	vf->type		|= VIDTYPE_V4L_EOS;
	vf->timestamp		= ULONG_MAX;
	vf->flag		= VFRAME_FLAG_EMPTY_FRAME_V4L;
	vf->v4l_mem_handle	= (ulong)aml_buf;

	vdec_vframe_ready(vdec, vf);
	aml_buf_set_vframe(aml_buf, vf);
	kfifo_put(&hw->display_q, (const struct vframe_s *)vf);

	vdec_tracing(&ctx->vtr, VTRACE_DEC_PIC_0, aml_buf->index);
	aml_buf_done(&ctx->bm, aml_buf, BUF_USER_DEC);

	hw->eos = true;

	pr_info("[%d] VP9 EOS notify.\n", ctx->id);

	return 0;
}

static void get_rpm_param(union param_u *params)
{
	int i;
	unsigned int data32;

	if (debug & VP9_DEBUG_BUFMGR)
		pr_info("enter %s\r\n", __func__);
	for (i = 0; i < 128; i++) {
		do {
			data32 = READ_VREG(RPM_CMD_REG);
		} while ((data32 & 0x10000) == 0);
		params->l.data[i] = data32&0xffff;
		WRITE_VREG(RPM_CMD_REG, 0);
	}
	if (debug & VP9_DEBUG_BUFMGR)
		pr_info("leave %s\r\n", __func__);
}
static void debug_buffer_mgr_more(struct VP9Decoder_s *pbi)
{
	int i;

	if (!(debug & VP9_DEBUG_DUMP_DATA))
		return;
	pr_info("vp9_param: (%d)\n", pbi->slice_idx);
	for (i = 0; i < (RPM_END-RPM_BEGIN); i++) {
		pr_info("%04x ", pbi->vp9_param.l.data[i]);
		if (((i + 1) & 0xf) == 0)
			pr_info("\n");
	}
	pr_info("=============param==========\r\n");
	pr_info("profile               %x\r\n", pbi->vp9_param.p.profile);
	pr_info("show_existing_frame   %x\r\n",
	pbi->vp9_param.p.show_existing_frame);
	pr_info("frame_to_show_idx     %x\r\n",
	pbi->vp9_param.p.frame_to_show_idx);
	pr_info("frame_type            %x\r\n", pbi->vp9_param.p.frame_type);
	pr_info("show_frame            %x\r\n", pbi->vp9_param.p.show_frame);
	pr_info("e.r.r.o.r_resilient_mode  %x\r\n",
	pbi->vp9_param.p.error_resilient_mode);
	pr_info("intra_only            %x\r\n", pbi->vp9_param.p.intra_only);
	pr_info("display_size_present  %x\r\n",
	pbi->vp9_param.p.display_size_present);
	pr_info("reset_frame_context   %x\r\n",
	pbi->vp9_param.p.reset_frame_context);
	pr_info("refresh_frame_flags   %x\r\n",
	pbi->vp9_param.p.refresh_frame_flags);
	pr_info("bit_depth             %x\r\n", pbi->vp9_param.p.bit_depth);
	pr_info("width                 %x\r\n", pbi->vp9_param.p.width);
	pr_info("height                %x\r\n", pbi->vp9_param.p.height);
	pr_info("display_width         %x\r\n", pbi->vp9_param.p.display_width);
	pr_info("display_height        %x\r\n", pbi->vp9_param.p.display_height);
	pr_info("ref_info              %x\r\n", pbi->vp9_param.p.ref_info);
	pr_info("same_frame_size       %x\r\n", pbi->vp9_param.p.same_frame_size);
	if (!(debug & VP9_DEBUG_DBG_LF_PRINT))
		return;
	pr_info("mode_ref_delta_enabled: 0x%x\r\n",
	pbi->vp9_param.p.mode_ref_delta_enabled);
	pr_info("sharpness_level: 0x%x\r\n",
	pbi->vp9_param.p.sharpness_level);
	pr_info("ref_deltas: 0x%x, 0x%x, 0x%x, 0x%x\r\n",
	pbi->vp9_param.p.ref_deltas[0], pbi->vp9_param.p.ref_deltas[1],
	pbi->vp9_param.p.ref_deltas[2], pbi->vp9_param.p.ref_deltas[3]);
	pr_info("mode_deltas: 0x%x, 0x%x\r\n", pbi->vp9_param.p.mode_deltas[0],
	pbi->vp9_param.p.mode_deltas[1]);
	pr_info("filter_level: 0x%x\r\n", pbi->vp9_param.p.filter_level);
	pr_info("seg_enabled: 0x%x\r\n", pbi->vp9_param.p.seg_enabled);
	pr_info("seg_abs_delta: 0x%x\r\n", pbi->vp9_param.p.seg_abs_delta);
	pr_info("seg_lf_feature_enabled: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\r\n",
	(pbi->vp9_param.p.seg_lf_info[0]>>15 & 1),
	(pbi->vp9_param.p.seg_lf_info[1]>>15 & 1),
	(pbi->vp9_param.p.seg_lf_info[2]>>15 & 1),
	(pbi->vp9_param.p.seg_lf_info[3]>>15 & 1),
	(pbi->vp9_param.p.seg_lf_info[4]>>15 & 1),
	(pbi->vp9_param.p.seg_lf_info[5]>>15 & 1),
	(pbi->vp9_param.p.seg_lf_info[6]>>15 & 1),
	(pbi->vp9_param.p.seg_lf_info[7]>>15 & 1));
	pr_info("seg_lf_feature_data: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\r\n",
	(pbi->vp9_param.p.seg_lf_info[0] & 0x13f),
	(pbi->vp9_param.p.seg_lf_info[1] & 0x13f),
	(pbi->vp9_param.p.seg_lf_info[2] & 0x13f),
	(pbi->vp9_param.p.seg_lf_info[3] & 0x13f),
	(pbi->vp9_param.p.seg_lf_info[4] & 0x13f),
	(pbi->vp9_param.p.seg_lf_info[5] & 0x13f),
	(pbi->vp9_param.p.seg_lf_info[6] & 0x13f),
	(pbi->vp9_param.p.seg_lf_info[7] & 0x13f));

}

static int recycle_mmu_buf_tail(struct VP9Decoder_s *pbi,
		bool check_dma)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct aml_buf *aml_buf;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);

	if (pbi->used_4k_num == -1) {
		pbi->used_4k_num =
			READ_VREG(HEVC_SAO_MMU_STATUS) >> 16;
	}
	vp9_print(pbi, VP9_DEBUG_BUFMGR_MORE,
		"afbc index %d page_start %d\n",
		cm->cur_fb_idx_mmu, pbi->used_4k_num);

	ctx->cal_compress_buff_info(pbi->used_4k_num, ctx);

	if (check_dma)
		hevc_mmu_dma_check(hw_to_vdec(pbi));

	aml_buf = index_to_afbc_aml_buf(pbi, cm->cur_fb_idx_mmu);

	if (aml_buf == NULL) {
		vp9_print(pbi,
			0,
			"[ERR]%s afbc index %d\n",
			__func__, cm->cur_fb_idx_mmu);
		cm->cur_fb_idx_mmu = INVALID_IDX;
		pbi->used_4k_num = -1;

		return 0;
	}

	decoder_mmu_box_free_idx_tail(
				aml_buf->fbc->mmu,
				aml_buf->fbc->index,
				pbi->used_4k_num);

	cm->cur_fb_idx_mmu = INVALID_IDX;
	pbi->used_4k_num = -1;

	return 0;
}

static void vp9_recycle_mmu_buf_tail(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	if (pbi->double_write_mode & 0x10)
		return;
	if (cm->cur_fb_idx_mmu != INVALID_IDX) {
		recycle_mmu_buf_tail(pbi,
			((pbi->used_4k_num == -1) &&
			pbi->m_ins_flag) ? 1 : 0);
	}
}

#ifdef MULTI_INSTANCE_SUPPORT
void vp9_recycle_mmu_work(struct work_struct *work)
{
	return;
}
#endif


static void dec_again_process(struct VP9Decoder_s *pbi)
{
	amhevc_stop();
	pbi->dec_result = DEC_RESULT_AGAIN;
	if (pbi->process_state ==
		PROC_STATE_DECODESLICE) {
		pbi->process_state =
		PROC_STATE_SENDAGAIN;
		if (pbi->mmu_enable) {
			/*
			 * Because vp9_recycle_mmu_buf has sleep function,we can't
			 * call it directly. Use a recycle_mmu_work to substitude it.
			 */
			vdec_schedule_work(&pbi->recycle_mmu_work);
		}
	}
	reset_process_time(pbi);
	vdec_schedule_work(&pbi->work);
}

int continue_decoding(struct VP9Decoder_s *pbi)
{
	int ret;
	int i;
	struct VP9_Common_s *const cm = &pbi->common;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	debug_buffer_mgr_more(pbi);

	if (ctx->param_sets_from_ucode)
		pbi->res_ch_flag = 0;
	bit_depth_luma = pbi->vp9_param.p.bit_depth;
	bit_depth_chroma = pbi->vp9_param.p.bit_depth;

	if ((pbi->vp9_param.p.bit_depth >= VPX_BITS_10) &&
		(get_double_write_mode(pbi) == 0x10)) {
		pbi->fatal_error |= DECODER_FATAL_ERROR_SIZE_OVERFLOW;
		pr_err("fatal err, bit_depth %d, unsupport dw 0x10\n",
			pbi->vp9_param.p.bit_depth);
		vdec_v4l_post_error_event(ctx, DECODER_EMERGENCY_UNSUPPORT);
		start_process_time(pbi);
		return -1;
	}

	if (pbi->process_state != PROC_STATE_SENDAGAIN) {
		ret = vp9_bufmgr_process(pbi, &pbi->vp9_param);
		if (!pbi->m_ins_flag)
			pbi->slice_idx++;
	} else {
		union param_u *params = &pbi->vp9_param;
		if (pbi->mmu_enable && ((pbi->double_write_mode & 0x10) == 0)) {
			for (i = 0; i < BUF_FBC_NUM_MAX; i++) {
				if (pbi->m_BUF[cm->new_fb_idx].v4l_ref_buf_addr
					== pbi->afbc_buf_table[i].fb) {
					cm->cur_fb_idx_mmu = i;
					vp9_print(pbi, VP9_DEBUG_BUFMGR,
						"cur fb idx mmu %d\n",
						i);
					break;
				}
			}

			if (i >= BUF_FBC_NUM_MAX) {
				vp9_print(pbi, 0, "[ERR]can't find fb(0x%lx) in afbc table\n",
						pbi->m_BUF[cm->new_fb_idx].v4l_ref_buf_addr);
				ret = -1;
			} else {
				ret = vp9_alloc_mmu(pbi,
					cm->cur_fb_idx_mmu,
					params->p.width,
					params->p.height,
					params->p.bit_depth,
					pbi->frame_mmu_map_addr);
			}

			if (ret < 0) {
				pr_err("can't alloc need mmu1,idx %d ret =%d\n",
					cm->new_fb_idx,
					ret);
			}
		} else {
			ret = 0;
		}
		WRITE_VREG(HEVC_PARSER_PICTURE_SIZE,
		(params->p.height << 16) | params->p.width);
	}

	if (ret < 0) {
		pr_info("vp9_bufmgr_process=> %d, VP9_10B_DISCARD_NAL\r\n", ret);
		WRITE_VREG(HEVC_DEC_STATUS_REG, VP9_10B_DISCARD_NAL);
		cm->show_frame = 0;

		if (ret != -3) {
			vp9_buf_ref_process_for_exception(pbi);
			if (vdec_frame_based(hw_to_vdec(pbi)))
				vdec_v4l_post_error_frame_event(ctx);
		}
#ifdef MULTI_INSTANCE_SUPPORT
		if (pbi->m_ins_flag) {
			reset_process_time(pbi);
			pbi->dec_result = DEC_RESULT_DONE;
#ifdef SUPPORT_FB_DECODING
			if (pbi->used_stage_buf_num == 0)
#endif
				amhevc_stop();
			vdec_schedule_work(&pbi->work);
		}
#endif
		return ret;
	} else if (ret == 0) {
		struct PIC_BUFFER_CONFIG_s *cur_pic_config
			= &cm->cur_frame->buf;
		cur_pic_config->decode_idx = pbi->frame_count;

		if (pbi->process_state != PROC_STATE_SENDAGAIN) {
			if (!pbi->m_ins_flag) {
				pbi->frame_count++;
				decode_frame_count[pbi->index]
					= pbi->frame_count;
			}
#ifdef MULTI_INSTANCE_SUPPORT
			if (pbi->chunk) {
				cur_pic_config->pts = pbi->chunk->pts;
				cur_pic_config->pts64 = pbi->chunk->pts64;

				if (!v4l_bitstream_id_enable)
					cur_pic_config->pts64 = pbi->chunk->timestamp;
			}
#endif
		}
		/*pr_info("Decode Frame Data %d\n", pbi->frame_count);*/
		ATRACE_COUNTER(pbi->trace.decode_header_memory_time_name, TRACE_HEADER_REGISTER_START);
		config_pic_size(pbi, pbi->vp9_param.p.bit_depth);

		if ((pbi->common.frame_type != KEY_FRAME)
			&& (!pbi->common.intra_only)) {
			config_mc_buffer(pbi, pbi->vp9_param.p.bit_depth);
#ifdef SUPPORT_FB_DECODING
			if (pbi->used_stage_buf_num == 0)
#endif
				config_mpred_hw(pbi);
		} else {
#ifdef SUPPORT_FB_DECODING
			if (pbi->used_stage_buf_num == 0)
#endif
				clear_mpred_hw(pbi);
		}
#ifdef MCRCC_ENABLE
		if (mcrcc_cache_alg_flag)
			config_mcrcc_axi_hw_new(pbi);
		else
			config_mcrcc_axi_hw(pbi);
#endif
		config_sao_hw(pbi, &pbi->vp9_param);

#ifdef VP9_LPF_LVL_UPDATE
		/*
		* Get loop filter related picture level parameters from Parser
		*/
		pbi->lf->mode_ref_delta_enabled = pbi->vp9_param.p.mode_ref_delta_enabled;
		pbi->lf->sharpness_level = pbi->vp9_param.p.sharpness_level;
		for (i = 0; i < 4; i++)
			pbi->lf->ref_deltas[i] = pbi->vp9_param.p.ref_deltas[i];
		for (i = 0; i < 2; i++)
			pbi->lf->mode_deltas[i] = pbi->vp9_param.p.mode_deltas[i];
		pbi->default_filt_lvl = pbi->vp9_param.p.filter_level;
		pbi->seg_4lf->enabled = pbi->vp9_param.p.seg_enabled;
		pbi->seg_4lf->abs_delta = pbi->vp9_param.p.seg_abs_delta;
		for (i = 0; i < MAX_SEGMENTS; i++)
			pbi->seg_4lf->feature_mask[i] = (pbi->vp9_param.p.seg_lf_info[i] &
			0x8000) ? (1 << SEG_LVL_ALT_LF) : 0;
		for (i = 0; i < MAX_SEGMENTS; i++)
			pbi->seg_4lf->feature_data[i][SEG_LVL_ALT_LF]
			= (pbi->vp9_param.p.seg_lf_info[i]
			& 0x100) ? -(pbi->vp9_param.p.seg_lf_info[i]
			& 0x3f) : (pbi->vp9_param.p.seg_lf_info[i] & 0x3f);
		if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) {
			/*Set pipeline mode*/
			uint32_t lpf_data32 = READ_VREG(HEVC_DBLK_CFGB);
			/*dblk pipeline mode=1 for performance*/
			if (pbi->vp9_param.p.width >= 1280)
				lpf_data32 |= (0x1 << 4);
			else
				lpf_data32 &= ~(0x3 << 4);
			WRITE_VREG(HEVC_DBLK_CFGB, lpf_data32);
		}
		/*
		* Update loop filter Thr/Lvl table for every frame
		*/
		vp9_loop_filter_frame_init(pbi->seg_4lf,
			pbi->lfi, pbi->lf, pbi->default_filt_lvl);
#endif
		WRITE_VREG(HEVC_DEC_STATUS_REG, VP9_10B_DECODE_SLICE);
	} else {
		pr_info("Skip search next start code\n");
		cm->prev_fb_idx = INVALID_IDX;
		/*skip, search next start code*/
		WRITE_VREG(HEVC_DEC_STATUS_REG, VP9_10B_DECODE_SLICE);
	}
	pbi->process_state = PROC_STATE_DECODESLICE;
	ATRACE_COUNTER(pbi->trace.decode_header_memory_time_name, TRACE_HEADER_REGISTER_START);
	start_process_time(pbi);
	return ret;
}

static void fill_frame_info(struct VP9Decoder_s *pbi,
	struct PIC_BUFFER_CONFIG_s *frame,
	unsigned int framesize,
	unsigned int pts)
{
	struct vframe_qos_s *vframe_qos = &pbi->vframe_qos;

	if (frame->slice_type == KEY_FRAME)
		vframe_qos->type = 1;
	else if (frame->slice_type == INTER_FRAME)
		vframe_qos->type = 2;

	if (input_frame_based(hw_to_vdec(pbi)))
		vframe_qos->size = frame->frame_size2;
	else
		vframe_qos->size = framesize;
	vframe_qos->pts = pts;
#ifdef SHOW_QOS_INFO
	vp9_print(pbi, 0, "slice:%d\n", frame->slice_type);
#endif
	vframe_qos->max_mv = frame->max_mv;
	vframe_qos->avg_mv = frame->avg_mv;
	vframe_qos->min_mv = frame->min_mv;
#ifdef SHOW_QOS_INFO
	vp9_print(pbi, 0, "mv: max:%d,  avg:%d, min:%d\n",
			vframe_qos->max_mv,
			vframe_qos->avg_mv,
			vframe_qos->min_mv);
#endif
	vframe_qos->max_qp = frame->max_qp;
	vframe_qos->avg_qp = frame->avg_qp;
	vframe_qos->min_qp = frame->min_qp;
#ifdef SHOW_QOS_INFO
	vp9_print(pbi, 0, "qp: max:%d,  avg:%d, min:%d\n",
			vframe_qos->max_qp,
			vframe_qos->avg_qp,
			vframe_qos->min_qp);
#endif
	vframe_qos->max_skip = frame->max_skip;
	vframe_qos->avg_skip = frame->avg_skip;
	vframe_qos->min_skip = frame->min_skip;
#ifdef SHOW_QOS_INFO
	vp9_print(pbi, 0, "skip: max:%d,	avg:%d, min:%d\n",
			vframe_qos->max_skip,
			vframe_qos->avg_skip,
			vframe_qos->min_skip);
#endif
	vframe_qos->num++;
}

static void v4l_vp9_collect_stream_info(struct vdec_s *vdec,
	struct VP9Decoder_s *pbi)
{
	struct aml_vcodec_ctx *ctx = pbi->v4l2_ctx;
	struct dec_stream_info_s *str_info = NULL;

	if (ctx == NULL) {
		pr_info("param invalid\n");
		return;
	}
	str_info = &ctx->dec_intf.dec_stream;

	snprintf(str_info->vdec_name, sizeof(str_info->vdec_name),
		"%s", DRIVER_NAME);

	str_info->vdec_type = input_frame_based(vdec);
	str_info->dual_core_flag = vdec_dual(vdec);
	str_info->is_secure = vdec_secure(vdec);
	str_info->profile_idc = pbi->param.p.profile;
	str_info->filed_flag = 0;
	str_info->frame_height = pbi->frame_height;
	str_info->frame_width = pbi->frame_width;
	str_info->crop_top = 0;
	str_info->crop_bottom = 0;
	str_info->crop_left= 0;
	str_info->crop_right = 0;
	str_info->double_write_mode = pbi->double_write_mode;
	str_info->error_handle_policy = error_handle_policy;
	str_info->bit_depth = pbi->param.p.bit_depth;
	str_info->fence_enable = pbi->enable_fence;
	str_info->ratio_size.sar_width = -1;
	str_info->ratio_size.sar_height = -1;
	str_info->ratio_size.dar_width = -1;
	str_info->ratio_size.dar_height = -1;
	if (pbi->frame_dur != 0)
		str_info->frame_rate = ((96000 * 10 / pbi->frame_dur) % 10) < 5 ?
				96000 / pbi->frame_dur : (96000 / pbi->frame_dur +1);
	else
		str_info->frame_rate = -1;
	ctx->dec_intf.decinfo_event_report(ctx, AML_DECINFO_EVENT_STREAM, NULL);
}

/* only when we decoded one field or one frame,
we can call this function to get qos info*/
static void get_picture_qos_info(struct VP9Decoder_s *pbi)
{
	struct PIC_BUFFER_CONFIG_s *frame = &pbi->cur_buf->buf;
	struct vdec_s *vdec = hw_to_vdec(pbi);

	if (!frame)
		return;
	if (vdec->mvfrm) {
		frame->frame_size2 = vdec->mvfrm->frame_size;
		frame->hw_decode_time =
		local_clock() - vdec->mvfrm->hw_decode_start;
	}

	if (get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_G12A) {
		unsigned char a[3];
		unsigned char i, j, t;
		unsigned long  data;

		data = READ_VREG(HEVC_MV_INFO);
		if (frame->slice_type == KEY_FRAME)
			data = 0;
		a[0] = data & 0xff;
		a[1] = (data >> 8) & 0xff;
		a[2] = (data >> 16) & 0xff;

		for (i = 0; i < 3; i++) {
			for (j = i+1; j < 3; j++) {
				if (a[j] < a[i]) {
					t = a[j];
					a[j] = a[i];
					a[i] = t;
				} else if (a[j] == a[i]) {
					a[i]++;
					t = a[j];
					a[j] = a[i];
					a[i] = t;
				}
			}
		}
		frame->max_mv = a[2];
		frame->avg_mv = a[1];
		frame->min_mv = a[0];

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"mv data %x  a[0]= %x a[1]= %x a[2]= %x\n",
			data, a[0], a[1], a[2]);

		data = READ_VREG(HEVC_QP_INFO);
		a[0] = data & 0x1f;
		a[1] = (data >> 8) & 0x3f;
		a[2] = (data >> 16) & 0x7f;

		for (i = 0; i < 3; i++) {
			for (j = i+1; j < 3; j++) {
				if (a[j] < a[i]) {
					t = a[j];
					a[j] = a[i];
					a[i] = t;
				} else if (a[j] == a[i]) {
					a[i]++;
					t = a[j];
					a[j] = a[i];
					a[i] = t;
				}
			}
		}
		frame->max_qp = a[2];
		frame->avg_qp = a[1];
		frame->min_qp = a[0];

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"qp data %x  a[0]= %x a[1]= %x a[2]= %x\n",
			data, a[0], a[1], a[2]);

		data = READ_VREG(HEVC_SKIP_INFO);
		a[0] = data & 0x1f;
		a[1] = (data >> 8) & 0x3f;
		a[2] = (data >> 16) & 0x7f;

		for (i = 0; i < 3; i++) {
			for (j = i+1; j < 3; j++) {
				if (a[j] < a[i]) {
					t = a[j];
					a[j] = a[i];
					a[i] = t;
				} else if (a[j] == a[i]) {
					a[i]++;
					t = a[j];
					a[j] = a[i];
					a[i] = t;
				}
			}
		}
		frame->max_skip = a[2];
		frame->avg_skip = a[1];
		frame->min_skip = a[0];

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"skip data %x  a[0]= %x a[1]= %x a[2]= %x\n",
			data, a[0], a[1], a[2]);
	} else {
		uint32_t blk88_y_count;
		uint32_t blk88_c_count;
		uint32_t blk22_mv_count;
		uint32_t rdata32;
		int32_t mv_hi;
		int32_t mv_lo;
		uint32_t rdata32_l;
		uint32_t mvx_L0_hi;
		uint32_t mvy_L0_hi;
		uint32_t mvx_L1_hi;
		uint32_t mvy_L1_hi;
		int64_t value;
		uint64_t temp_value;
		int pic_number = frame->decode_idx;

		frame->max_mv = 0;
		frame->avg_mv = 0;
		frame->min_mv = 0;

		frame->max_skip = 0;
		frame->avg_skip = 0;
		frame->min_skip = 0;

		frame->max_qp = 0;
		frame->avg_qp = 0;
		frame->min_qp = 0;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO, "slice_type:%d, poc:%d\n",
			frame->slice_type,
			pic_number);

		/* set rd_idx to 0 */
		WRITE_VREG(HEVC_PIC_QUALITY_CTRL, 0);

		blk88_y_count = READ_VREG(HEVC_PIC_QUALITY_DATA);
		if (blk88_y_count == 0) {

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] NO Data yet.\n",
			pic_number);

			/* reset all counts */
			WRITE_VREG(HEVC_PIC_QUALITY_CTRL, (1<<8));
			return;
		}
		/* qp_y_sum */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] Y QP AVG : %d (%d/%d)\n",
			pic_number, rdata32/blk88_y_count,
			rdata32, blk88_y_count);

		frame->avg_qp = rdata32/blk88_y_count;
		/* intra_y_count */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] Y intra rate : %d%c (%d)\n",
			pic_number, rdata32*100/blk88_y_count,
			'%', rdata32);

		/* skipped_y_count */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] Y skipped rate : %d%c (%d)\n",
			pic_number, rdata32*100/blk88_y_count,
			'%', rdata32);

		frame->avg_skip = rdata32*100/blk88_y_count;
		/* coeff_non_zero_y_count */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] Y ZERO_Coeff rate : %d%c (%d)\n",
			pic_number, (100 - rdata32*100/(blk88_y_count*1)),
			'%', rdata32);

		/* blk66_c_count */
		blk88_c_count = READ_VREG(HEVC_PIC_QUALITY_DATA);
		if (blk88_c_count == 0) {
			vp9_print(pbi, VP9_DEBUG_QOS_INFO,
				"[Picture %d Quality] NO Data yet.\n",
				pic_number);
			/* reset all counts */
			WRITE_VREG(HEVC_PIC_QUALITY_CTRL, (1<<8));
			return;
		}
		/* qp_c_sum */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
				"[Picture %d Quality] C QP AVG : %d (%d/%d)\n",
				pic_number, rdata32/blk88_c_count,
				rdata32, blk88_c_count);

		/* intra_c_count */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] C intra rate : %d%c (%d)\n",
			pic_number, rdata32*100/blk88_c_count,
			'%', rdata32);

		/* skipped_cu_c_count */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] C skipped rate : %d%c (%d)\n",
			pic_number, rdata32*100/blk88_c_count,
			'%', rdata32);

		/* coeff_non_zero_c_count */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] C ZERO_Coeff rate : %d%c (%d)\n",
			pic_number, (100 - rdata32*100/(blk88_c_count*1)),
			'%', rdata32);

		/* 1'h0, qp_c_max[6:0], 1'h0, qp_c_min[6:0],
		1'h0, qp_y_max[6:0], 1'h0, qp_y_min[6:0] */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] Y QP min : %d\n",
			pic_number, (rdata32>>0)&0xff);

		frame->min_qp = (rdata32>>0)&0xff;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] Y QP max : %d\n",
			pic_number, (rdata32>>8)&0xff);

		frame->max_qp = (rdata32>>8)&0xff;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] C QP min : %d\n",
			pic_number, (rdata32>>16)&0xff);
		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] C QP max : %d\n",
			pic_number, (rdata32>>24)&0xff);

		/* blk22_mv_count */
		blk22_mv_count = READ_VREG(HEVC_PIC_QUALITY_DATA);
		if (blk22_mv_count == 0) {
			vp9_print(pbi, VP9_DEBUG_QOS_INFO,
				"[Picture %d Quality] NO MV Data yet.\n",
				pic_number);
			/* reset all counts */
			WRITE_VREG(HEVC_PIC_QUALITY_CTRL, (1<<8));
			return;
		}
		/* mvy_L1_count[39:32], mvx_L1_count[39:32],
		mvy_L0_count[39:32], mvx_L0_count[39:32] */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);
		/* should all be 0x00 or 0xff */
		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MV AVG High Bits: 0x%X\n",
			pic_number, rdata32);

		mvx_L0_hi = ((rdata32>>0)&0xff);
		mvy_L0_hi = ((rdata32>>8)&0xff);
		mvx_L1_hi = ((rdata32>>16)&0xff);
		mvy_L1_hi = ((rdata32>>24)&0xff);

		/* mvx_L0_count[31:0] */
		rdata32_l = READ_VREG(HEVC_PIC_QUALITY_DATA);
		temp_value = mvx_L0_hi;
		temp_value = (temp_value << 32) | rdata32_l;

		if (mvx_L0_hi & 0x80)
			value = 0xFFFFFFF000000000 | temp_value;
		else
			value = temp_value;

		value = div_s64(value, blk22_mv_count);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVX_L0 AVG : %d (%lld/%d)\n",
			pic_number, (int)value,
			value, blk22_mv_count);

		frame->avg_mv = value;

		/* mvy_L0_count[31:0] */
		rdata32_l = READ_VREG(HEVC_PIC_QUALITY_DATA);
		temp_value = mvy_L0_hi;
		temp_value = (temp_value << 32) | rdata32_l;

		if (mvy_L0_hi & 0x80)
			value = 0xFFFFFFF000000000 | temp_value;
		else
			value = temp_value;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVY_L0 AVG : %d (%lld/%d)\n",
			pic_number, rdata32_l/blk22_mv_count,
			value, blk22_mv_count);

		/* mvx_L1_count[31:0] */
		rdata32_l = READ_VREG(HEVC_PIC_QUALITY_DATA);
		temp_value = mvx_L1_hi;
		temp_value = (temp_value << 32) | rdata32_l;
		if (mvx_L1_hi & 0x80)
			value = 0xFFFFFFF000000000 | temp_value;
		else
			value = temp_value;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVX_L1 AVG : %d (%lld/%d)\n",
			pic_number, rdata32_l/blk22_mv_count,
			value, blk22_mv_count);

		/* mvy_L1_count[31:0] */
		rdata32_l = READ_VREG(HEVC_PIC_QUALITY_DATA);
		temp_value = mvy_L1_hi;
		temp_value = (temp_value << 32) | rdata32_l;
		if (mvy_L1_hi & 0x80)
			value = 0xFFFFFFF000000000 | temp_value;
		else
			value = temp_value;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVY_L1 AVG : %d (%lld/%d)\n",
			pic_number, rdata32_l/blk22_mv_count,
			value, blk22_mv_count);

		/* {mvx_L0_max, mvx_L0_min} // format : {sign, abs[14:0]}  */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);
		mv_hi = (rdata32>>16)&0xffff;
		if (mv_hi & 0x8000)
			mv_hi = 0x8000 - mv_hi;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVX_L0 MAX : %d\n",
			pic_number, mv_hi);

		frame->max_mv = mv_hi;

		mv_lo = (rdata32>>0)&0xffff;
		if (mv_lo & 0x8000)
			mv_lo = 0x8000 - mv_lo;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVX_L0 MIN : %d\n",
			pic_number, mv_lo);

		frame->min_mv = mv_lo;

		/* {mvy_L0_max, mvy_L0_min} */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);
		mv_hi = (rdata32>>16)&0xffff;
		if (mv_hi & 0x8000)
			mv_hi = 0x8000 - mv_hi;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVY_L0 MAX : %d\n",
			pic_number, mv_hi);

		mv_lo = (rdata32>>0)&0xffff;
		if (mv_lo & 0x8000)
			mv_lo = 0x8000 - mv_lo;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVY_L0 MIN : %d\n",
			pic_number, mv_lo);

		/* {mvx_L1_max, mvx_L1_min} */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);
		mv_hi = (rdata32>>16)&0xffff;
		if (mv_hi & 0x8000)
			mv_hi = 0x8000 - mv_hi;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVX_L1 MAX : %d\n",
			pic_number, mv_hi);

		mv_lo = (rdata32>>0)&0xffff;
		if (mv_lo & 0x8000)
			mv_lo = 0x8000 - mv_lo;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVX_L1 MIN : %d\n",
			pic_number, mv_lo);

		/* {mvy_L1_max, mvy_L1_min} */
		rdata32 = READ_VREG(HEVC_PIC_QUALITY_DATA);
		mv_hi = (rdata32>>16)&0xffff;
		if (mv_hi & 0x8000)
			mv_hi = 0x8000 - mv_hi;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVY_L1 MAX : %d\n",
			pic_number, mv_hi);

		mv_lo = (rdata32>>0)&0xffff;
		if (mv_lo & 0x8000)
			mv_lo = 0x8000 - mv_lo;

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] MVY_L1 MIN : %d\n",
			pic_number, mv_lo);

		rdata32 = READ_VREG(HEVC_PIC_QUALITY_CTRL);

		vp9_print(pbi, VP9_DEBUG_QOS_INFO,
			"[Picture %d Quality] After Read : VDEC_PIC_QUALITY_CTRL : 0x%x\n",
			pic_number, rdata32);

		/* reset all counts */
		WRITE_VREG(HEVC_PIC_QUALITY_CTRL, (1<<8));
	}
}

static void vvp9_get_comp_buf_info(struct VP9Decoder_s *pbi,
					struct vdec_comp_buf_info *info)
{
	u16 bit_depth = pbi->vp9_param.p.bit_depth;

	info->max_size = vp9_max_mmu_buf_size(
			pbi->max_pic_w,
			pbi->max_pic_h);
	info->header_size = vvp9_mmu_compress_header_size(
			pbi->frame_width,
			pbi->frame_height);
	info->frame_buffer_size = vp9_mmu_page_num(
			pbi, pbi->frame_width,
			pbi->frame_height,
			bit_depth == VPX_BITS_10);
}

static int vvp9_get_ps_info(struct VP9Decoder_s *pbi, struct aml_vdec_ps_infos *ps)
{
	ps->visible_width 	= pbi->frame_width;
	ps->visible_height 	= pbi->frame_height;
	ps->coded_width 	= ALIGN(pbi->frame_width, is_hevc_align32(0) ? 32 : 64);
	ps->coded_height 	= ALIGN(pbi->frame_height, 64);
	ps->dpb_size 		= pbi->used_buf_num;
	ps->dpb_margin	= pbi->dynamic_buf_num_margin;
	if (pbi->frame_width > 2048 && pbi->frame_height > 1088)
		ps->dpb_frames	= 4;	 /* > level 4.1*/
	else
		ps->dpb_frames	= 8;	/* < level 4.1 */
	/*
	1. current decoding frame is not include in dpb;
	2. for frame push out, one more buffer necessary.
	3. Two consecutive frames cannot use the same buffer.
	*/
	ps->dpb_frames += 3;

	if (ps->dpb_margin + ps->dpb_frames > MAX_BUF_NUM_NORMAL) {
		u32 delta;
		delta = ps->dpb_margin + ps->dpb_frames - MAX_BUF_NUM_NORMAL;
		ps->dpb_margin -= delta;
	}
	ps->field = V4L2_FIELD_NONE;
	ps->bitdepth = pbi->vp9_param.p.bit_depth;

	return 0;
}


static int v4l_res_change(struct VP9Decoder_s *pbi)
{
	struct aml_vcodec_ctx *ctx =
		(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	int ret = 0;

	if (ctx->param_sets_from_ucode &&
		pbi->res_ch_flag == 0) {
		struct aml_vdec_ps_infos ps;
		struct vdec_comp_buf_info comp;
		struct VP9_Common_s *cm = &pbi->common;

		if ((pbi->last_width != 0 &&
			pbi->last_height != 0) &&
			(pbi->frame_width != pbi->last_width ||
			pbi->frame_height != pbi->last_height)) {

			vp9_print(pbi, 0, "%s (%d,%d)=>(%d,%d)\r\n", __func__, pbi->last_width,
				pbi->last_height, pbi->frame_width, pbi->frame_height);

			if (get_valid_double_write_mode(pbi) != 16) {
				vvp9_get_comp_buf_info(pbi, &comp);
				vdec_v4l_set_comp_buf_info(ctx, &comp);
			}

			vvp9_get_ps_info(pbi, &ps);
			vdec_v4l_set_ps_infos(ctx, &ps);
			vdec_v4l_res_ch_event(ctx);

			pbi->init_pic_w = pbi->frame_width;
			pbi->init_pic_h = pbi->frame_height;
			init_mv_buf_list(pbi);

			pbi->process_state = PROC_STATE_INIT;
			pbi->v4l_params_parsed = false;
			pbi->res_ch_flag = 1;
			ctx->v4l_resolution_change = 1;
			if (cm->new_fb_idx != INVALID_IDX)
				vp9_bufmgr_postproc(pbi);

			vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_4, __LINE__);
			notify_v4l_eos(hw_to_vdec(pbi));
			vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_4, 0);

			ret = 1;
		}
	}

	return ret;
}

static void vp9_buf_ref_process_for_exception(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	struct aml_vcodec_ctx *ctx =
		(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	struct aml_buf *aml_buf;

	if (pbi->cur_idx != INVALID_IDX) {
		int cur_idx = pbi->cur_idx;
		aml_buf = (struct aml_buf *)pbi->m_BUF[cur_idx].v4l_ref_buf_addr;
		if (aml_buf == NULL) {
			pbi->cur_idx = INVALID_IDX;
			vp9_print(pbi, 0,
				"%s aml_buf is NULL\n", __func__);
			return;
		}


		vp9_print(pbi, 0,
			"process_for_exception: dma addr(0x%lx)\n",
			frame_bufs[cur_idx].buf.cma_alloc_addr);

		aml_buf_put_ref(&ctx->bm, aml_buf);
		aml_buf_put_ref(&ctx->bm, aml_buf);

		frame_bufs[cur_idx].buf.cma_alloc_addr = 0;
		frame_bufs[cur_idx].buf.vf_ref = 0;
		pbi->m_BUF[cur_idx].v4l_ref_buf_addr = 0;

		pbi->cur_idx = INVALID_IDX;
	}
}

static irqreturn_t vvp9_isr_thread_fn(int irq, void *data)
{
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)data;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	unsigned int dec_status = pbi->dec_status;
	int i;
	uint debug_tag;

	if (dec_status == VP9_HEAD_PARSER_DONE) {
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_THREAD_HEAD_START);
	}
	else if (dec_status == HEVC_DECPIC_DATA_DONE) {
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_THREAD_PIC_DONE_START);
	}

	if (debug & VP9_DEBUG_BUFMGR)
		pr_info("vp9 isr (%d) dec status  = 0x%x, lcu 0x%x shiftbyte 0x%x (%x %x lev %x, wr %x, rd %x)\n",
			irq,
			dec_status, READ_VREG(HEVC_PARSER_LCU_START),
			READ_VREG(HEVC_SHIFT_BYTE_COUNT),
			READ_VREG(HEVC_STREAM_START_ADDR),
			READ_VREG(HEVC_STREAM_END_ADDR),
			READ_VREG(HEVC_STREAM_LEVEL),
			READ_VREG(HEVC_STREAM_WR_PTR),
			READ_VREG(HEVC_STREAM_RD_PTR)
		);

	debug_tag = READ_HREG(DEBUG_REG1);
	if (debug_tag & 0x10000) {
		pr_info("LMEM<tag %x>:\n", READ_HREG(DEBUG_REG1));
		for (i = 0; i < 0x400; i += 4) {
			int ii;
			if ((i & 0xf) == 0)
				pr_info("%03x: ", i);
			for (ii = 0; ii < 4; ii++) {
				pr_info("%04x ",
					   pbi->lmem_ptr[i + 3 - ii]);
			}
			if (((i + ii) & 0xf) == 0)
				pr_info("\n");
		}

		if ((udebug_pause_pos == (debug_tag & 0xffff)) &&
			(udebug_pause_decode_idx == 0 ||
			udebug_pause_decode_idx == pbi->slice_idx) &&
			(udebug_pause_val == 0 ||
			udebug_pause_val == READ_HREG(DEBUG_REG2)))
			pbi->ucode_pause_pos = udebug_pause_pos;
		else if (debug_tag & 0x20000)
			pbi->ucode_pause_pos = 0xffffffff;
		if (pbi->ucode_pause_pos)
			reset_process_time(pbi);
		else
			WRITE_HREG(DEBUG_REG1, 0);
	} else if (debug_tag != 0) {
		pr_info(
			"dbg%x: %x lcu %x\n", READ_HREG(DEBUG_REG1),
			   READ_HREG(DEBUG_REG2),
			   READ_VREG(HEVC_PARSER_LCU_START));
		if ((udebug_pause_pos == (debug_tag & 0xffff)) &&
			(udebug_pause_decode_idx == 0 ||
			udebug_pause_decode_idx == pbi->slice_idx) &&
			(udebug_pause_val == 0 ||
			udebug_pause_val == READ_HREG(DEBUG_REG2)))
			pbi->ucode_pause_pos = udebug_pause_pos;
		if (pbi->ucode_pause_pos)
			reset_process_time(pbi);
		else
			WRITE_HREG(DEBUG_REG1, 0);
		pbi->process_busy = 0;
		return IRQ_HANDLED;
	}

	if (pbi->eos)
		return IRQ_HANDLED;
	pbi->wait_buf = 0;
#ifdef MULTI_INSTANCE_SUPPORT
#ifdef SUPPORT_FB_DECODING
#ifdef FB_DECODING_TEST_SCHEDULE
	if (pbi->s1_test_cmd == TEST_SET_PIC_DONE)
		dec_status = HEVC_DECPIC_DATA_DONE;
	else if (pbi->s1_test_cmd == TEST_SET_S2_DONE
		&& dec_status == HEVC_DECPIC_DATA_DONE)
		dec_status = HEVC_S2_DECODING_DONE;
	pbi->s1_test_cmd = TEST_SET_NONE;
#endif
	if (dec_status == HEVC_S2_DECODING_DONE) {
		pbi->dec_result = DEC_RESULT_DONE;
		vdec_schedule_work(&pbi->work);
#ifdef FB_DECODING_TEST_SCHEDULE
		amhevc_stop();
		pbi->dec_s1_result = DEC_S1_RESULT_DONE;
		vdec_schedule_work(&pbi->s1_work);
#endif
	} else
#endif
	if ((dec_status == HEVC_NAL_DECODE_DONE) ||
			(dec_status == HEVC_SEARCH_BUFEMPTY) ||
			(dec_status == HEVC_DECODE_BUFEMPTY)
		) {
		if (pbi->m_ins_flag) {
			reset_process_time(pbi);
			if (!vdec_frame_based(hw_to_vdec(pbi))) {
				vp9_buf_ref_process_for_exception(pbi);
				dec_again_process(pbi);
			} else {
				if (pbi->common.show_existing_frame) {
					pbi->dec_result = DEC_RESULT_DONE;
					amhevc_stop();
					vdec_schedule_work(&pbi->work);
				}
				else {
					pbi->dec_result = DEC_RESULT_GET_DATA;
					if (vdec_frame_based(hw_to_vdec(pbi)))
						vdec_v4l_post_error_frame_event(ctx);
					vdec_schedule_work(&pbi->work);
				}
			}
		}
		pbi->process_busy = 0;
		return IRQ_HANDLED;
	} else if (dec_status == HEVC_DECPIC_DATA_DONE) {
		vdec_profile(hw_to_vdec(pbi), VDEC_PROFILE_DECODED_FRAME, CORE_MASK_HEVC);
		if (pbi->m_ins_flag) {
			get_picture_qos_info(pbi);
#ifdef SUPPORT_FB_DECODING
			if (pbi->used_stage_buf_num > 0) {
				reset_process_time(pbi);
				inc_s1_pos(pbi);
				trigger_schedule(pbi);
#ifdef FB_DECODING_TEST_SCHEDULE
				pbi->s1_test_cmd = TEST_SET_S2_DONE;
#else
				amhevc_stop();
				pbi->dec_s1_result = DEC_S1_RESULT_DONE;
				vdec_schedule_work(&pbi->s1_work);
#endif
			} else
#endif
			{
				reset_process_time(pbi);
				if (atomic_read(&pbi->vf_pre_count) == 0 || pbi->low_latency_flag)
					vp9_bufmgr_postproc(pbi);

				pbi->dec_result = DEC_RESULT_DONE;
				amhevc_stop();
				if (vdec_frame_based(hw_to_vdec(pbi)) &&
					(READ_VREG(HEVC_SHIFT_BYTE_COUNT) + 4 < pbi->data_size)) {
					pbi->consume_byte = READ_VREG(HEVC_SHIFT_BYTE_COUNT);
					pbi->dec_result = DEC_RESULT_UNFINISH;
				} else {
					pbi->data_size = 0;
					pbi->data_offset = 0;
				}
				if (mcrcc_cache_alg_flag)
					dump_hit_rate(pbi);
				ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_THREAD_EDN);
				vdec_schedule_work(&pbi->work);
			}
		} else {
			if (pbi->low_latency_flag) {
				vp9_bufmgr_postproc(pbi);
				WRITE_VREG(HEVC_DEC_STATUS_REG, HEVC_ACTION_DONE);
#ifdef CONFIG_AMLOGIC_MEDIA_MULTI_DEC
				vdec_profile(hw_to_vdec(pbi), VDEC_PROFILE_EVENT_CB, 0);
				if (debug & PRINT_FLAG_VDEC_DETAIL)
					pr_info("%s VP9 frame done \n", __func__);
#endif
			}
		}

		pbi->process_busy = 0;
		return IRQ_HANDLED;
	}
#endif

	if (dec_status == VP9_EOS) {
#ifdef MULTI_INSTANCE_SUPPORT
		if (pbi->m_ins_flag)
			reset_process_time(pbi);
#endif

		pr_info("VP9_EOS, flush buffer\r\n");

		vp9_bufmgr_postproc(pbi);

		pr_info("send VP9_10B_DISCARD_NAL\r\n");
		WRITE_VREG(HEVC_DEC_STATUS_REG, VP9_10B_DISCARD_NAL);
		pbi->process_busy = 0;
#ifdef MULTI_INSTANCE_SUPPORT
		if (pbi->m_ins_flag) {
			pbi->dec_result = DEC_RESULT_DONE;
			amhevc_stop();
			vdec_schedule_work(&pbi->work);
		}
#endif
		return IRQ_HANDLED;
	} else if (dec_status == HEVC_DECODE_OVER_SIZE) {
		struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
		pr_info("vp9  decode oversize !!\n");
		debug |= (VP9_DEBUG_DIS_LOC_ERROR_PROC |
			VP9_DEBUG_DIS_SYS_ERROR_PROC);
		pbi->fatal_error |= DECODER_FATAL_ERROR_SIZE_OVERFLOW;
		vdec_v4l_post_error_event(ctx, DECODER_WARNING_DATA_ERROR);
		if (vdec_frame_based(hw_to_vdec(pbi)))
			vdec_v4l_post_error_frame_event(ctx);
#ifdef MULTI_INSTANCE_SUPPORT
	if (pbi->m_ins_flag)
		reset_process_time(pbi);
#endif
		return IRQ_HANDLED;
	}

	if (dec_status != VP9_HEAD_PARSER_DONE) {
		pbi->process_busy = 0;
		return IRQ_HANDLED;
	}

	if (pbi->m_ins_flag &&
			!get_free_buf_count(pbi) && pbi->pic_list_init_done) {
		vp9_print(pbi, VP9_DEBUG_OUT_PTS,
			"detected %d frames in a packet, ts: %lld.\n",
			pbi->one_package_frame_cnt + 1,
			pbi->chunk->timestamp);
		pbi->process_busy = 0;
		pbi->dec_result = DEC_RESULT_NEED_MORE_BUFFER;
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_THREAD_HEAD_END);
		vdec_schedule_work(&pbi->work);
		return IRQ_HANDLED;
	}

	pbi->one_package_frame_cnt++;

#ifdef MULTI_INSTANCE_SUPPORT
#ifdef CONFIG_AMLOGIC_MEDIA_MULTI_DEC
	if (pbi->m_ins_flag ==0 && pbi->low_latency_flag) {
		vdec_profile(hw_to_vdec(pbi), VDEC_PROFILE_EVENT_RUN, 0);
		if (debug & PRINT_FLAG_VDEC_DETAIL)
			pr_info("%s VP9 frame header found \n", __func__);
	}
#endif
	if (pbi->m_ins_flag)
		reset_process_time(pbi);
#endif
	if (pbi->process_state != PROC_STATE_SENDAGAIN
#ifdef SUPPORT_FB_DECODING
		&& pbi->used_stage_buf_num == 0
#endif
		) {
		if (pbi->mmu_enable) {
			ATRACE_COUNTER(pbi->trace.decode_header_memory_time_name, TRACE_HEADER_MEMORY_START);
			vp9_recycle_mmu_buf_tail(pbi);
			ATRACE_COUNTER(pbi->trace.decode_header_memory_time_name, TRACE_HEADER_MEMORY_END);
		}

		if (atomic_read(&pbi->vf_pre_count) > 0)
			vp9_bufmgr_postproc(pbi);
	}

	if (debug & VP9_DEBUG_SEND_PARAM_WITH_REG) {
		get_rpm_param(&pbi->vp9_param);
	} else {
#ifdef SUPPORT_FB_DECODING
		if (pbi->used_stage_buf_num > 0) {
			reset_process_time(pbi);
			get_s1_buf(pbi);

			if (get_mv_buf(pbi,
				&pbi->s1_mv_buf_index,
				&pbi->s1_mpred_mv_wr_start_addr
				) < 0) {
				vp9_print(pbi, 0,
					"%s: Error get_mv_buf fail\n",
					__func__);
			}

			if (pbi->s1_buf == NULL) {
				vp9_print(pbi, 0,
					"%s: Error get_s1_buf fail\n",
					__func__);
				pbi->process_busy = 0;
				return IRQ_HANDLED;
			}

			for (i = 0; i < (RPM_END - RPM_BEGIN); i += 4) {
				int ii;
				for (ii = 0; ii < 4; ii++) {
					pbi->s1_buf->rpm[i + 3 - ii] =
						pbi->rpm_ptr[i + 3 - ii];
					pbi->s1_param.l.data[i + ii] =
						pbi->rpm_ptr[i + 3 - ii];
				}
			}

			mpred_process(pbi);
#ifdef FB_DECODING_TEST_SCHEDULE
			pbi->dec_s1_result =
				DEC_S1_RESULT_TEST_TRIGGER_DONE;
			vdec_schedule_work(&pbi->s1_work);
#else
			WRITE_VREG(HEVC_ASSIST_FB_MMU_MAP_ADDR,
				pbi->stage_mmu_map_phy_addr +
				pbi->s1_buf->index * STAGE_MMU_MAP_SIZE);

			start_s1_decoding(pbi);
#endif
			start_process_time(pbi);
			pbi->process_busy = 0;
			return IRQ_HANDLED;
		} else
#endif
		{
			ATRACE_COUNTER(pbi->trace.decode_header_memory_time_name, TRACE_HEADER_RPM_START);
			for (i = 0; i < (RPM_END - RPM_BEGIN); i += 4) {
				int ii;
				for (ii = 0; ii < 4; ii++)
					pbi->vp9_param.l.data[i + ii] =
						pbi->rpm_ptr[i + 3 - ii];
			}
			ATRACE_COUNTER(pbi->trace.decode_header_memory_time_name, TRACE_HEADER_RPM_END);
		}
	}

	// get pixelaspect
	ctx->height_aspect_ratio = 1;
	ctx->width_aspect_ratio = 1;

	pbi->frame_width = pbi->vp9_param.p.width;
	pbi->frame_height = pbi->vp9_param.p.height;

	if (is_oversize(pbi->frame_width, pbi->frame_height)) {
		continue_decoding(pbi);
		vp9_print(pbi, 0, "pic size(%d x %d) is oversize\n",
			pbi->frame_width, pbi->frame_height);
		pbi->postproc_done = 0;
		pbi->process_busy = 0;
		return IRQ_HANDLED;
	}

	if (!v4l_res_change(pbi)) {
		if (ctx->param_sets_from_ucode && !pbi->v4l_params_parsed) {
			struct aml_vdec_ps_infos ps;
			struct vdec_comp_buf_info comp;
			struct vdec_s * vdec = hw_to_vdec(pbi);
			pr_debug("set ucode parse\n");
			if (get_valid_double_write_mode(pbi) != 16) {
				vvp9_get_comp_buf_info(pbi, &comp);
				vdec_v4l_set_comp_buf_info(ctx, &comp);
			}

			vvp9_get_ps_info(pbi, &ps);
			/*notice the v4l2 codec.*/
			vdec_v4l_set_ps_infos(ctx, &ps);

			pbi->init_pic_w = pbi->frame_width;
			pbi->init_pic_h = pbi->frame_height;

			pbi->last_width = pbi->frame_width;
			pbi->last_height = pbi->frame_height;

			ctx->decoder_status_info.frame_height = ps.visible_height;
			ctx->decoder_status_info.frame_width = ps.visible_width;
			v4l_vp9_collect_stream_info(vdec, pbi);
			ctx->dec_intf.decinfo_event_report(ctx, AML_DECINFO_EVENT_STATISTIC, NULL);
			pbi->v4l_params_parsed	= true;
			pbi->postproc_done = 0;
			pbi->process_busy = 0;
			ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_THREAD_HEAD_END);
			dec_again_process(pbi);
			return IRQ_HANDLED;
		} else {
			if (!pbi->pic_list_init_done) {
				struct vdec_pic_info pic;

				vdec_v4l_get_pic_info(ctx, &pic);
				pbi->used_buf_num = pic.dpb_frames +
					pic.dpb_margin;

				if (pbi->used_buf_num > MAX_BUF_NUM)
					pbi->used_buf_num = MAX_BUF_NUM;

				init_pic_list(pbi);
				init_pic_list_hw(pbi);

				init_mv_buf_list(pbi);
				pbi->pic_list_init_done = true;
			}
		}
	} else {
		pbi->postproc_done = 0;
		pbi->process_busy = 0;
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_THREAD_HEAD_END);
		dec_again_process(pbi);
		return IRQ_HANDLED;
	}

	continue_decoding(pbi);
	pbi->postproc_done = 0;
	pbi->process_busy = 0;

	vdec_profile(hw_to_vdec(pbi), VDEC_PROFILE_DECODER_START, CORE_MASK_HEVC);
	ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_THREAD_HEAD_END);
	return IRQ_HANDLED;
}

static irqreturn_t vvp9_isr(int irq, void *data)
{
	unsigned int dec_status;
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)data;
	struct aml_vcodec_ctx *ctx = pbi->v4l2_ctx;
	unsigned int adapt_prob_status;

	WRITE_VREG(HEVC_ASSIST_MBOX0_CLR_REG, 1);
	dec_status = READ_VREG(HEVC_DEC_STATUS_REG);
	vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_2, dec_status);

	if (dec_status == VP9_HEAD_PARSER_DONE) {
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_HEAD_DONE);
	}
	else if (dec_status == HEVC_DECPIC_DATA_DONE) {
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_PIC_DONE);
		vdec_profile(hw_to_vdec(pbi), VDEC_PROFILE_DECODER_END, CORE_MASK_HEVC);
	}

	adapt_prob_status = READ_VREG(VP9_ADAPT_PROB_REG);
	if (!pbi)
		return IRQ_HANDLED;
	if (pbi->init_flag == 0)
		return IRQ_HANDLED;
	if (pbi->process_busy) { /*on process.*/
		vp9_print(pbi, PRINT_FLAG_VDEC_STATUS,
			"%s: process_busy isr return\n", __func__);
		return IRQ_HANDLED;
	}
	pbi->dec_status = dec_status;
	pbi->process_busy = 1;

#ifdef MULTI_INSTANCE_SUPPORT
	if (!pbi->m_ins_flag) {
#endif
		if (pbi->error_flag == 1) {
			pbi->error_flag = 2;
			pbi->process_busy = 0;
			return IRQ_HANDLED;
		} else if (pbi->error_flag == 3) {
			pbi->process_busy = 0;
			return IRQ_HANDLED;
		}

		if (get_free_buf_count(pbi) <= 0) {
			/*
			if (pbi->wait_buf == 0)
				pr_info("set wait_buf to 1\r\n");
			*/
			pbi->wait_buf = 1;
			pbi->process_busy = 0;
			return IRQ_HANDLED;
		}
#ifdef MULTI_INSTANCE_SUPPORT
	}
#endif
	if ((adapt_prob_status & 0xff) == 0xfd) {
		struct VP9_Common_s *const cm = &pbi->common;
		int pre_fc = 0;

		if (pbi->prob_buffer_phy_addr) {
			vp9_print(pbi, VP9_DEBUG_BUFMGR_DETAIL,
				"VP9: use adapt prob soft mode\n");
		} else {
			vp9_print(pbi, PRINT_FLAG_ERROR,
				"Error: the vp9 prob_buffer_phy_addr is NULL\n");
		}

		if (pbi->m_ins_flag)
				reset_process_time(pbi);

		if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) &&
			(vdec_secure(hw_to_vdec(pbi)))) {
			pre_fc = ((cm->frame_type == KEY_FRAME) || (cm->intra_only)) ? 0 : 1;
			tee_vp9_prob_process(pre_fc, cm->last_frame_type,
				adapt_prob_status, (unsigned int)pbi->prob_buffer_phy_addr);
		} else {
			uint8_t *prev_prob_b, *cur_prob_b, *count_b;

			/*VP9_REQ_ADAPT_PROB*/
			pre_fc = ((cm->frame_type == KEY_FRAME) || (cm->intra_only));
			prev_prob_b = ((uint8_t *)pbi->prob_buffer_addr) +
				((adapt_prob_status >> 8) * 0x1000);
			cur_prob_b = ((uint8_t *)pbi->prob_buffer_addr) + 0x4000;
			count_b = (uint8_t *)pbi->count_buffer_addr;

			adapt_coef_probs(pbi->pic_count,
				(cm->last_frame_type == KEY_FRAME),
				pre_fc, (adapt_prob_status >> 8),
				(unsigned int *)prev_prob_b,
				(unsigned int *)cur_prob_b, (unsigned int *)count_b);

			memcpy(prev_prob_b, cur_prob_b, PROB_SIZE);
		}

		WRITE_VREG(VP9_ADAPT_PROB_REG, 0);
		pbi->pic_count += 1;
#ifdef MULTI_INSTANCE_SUPPORT
		if (pbi->m_ins_flag)
			start_process_time(pbi);
#endif
	}
	ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_ISR_END);
	return IRQ_WAKE_THREAD;
}

static void vp9_set_clk(struct work_struct *work)
{
	struct VP9Decoder_s *pbi = container_of(work,
		struct VP9Decoder_s, set_clk_work);
	int fps = 96000 / pbi->frame_dur;

	if (hevc_source_changed(VFORMAT_VP9,
		frame_width, frame_height, fps) > 0)
		pbi->saved_resolution = frame_width *
		frame_height * fps;
}

static void vvp9_put_timer_func(struct timer_list *timer)
{
	struct VP9Decoder_s *pbi = container_of(timer,
		struct VP9Decoder_s, timer);
	enum receiver_start_e state = RECEIVER_INACTIVE;
	uint8_t empty_flag;
	unsigned int buf_level;

	if (pbi->init_flag == 0) {
		if (pbi->stat & STAT_TIMER_ARM) {
			timer->expires = jiffies + PUT_INTERVAL;
			add_timer(&pbi->timer);
		}
		return;
	}
	if (pbi->m_ins_flag == 0) {
		if (vf_get_receiver(pbi->provider_name)) {
			state =
				vf_notify_receiver(pbi->provider_name,
					VFRAME_EVENT_PROVIDER_QUREY_STATE,
					NULL);
			if ((state == RECEIVER_STATE_NULL)
				|| (state == RECEIVER_STATE_NONE))
				state = RECEIVER_INACTIVE;
		} else
			state = RECEIVER_INACTIVE;

		empty_flag = (READ_VREG(HEVC_PARSER_INT_STATUS) >> 6) & 0x1;
		/* error watchdog */
		if (empty_flag == 0) {
			/* decoder has input */
			if ((debug & VP9_DEBUG_DIS_LOC_ERROR_PROC) == 0) {

				buf_level = READ_VREG(HEVC_STREAM_LEVEL);
				/* receiver has no buffer to recycle */
				if ((state == RECEIVER_INACTIVE) &&
					(kfifo_is_empty(&pbi->display_q) &&
					 buf_level > 0x200)
					) {
						WRITE_VREG
						(HEVC_ASSIST_MBOX0_IRQ_REG,
						 0x1);
				}
			}
		}
	}
#ifdef MULTI_INSTANCE_SUPPORT
	else {
		if ((decode_timeout_val > 0) &&
			(pbi->start_process_time > 0) &&
			((1000 * (jiffies - pbi->start_process_time) / HZ)
				> decode_timeout_val)) {
			int current_lcu_idx =
				READ_VREG(HEVC_PARSER_LCU_START)
				& 0xffffff;
			if (pbi->last_lcu_idx == current_lcu_idx) {
				if (pbi->decode_timeout_count > 0)
					pbi->decode_timeout_count--;
				if (pbi->decode_timeout_count == 0) {
					if (input_frame_based(
						hw_to_vdec(pbi)) ||
					(READ_VREG(HEVC_STREAM_LEVEL) > 0x200))
						timeout_process(pbi);
					else {
						vp9_print(pbi, 0,
							"timeout & empty, again\n");
						dec_again_process(pbi);
					}
				}
			} else {
				start_process_time(pbi);
				pbi->last_lcu_idx = current_lcu_idx;
			}
		}
	}
#endif

	if ((pbi->ucode_pause_pos != 0) &&
		(pbi->ucode_pause_pos != 0xffffffff) &&
		udebug_pause_pos != pbi->ucode_pause_pos) {
		pbi->ucode_pause_pos = 0;
		WRITE_HREG(DEBUG_REG1, 0);
	}
#ifdef MULTI_INSTANCE_SUPPORT
	if (debug & VP9_DEBUG_FORCE_SEND_AGAIN) {
		pr_info(
		"Force Send Again\r\n");
		debug &= ~VP9_DEBUG_FORCE_SEND_AGAIN;
		reset_process_time(pbi);
		pbi->dec_result = DEC_RESULT_AGAIN;
		if (pbi->process_state == PROC_STATE_DECODESLICE) {
			pbi->process_state = PROC_STATE_SENDAGAIN;
		}
		amhevc_stop();

		vdec_schedule_work(&pbi->work);
	}

	if (debug & VP9_DEBUG_DUMP_DATA) {
		debug &= ~VP9_DEBUG_DUMP_DATA;
		vp9_print(pbi, 0,
			"%s: chunk size 0x%x off 0x%x sum 0x%x\n",
			__func__,
			pbi->data_size,
			pbi->data_offset,
			get_data_check_sum(pbi, pbi->data_size)
			);
		dump_data(pbi, pbi->data_size);
	}
#endif
	if (debug & VP9_DEBUG_DUMP_PIC_LIST) {
		dump_pic_list(pbi);
		debug &= ~VP9_DEBUG_DUMP_PIC_LIST;
	}
	if (debug & VP9_DEBUG_TRIG_SLICE_SEGMENT_PROC) {
		WRITE_VREG(HEVC_ASSIST_MBOX0_IRQ_REG, 0x1);
		debug &= ~VP9_DEBUG_TRIG_SLICE_SEGMENT_PROC;
	}

	if (radr != 0) {
		if (rval != 0) {
			WRITE_VREG(radr, rval);
			pr_info("WRITE_VREG(%x,%x)\n", radr, rval);
		} else
			pr_info("READ_VREG(%x)=%x\n", radr, READ_VREG(radr));
		rval = 0;
		radr = 0;
	}
	if (pop_shorts != 0) {
		int i;
		u32 sum = 0;

		pr_info("pop stream 0x%x shorts\r\n", pop_shorts);
		for (i = 0; i < pop_shorts; i++) {
			u32 data =
			(READ_HREG(HEVC_SHIFTED_DATA) >> 16);
			WRITE_HREG(HEVC_SHIFT_COMMAND,
			(1<<7)|16);
			if ((i & 0xf) == 0)
				pr_info("%04x:", i);
			pr_info("%04x ", data);
			if (((i + 1) & 0xf) == 0)
				pr_info("\r\n");
			sum += data;
		}
		pr_info("\r\nsum = %x\r\n", sum);
		pop_shorts = 0;
	}
	if (dbg_cmd != 0) {
		if (dbg_cmd == 1) {
			u32 disp_laddr;

			if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_GXBB &&
				get_double_write_mode(pbi) == 0) {
				disp_laddr =
					READ_VCBUS_REG(AFBC_BODY_BADDR) << 4;
			} else {
				struct canvas_s cur_canvas;

				canvas_read((READ_VCBUS_REG(VD1_IF0_CANVAS0)
					& 0xff), &cur_canvas);
				disp_laddr = cur_canvas.addr;
			}
			pr_info("current displayed buffer address %x\r\n",
				disp_laddr);
		}
		dbg_cmd = 0;
	}
	/*don't changed at start.*/
	if (pbi->get_frame_dur && pbi->show_frame_num > 60 &&
		pbi->frame_dur > 0 && pbi->saved_resolution !=
		frame_width * frame_height *
			(96000 / pbi->frame_dur))
		vdec_schedule_work(&pbi->set_clk_work);

	timer->expires = jiffies + PUT_INTERVAL;
	add_timer(timer);
}


int vvp9_dec_status(struct vdec_s *vdec, struct vdec_info *vstatus)
{
	struct VP9Decoder_s *vp9 =
		(struct VP9Decoder_s *)vdec->private;

	if (!vp9)
		return -1;

	vstatus->frame_width = frame_width;
	vstatus->frame_height = frame_height;
	if (vp9->error_frame_width &&
		vp9->error_frame_height) {
		vstatus->frame_width = vp9->error_frame_width;
		vstatus->frame_height = vp9->error_frame_height;
	}

	if (vp9->frame_dur != 0)
		vstatus->frame_rate = ((96000 * 10 / vp9->frame_dur) % 10) < 5 ?
				96000 / vp9->frame_dur : (96000 / vp9->frame_dur +1);
	else
		vstatus->frame_rate = -1;
	vstatus->error_count = 0;
	vstatus->status = vp9->stat | vp9->fatal_error;
	vstatus->frame_dur = vp9->frame_dur;
	vstatus->bit_rate = vp9->gvs->bit_rate;
	vstatus->frame_data = vp9->gvs->frame_data;
	vstatus->total_data = vp9->gvs->total_data;
	vstatus->frame_count = vp9->gvs->frame_count;
	vstatus->error_frame_count = vp9->gvs->error_frame_count;
	vstatus->drop_frame_count = vp9->gvs->drop_frame_count;
	vstatus->i_decoded_frames = vp9->gvs->i_decoded_frames;
	vstatus->i_lost_frames = vp9->gvs->i_lost_frames;
	vstatus->i_concealed_frames = vp9->gvs->i_concealed_frames;
	vstatus->p_decoded_frames = vp9->gvs->p_decoded_frames;
	vstatus->p_lost_frames = vp9->gvs->p_lost_frames;
	vstatus->p_concealed_frames = vp9->gvs->p_concealed_frames;
	vstatus->b_decoded_frames = vp9->gvs->b_decoded_frames;
	vstatus->b_lost_frames = vp9->gvs->b_lost_frames;
	vstatus->b_concealed_frames = vp9->gvs->b_concealed_frames;
	vstatus->total_data = vp9->gvs->total_data;
	vstatus->samp_cnt = vp9->gvs->samp_cnt;
	vstatus->offset = vp9->gvs->offset;
	snprintf(vstatus->vdec_name, sizeof(vstatus->vdec_name),
		"%s", DRIVER_NAME);
	return 0;
}

int vvp9_set_isreset(struct vdec_s *vdec, int isreset)
{
	is_reset = isreset;
	return 0;
}

static void vvp9_prot_init(struct VP9Decoder_s *pbi, u32 mask)
{
	unsigned int data32;
	/* VP9_DECODE_INIT(); */
	vp9_config_work_space_hw(pbi, mask);
	if (mask & HW_MASK_BACK)
		init_pic_list_hw(pbi);

	vp9_init_decoder_hw(pbi, mask);

#ifdef VP9_LPF_LVL_UPDATE
	if (mask & HW_MASK_BACK)
		vp9_loop_filter_init(pbi);
#endif

	if ((mask & HW_MASK_FRONT) == 0)
		return;

	if (debug & VP9_DEBUG_BUFMGR_MORE)
		pr_info("%s\n", __func__);
	data32 = READ_VREG(HEVC_STREAM_CONTROL);
	data32 = data32 |
		(1 << 0)/*stream_fetch_enable*/
		;
	WRITE_VREG(HEVC_STREAM_CONTROL, data32);

	if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A) {
	    if (debug & VP9_DEBUG_BUFMGR)
	        pr_info("[test.c] Config STREAM_FIFO_CTL\n");
	    data32 = READ_VREG(HEVC_STREAM_FIFO_CTL);
	    data32 = data32 |
	             (1 << 29) // stream_fifo_hole
	             ;
	    WRITE_VREG(HEVC_STREAM_FIFO_CTL, data32);
	}

	WRITE_VREG(HEVC_SHIFT_STARTCODE, 0x000000001);
	WRITE_VREG(HEVC_SHIFT_EMULATECODE, 0x00000300);
	WRITE_VREG(HEVC_WAIT_FLAG, 1);

	/* WRITE_VREG(HEVC_MPSR, 1); */

	/* clear mailbox interrupt */
	WRITE_VREG(HEVC_ASSIST_MBOX0_CLR_REG, 1);

	/* enable mailbox interrupt */
	WRITE_VREG(HEVC_ASSIST_MBOX0_MASK, 1);

	/* disable PSCALE for hardware sharing */
	WRITE_VREG(HEVC_PSCALE_CTRL, 0);

	WRITE_VREG(DEBUG_REG1, 0x0);
	/*check vps/sps/pps/i-slice in ucode*/
	WRITE_VREG(NAL_SEARCH_CTL, 0x8);

	WRITE_VREG(DECODE_STOP_POS, udebug_flag);
#ifdef SUPPORT_FB_DECODING
#ifndef FB_DECODING_TEST_SCHEDULE
	if (pbi->used_stage_buf_num > 0) {
		if (mask & HW_MASK_FRONT) {
			data32 = READ_VREG(
				HEVC_ASSIST_HED_FB_W_CTL);
			data32 = data32 |
				(1 << 0) /*hed_fb_wr_en*/
				;
			WRITE_VREG(HEVC_ASSIST_HED_FB_W_CTL,
				data32);
		}
		if (mask & HW_MASK_BACK) {
			data32 = READ_VREG(
				HEVC_ASSIST_HED_FB_R_CTL);
			while (data32 & (1 << 7)) {
				/*wait finish*/
				data32 = READ_VREG(
					HEVC_ASSIST_HED_FB_R_CTL);
			}
			data32 &= (~(0x1 << 0));
			/*hed_fb_rd_addr_auto_rd*/
			data32 &= (~(0x1 << 1));
			/*rd_id = 0, hed_rd_map_auto_halt_num,
			after wr 2 ready, then start reading*/
			data32 |= (0x2 << 16);
			WRITE_VREG(HEVC_ASSIST_HED_FB_R_CTL,
				data32);

			data32 |= (0x1 << 11); /*hed_rd_map_auto_halt_en*/
			data32 |= (0x1 << 1); /*hed_fb_rd_addr_auto_rd*/
			data32 |= (0x1 << 0); /*hed_fb_rd_en*/
			WRITE_VREG(HEVC_ASSIST_HED_FB_R_CTL,
				data32);
		}

	}
#endif
#endif
}

static int vvp9_local_init(struct VP9Decoder_s *pbi)
{
	int i;
	int ret;
	int width, height;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	if (alloc_lf_buf(pbi) < 0)
		return -1;

	pbi->gvs = vzalloc(sizeof(struct vdec_info));
	if (NULL == pbi->gvs) {
		pr_info("the struct of vdec status malloc failed.\n");
		return -1;
	}
	vdec_set_vframe_comm(hw_to_vdec(pbi), DRIVER_NAME);
#ifdef DEBUG_PTS
	pbi->pts_missed = 0;
	pbi->pts_hit = 0;
#endif
	pbi->new_frame_displayed = 0;
	pbi->last_put_idx = -1;
	pbi->saved_resolution = 0;
	pbi->get_frame_dur = false;
	on_no_keyframe_skiped = 0;
	pbi->duration_from_pts_done = 0;
	pbi->vp9_first_pts_ready = 0;
	pbi->frame_cnt_window = 0;
	width = pbi->vvp9_amstream_dec_info.width;
	height = pbi->vvp9_amstream_dec_info.height;
	pbi->frame_dur = pbi->v4l_duration ? pbi->v4l_duration :
		((pbi->vvp9_amstream_dec_info.rate ==
		 0) ? 3200 : pbi->vvp9_amstream_dec_info.rate);
	if (width && height)
		pbi->frame_ar = height * 0x100 / width;
/*
 *TODO:FOR VERSION
 */
	pr_info("vp9: ver (%d,%d) decinfo: %dx%d rate=%d\n", vp9_version,
		   0, width, height, pbi->frame_dur);

	if (pbi->frame_dur == 0)
		pbi->frame_dur = 96000 / 24;

	INIT_KFIFO(pbi->display_q);
	INIT_KFIFO(pbi->newframe_q);

	for (i = 0; i < VF_POOL_SIZE; i++) {
		const struct vframe_s *vf = &pbi->vfpool[i];

		pbi->vfpool[i].index = -1;
		kfifo_put(&pbi->newframe_q, vf);
	}


	ret = vp9_local_init(pbi);
	if (ret < 0) {
		vdec_v4l_post_error_event(ctx, DECODER_ERROR_ALLOC_BUFFER_FAIL);
	}

	if (!pbi->pts_unstable) {
		pbi->pts_unstable =
		(pbi->vvp9_amstream_dec_info.rate == 0)?1:0;
		pr_debug("set pts unstable\n");
	}

	return ret;
}


#ifdef MULTI_INSTANCE_SUPPORT
static s32 vvp9_init(struct vdec_s *vdec)
{
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)vdec->private;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
#else
static s32 vvp9_init(struct VP9Decoder_s *pbi)
{
#endif
	int ret;
	int fw_size = 0x1000 * 16;
	struct firmware_s *fw = NULL;

	pbi->stat |= STAT_TIMER_INIT;

	if (vvp9_local_init(pbi) < 0)
		return -EBUSY;

	fw = fw_firmare_s_creat(fw_size);
	if (IS_ERR_OR_NULL(fw))
		return -ENOMEM;

	if (get_firmware_data(VIDEO_DEC_VP9_MMU, fw->data) < 0) {
		pr_err("get firmware fail.\n");
		vfree(fw);
		return -1;
	}

	fw->len = fw_size;

	INIT_WORK(&pbi->set_clk_work, vp9_set_clk);
	timer_setup(&pbi->timer, vvp9_put_timer_func, 0);
	spin_lock_init(&pbi->wait_buf_lock);

#ifdef MULTI_INSTANCE_SUPPORT
	if (pbi->m_ins_flag) {
		pbi->timer.expires = jiffies + PUT_INTERVAL;

		INIT_WORK(&pbi->work, vp9_work);
		INIT_WORK(&pbi->recycle_mmu_work, vp9_recycle_mmu_work);
#ifdef SUPPORT_FB_DECODING
		if (pbi->used_stage_buf_num > 0)
			INIT_WORK(&pbi->s1_work, vp9_s1_work);
#endif
		pbi->fw = fw;

		return 0;
	}
#endif
	amhevc_enable();

	init_pic_list(pbi);

	ret = amhevc_loadmc_ex(VFORMAT_VP9, NULL, fw->data);
	if (ret < 0) {
		amhevc_disable();
		vfree(fw);
		pr_err("VP9: the %s fw loading failed, err: %x\n",
			fw_tee_enabled() ? "TEE" : "local", ret);
		vdec_v4l_post_error_event(ctx, DECODER_EMERGENCY_FW_LOAD_ERROR);
		return -EBUSY;
	}

	vfree(fw);

	pbi->stat |= STAT_MC_LOAD;

	/* enable AMRISC side protocol */
	vvp9_prot_init(pbi, HW_MASK_FRONT | HW_MASK_BACK);

	if (vdec_request_threaded_irq(VDEC_IRQ_0,
				vvp9_isr,
				vvp9_isr_thread_fn,
				IRQF_ONESHOT,/*run thread on this irq disabled*/
				"vvp9-irq", (void *)pbi)) {
		pr_info("vvp9 irq register error.\n");
		amhevc_disable();
		return -ENOENT;
	}

	pbi->stat |= STAT_ISR_REG;

	pbi->provider_name = PROVIDER_NAME;
#ifndef MULTI_INSTANCE_SUPPORT
	vf_provider_init(&vvp9_vf_prov, PROVIDER_NAME, &vvp9_vf_provider,
					 pbi);
	vf_reg_provider(&vvp9_vf_prov);
	vf_notify_receiver(PROVIDER_NAME, VFRAME_EVENT_PROVIDER_START, NULL);
	if (!is_reset)
		vf_notify_receiver(PROVIDER_NAME, VFRAME_EVENT_PROVIDER_FR_HINT,
		(void *)((unsigned long)pbi->frame_dur));
#endif
	pbi->stat |= STAT_VF_HOOK;

	pbi->timer.expires = jiffies + PUT_INTERVAL;
	add_timer(&pbi->timer);

	pbi->stat |= STAT_VDEC_RUN;

	pbi->stat |= STAT_TIMER_ARM;

	amhevc_start();

	pbi->init_flag = 1;
	pbi->process_busy = 0;
	pr_info("%d, vvp9_init, RP=0x%x\n",
		__LINE__, READ_VREG(HEVC_STREAM_RD_PTR));
	return 0;
}

static int vmvp9_stop(struct VP9Decoder_s *pbi)
{
	pbi->init_flag = 0;

	if (pbi->stat & STAT_VDEC_RUN) {
		amhevc_stop();
		pbi->stat &= ~STAT_VDEC_RUN;
	}
	if (pbi->stat & STAT_ISR_REG) {
		vdec_free_irq(VDEC_IRQ_0, (void *)pbi);
		pbi->stat &= ~STAT_ISR_REG;
	}
	if (pbi->stat & STAT_TIMER_ARM) {
		del_timer_sync(&pbi->timer);
		pbi->stat &= ~STAT_TIMER_ARM;
	}

	vp9_local_uninit(pbi);
	reset_process_time(pbi);
	cancel_work_sync(&pbi->work);
	cancel_work_sync(&pbi->recycle_mmu_work);
#ifdef SUPPORT_FB_DECODING
	if (pbi->used_stage_buf_num > 0)
		cancel_work_sync(&pbi->s1_work);
#endif
	cancel_work_sync(&pbi->set_clk_work);
	uninit_mmu_buffers(pbi);
	if (pbi->fw)
		vfree(pbi->fw);
	pbi->fw = NULL;
	return 0;
}

static int amvdec_vp9_mmu_init(struct VP9Decoder_s *pbi)
{
	int tvp_flag = vdec_secure(hw_to_vdec(pbi)) ?
		CODEC_MM_FLAGS_TVP : 0;
	int buf_size = vp9_max_mmu_buf_size(pbi->max_pic_w, pbi->max_pic_h);
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);

	pbi->need_cache_size = buf_size * SZ_1M;
	pbi->sc_start_time = get_jiffies_64();

	pbi->bmmu_box = decoder_bmmu_box_alloc_box(
			DRIVER_NAME,
			pbi->index,
			MAX_BMMU_BUFFER_NUM,
			4 + PAGE_SHIFT,
			CODEC_MM_FLAGS_CMA_CLEAR |
			CODEC_MM_FLAGS_FOR_VDECODER |
			tvp_flag,
			BMMU_ALLOC_FLAGS_WAIT);
	if (!pbi->bmmu_box) {
		pr_err("vp9 alloc bmmu box failed!!\n");
		vdec_v4l_post_error_event(ctx, DECODER_ERROR_ALLOC_BUFFER_FAIL);
		return -1;
	}
	return 0;
}

static void vdec_fence_release(struct VP9Decoder_s *pbi,
			       struct vdec_sync *sync)
{
	ulong expires;

	/* notify signal to wake up all fences. */
	vdec_timeline_increase(sync, VF_POOL_SIZE);

	expires = jiffies + msecs_to_jiffies(2000);
	while (!check_objs_all_signaled(sync)) {
		if (time_after(jiffies, expires)) {
			pr_err("wait fence signaled timeout.\n");
			break;
		}
	}

	/* decreases refcnt of timeline. */
	vdec_timeline_put(sync);
}

/****************************************/

static unsigned char get_data_check_sum
	(struct VP9Decoder_s *pbi, int size)
{
	int jj;
	int sum = 0;
	u8 *data = NULL;

	if (vdec_secure(hw_to_vdec(pbi)))
		return 0;

	if (!pbi->chunk->block->is_mapped)
		data = codec_mm_vmap(pbi->chunk->block->start +
			pbi->data_offset, size);
	else
		data = ((u8 *)pbi->chunk->block->start_virt) +
			pbi->data_offset;

	for (jj = 0; jj < size; jj++)
		sum += data[jj];

	vp9_print(pbi, PRINT_FLAG_VDEC_STATUS,
		"%s: size 0x%x sum 0x%x %02x %02x %02x %02x %02x %02x .. %02x %02x %02x %02x\n",
		__func__, size, sum,
		(size < 1) ? 0 : data[0],
		(size < 2) ? 0 : data[1],
		(size < 3) ? 0 : data[2],
		(size < 4) ? 0 : data[3],
		(size < 5) ? 0 : data[4],
		(size < 6) ? 0 : data[5],
		(size < 4) ? 0 : data[size - 4],
		(size < 3) ? 0 : data[size - 3],
		(size < 2) ? 0 : data[size - 2],
		(size < 1) ? 0 : data[size - 1]);

	if (!pbi->chunk->block->is_mapped)
		codec_mm_unmap_phyaddr(data);
	return sum;
}

static void dump_data(struct VP9Decoder_s *pbi, int size)
{
	int jj;
	u8 *data = NULL;
	int padding_size = pbi->data_offset &
		(VDEC_FIFO_ALIGN - 1);

	if (!pbi->chunk->block->is_mapped)
		data = codec_mm_vmap(pbi->chunk->block->start +
			pbi->data_offset, size);
	else
		data = ((u8 *)pbi->chunk->block->start_virt) +
			pbi->data_offset;

	vp9_print(pbi, PRINT_FLAG_VDEC_DATA, "padding: ");
	for (jj = padding_size; jj > 0; jj--)
		vp9_print_cont(pbi,
			0,
			"%02x ", *(data - jj));
	vp9_print_cont(pbi, 0, "data adr %p\n",
		data);

	for (jj = 0; jj < size; jj++) {
		if ((jj & 0xf) == 0)
			vp9_print(pbi, PRINT_FLAG_VDEC_DATA,
				"%06x:", jj);
		vp9_print_cont(pbi,
			0,
			"%02x ", data[jj]);
		if (((jj + 1) & 0xf) == 0)
			vp9_print_cont(pbi, 0, "\n");
	}
	vp9_print_cont(pbi, 0, "\n");

	if (!pbi->chunk->block->is_mapped)
		codec_mm_unmap_phyaddr(data);
}

static int is_there_free_fb(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	struct aml_vcodec_ctx * ctx =
		(struct aml_vcodec_ctx *)pbi->v4l2_ctx;
	int i, free_slot = 0;

	for (i = 0; i < pbi->used_buf_num; ++i) {
		if ((frame_bufs[i].ref_count == 0) &&
			(frame_bufs[i].buf.vf_ref == 0) &&
			!frame_bufs[i].buf.cma_alloc_addr &&
			(cm->cur_frame != &frame_bufs[i])) {
			free_slot++;
			if (free_slot >= pbi->run_ready_min_buf_num)
				break;
		}
	}

	if (free_slot < pbi->run_ready_min_buf_num ||
		aml_buf_ready_num(&ctx->bm) < pbi->run_ready_min_buf_num )
		return false;

	return true;
}


static int vp9_wait_cap_buf(void *args)
{
	struct VP9Decoder_s *pbi =
		(struct VP9Decoder_s *) args;
	struct aml_vcodec_ctx * ctx =
		(struct aml_vcodec_ctx *)pbi->v4l2_ctx;
	ulong flags;
	int ret = 0;

	ret = wait_event_interruptible_timeout(ctx->cap_wq,
		(ctx->is_stream_off || (is_there_free_fb(pbi) > 0)),
		msecs_to_jiffies(300));
	if (ret <= 0){
		pr_err("%s, wait cap buf timeout or err %d\n",
			__func__, ret);
	}

	spin_lock_irqsave(&pbi->wait_buf_lock, flags);
	if (pbi->wait_more_buf) {
		pbi->wait_more_buf = false;
		pbi->dec_result = ctx->is_stream_off ?
		DEC_RESULT_FORCE_EXIT :
		DEC_RESULT_NEED_MORE_BUFFER;
		vdec_schedule_work(&pbi->work);
	}
	spin_unlock_irqrestore(&pbi->wait_buf_lock, flags);

	vp9_print(pbi, PRINT_FLAG_V4L_DETAIL,
		"%s wait capture buffer end, ret:%d\n",
		__func__, ret);
	return 0;
}

static void vp9_work(struct work_struct *work)
{
	struct VP9Decoder_s *pbi = container_of(work,
		struct VP9Decoder_s, work);
	struct aml_vcodec_ctx *ctx = pbi->v4l2_ctx;
	struct vdec_s *vdec = hw_to_vdec(pbi);
	/* finished decoding one frame or error,
	 * notify vdec core to switch context
	 */
	if (pbi->dec_result == DEC_RESULT_AGAIN)
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_WORKER_AGAIN);

	if (pbi->dec_result != DEC_RESULT_NEED_MORE_BUFFER)
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_WORKER_START);
	vp9_print(pbi, PRINT_FLAG_VDEC_DETAIL,
		"%s dec_result %d %x %x %x\n",
		__func__,
		pbi->dec_result,
		READ_VREG(HEVC_STREAM_LEVEL),
		READ_VREG(HEVC_STREAM_WR_PTR),
		READ_VREG(HEVC_STREAM_RD_PTR));

	vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_3, pbi->dec_result);

	if (pbi->dec_result == DEC_INIT_PICLIST) {
		init_pic_list(pbi);
		pbi->pic_list_init_done = true;
		return;
	}

	if (pbi->dec_result == DEC_RESULT_NEED_MORE_BUFFER) {
		vp9_recycle_frame_buffer(pbi);
		reset_process_time(pbi);
		if (!get_free_buf_count(pbi)) {
			ulong flags;

			spin_lock_irqsave(&pbi->wait_buf_lock, flags);
			if (vdec->next_status == VDEC_STATUS_DISCONNECTED) {
				pbi->dec_result = DEC_RESULT_AGAIN;
				pbi->postproc_done = 0;
				pbi->process_busy = 0;
				vdec_schedule_work(&pbi->work);
			} else {
				pbi->wait_more_buf = true;
			}
			spin_unlock_irqrestore(&pbi->wait_buf_lock, flags);

			if (pbi->wait_more_buf) {
				vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_5, __LINE__);
				vdec_post_task(vp9_wait_cap_buf, pbi);
			}
		} else {
			int i;

			vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_5, 0);

			if (pbi->mmu_enable)
				vp9_recycle_mmu_buf_tail(pbi);

			if (pbi->frame_count > 0)
				vp9_bufmgr_postproc(pbi);

			for (i = 0; i < (RPM_END - RPM_BEGIN); i += 4) {
				int ii;
				for (ii = 0; ii < 4; ii++)
					pbi->vp9_param.l.data[i + ii] =
						pbi->rpm_ptr[i + 3 - ii];
			}
			continue_decoding(pbi);
			pbi->postproc_done = 0;
			pbi->process_busy = 0;
		}

		return;
	}

	if (((pbi->dec_result == DEC_RESULT_GET_DATA) ||
		(pbi->dec_result == DEC_RESULT_GET_DATA_RETRY))
		&& (hw_to_vdec(pbi)->next_status !=
		VDEC_STATUS_DISCONNECTED)) {
		if (!vdec_has_more_input(vdec)) {
			pbi->dec_result = DEC_RESULT_EOS;
			vdec_schedule_work(&pbi->work);
			return;
		}

		if (pbi->dec_result == DEC_RESULT_GET_DATA) {
			vp9_print(pbi, PRINT_FLAG_VDEC_STATUS,
				"%s DEC_RESULT_GET_DATA %x %x %x\n",
				__func__,
				READ_VREG(HEVC_STREAM_LEVEL),
				READ_VREG(HEVC_STREAM_WR_PTR),
				READ_VREG(HEVC_STREAM_RD_PTR));
			vdec_vframe_dirty(vdec, pbi->chunk);
			vdec_clean_input(vdec);
		}

		if (get_free_buf_count(pbi) >=
			pbi->run_ready_min_buf_num) {
			int r;
			int decode_size;
			r = vdec_prepare_input(vdec, &pbi->chunk);
			if (r < 0) {
				pbi->dec_result = DEC_RESULT_GET_DATA_RETRY;

				vp9_print(pbi,
					PRINT_FLAG_VDEC_DETAIL,
					"vp9: Insufficient data\n");

				vdec_schedule_work(&pbi->work);
				return;
			}
			pbi->dec_result = DEC_RESULT_NONE;
			vp9_print(pbi, PRINT_FLAG_VDEC_STATUS,
				"%s: chunk size 0x%x sum 0x%x\n",
				__func__, r,
				(debug & PRINT_FLAG_VDEC_STATUS) ?
				get_data_check_sum(pbi, r) : 0
				);

			if (debug & PRINT_FLAG_VDEC_DATA)
				dump_data(pbi, pbi->data_size);

			decode_size = pbi->data_size +
				(pbi->data_offset & (VDEC_FIFO_ALIGN - 1));

			WRITE_VREG(HEVC_DECODE_SIZE,
				READ_VREG(HEVC_DECODE_SIZE) + decode_size);

			vdec_enable_input(vdec);

			WRITE_VREG(HEVC_DEC_STATUS_REG, HEVC_ACTION_DONE);

			start_process_time(pbi);

		} else{
			pbi->dec_result = DEC_RESULT_GET_DATA_RETRY;

			vp9_print(pbi, PRINT_FLAG_VDEC_DETAIL,
				"vp9: Insufficient data\n");

			vdec_schedule_work(&pbi->work);
		}
		return;
	} else if (pbi->dec_result == DEC_RESULT_DONE) {
		struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
#ifdef SUPPORT_FB_DECODING
		if (pbi->used_stage_buf_num > 0) {
#ifndef FB_DECODING_TEST_SCHEDULE
			if (!is_s2_decoding_finished(pbi)) {
				vp9_print(pbi, PRINT_FLAG_VDEC_DETAIL,
					"s2 decoding not done, check again later\n");
				vdec_schedule_work(&pbi->work);
			}
#endif
			inc_s2_pos(pbi);
			if (mcrcc_cache_alg_flag)
				dump_hit_rate(pbi);
		}
#endif

		if (pbi->timeout && vdec_frame_based(vdec)) {
			vdec_v4l_post_error_frame_event(ctx);
			pbi->timeout = false;
		}
		pbi->slice_idx++;
		pbi->frame_count++;
		pbi->process_state = PROC_STATE_INIT;
		decode_frame_count[pbi->index] = pbi->frame_count;
		ctx->decoder_status_info.decoder_count++;

		if (pbi->mmu_enable)
			pbi->used_4k_num =
				(READ_VREG(HEVC_SAO_MMU_STATUS) >> 16);
		vp9_print(pbi, PRINT_FLAG_VDEC_STATUS,
			"%s (===> %d) dec_result %d %x %x %x shiftbytes 0x%x decbytes 0x%x\n",
			__func__,
			pbi->frame_count,
			pbi->dec_result,
			READ_VREG(HEVC_STREAM_LEVEL),
			READ_VREG(HEVC_STREAM_WR_PTR),
			READ_VREG(HEVC_STREAM_RD_PTR),
			READ_VREG(HEVC_SHIFT_BYTE_COUNT),
			READ_VREG(HEVC_SHIFT_BYTE_COUNT) -
			pbi->start_shift_bytes
			);
		vdec_vframe_dirty(hw_to_vdec(pbi), pbi->chunk);
		if (pbi->dec_status == HEVC_DECPIC_DATA_DONE)
			vdec_code_rate(vdec, READ_VREG(HEVC_SHIFT_BYTE_COUNT) - pbi->start_shift_bytes);
	} else if (pbi->dec_result == DEC_RESULT_AGAIN) {
		/*
			stream base: stream buf empty or timeout
			frame base: vdec_prepare_input fail
		*/
		if (!vdec_has_more_input(vdec)) {
			pbi->dec_result = DEC_RESULT_EOS;
			vdec_schedule_work(&pbi->work);
			return;
		}
	} else if (pbi->dec_result == DEC_RESULT_EOS) {
		vp9_print(pbi, PRINT_FLAG_VDEC_STATUS,
			"%s: end of stream\n",
			__func__);
		vp9_bufmgr_postproc(pbi);
		vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_4, __LINE__);
		notify_v4l_eos(hw_to_vdec(pbi));
		vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_4, 0);

		vdec_vframe_dirty(hw_to_vdec(pbi), pbi->chunk);
		vdec_code_rate(vdec, READ_VREG(HEVC_SHIFT_BYTE_COUNT) - pbi->start_shift_bytes);
	} else if (pbi->dec_result == DEC_RESULT_FORCE_EXIT) {
		vp9_print(pbi, PRINT_FLAG_VDEC_STATUS,
			"%s: force exit\n",
			__func__);
		if (pbi->stat & STAT_VDEC_RUN) {
			amhevc_stop();
			pbi->stat &= ~STAT_VDEC_RUN;
		}

		if (pbi->stat & STAT_ISR_REG) {
#ifdef MULTI_INSTANCE_SUPPORT
			if (!pbi->m_ins_flag)
#endif
				WRITE_VREG(HEVC_ASSIST_MBOX0_MASK, 0);
			vdec_free_irq(VDEC_IRQ_0, (void *)pbi);
			pbi->stat &= ~STAT_ISR_REG;
		}
	} else if (pbi->dec_result == DEC_RESULT_UNFINISH) {
		pbi->slice_idx++;
		pbi->frame_count++;
		pbi->process_state = PROC_STATE_INIT;

		vdec_code_rate(vdec, READ_VREG(HEVC_SHIFT_BYTE_COUNT) - pbi->start_shift_bytes);
		if (pbi->mmu_enable)
			pbi->used_4k_num =
				(READ_VREG(HEVC_SAO_MMU_STATUS) >> 16);
		vp9_print(pbi, PRINT_FLAG_VDEC_STATUS,
			"%s (===> %d) dec_result %d %x %x %x shiftbytes 0x%x decbytes 0x%x\n",
			__func__,
			pbi->frame_count,
			pbi->dec_result,
			READ_VREG(HEVC_STREAM_LEVEL),
			READ_VREG(HEVC_STREAM_WR_PTR),
			READ_VREG(HEVC_STREAM_RD_PTR),
			READ_VREG(HEVC_SHIFT_BYTE_COUNT),
			READ_VREG(HEVC_SHIFT_BYTE_COUNT) -
			pbi->start_shift_bytes);
		amhevc_stop();
	}
	if (pbi->stat & STAT_VDEC_RUN) {
		amhevc_stop();
		pbi->stat &= ~STAT_VDEC_RUN;
	}

	if (pbi->stat & STAT_TIMER_ARM) {
		del_timer_sync(&pbi->timer);
		pbi->stat &= ~STAT_TIMER_ARM;
	}
	if (pbi->dec_result != DEC_RESULT_NEED_MORE_BUFFER)
		ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_WORKER_END);
	/* mark itself has all HW resource released and input released */
#ifdef SUPPORT_FB_DECODING
	if (pbi->used_stage_buf_num > 0)
		vdec_core_finish_run(hw_to_vdec(pbi), CORE_MASK_HEVC_BACK);
	else
		vdec_core_finish_run(hw_to_vdec(pbi), CORE_MASK_VDEC_1
				| CORE_MASK_HEVC
				| CORE_MASK_HEVC_FRONT
				| CORE_MASK_HEVC_BACK
				);
#else
	if (vdec->parallel_dec == 1)
		vdec_core_finish_run(vdec, CORE_MASK_HEVC);
	else
		vdec_core_finish_run(hw_to_vdec(pbi), CORE_MASK_VDEC_1
					| CORE_MASK_HEVC);
#endif
	trigger_schedule(pbi);
}

static int vp9_hw_ctx_restore(struct VP9Decoder_s *pbi)
{
	/* new to do ... */
#if (!defined SUPPORT_FB_DECODING)
	vvp9_prot_init(pbi, HW_MASK_FRONT | HW_MASK_BACK);
#elif (defined FB_DECODING_TEST_SCHEDULE)
	vvp9_prot_init(pbi, HW_MASK_FRONT | HW_MASK_BACK);
#else
	if (pbi->used_stage_buf_num > 0)
		vvp9_prot_init(pbi, HW_MASK_FRONT);
	else
		vvp9_prot_init(pbi, HW_MASK_FRONT | HW_MASK_BACK);
#endif
	return 0;
}

static int vp9_reset_frame_buffer(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	struct aml_vcodec_ctx *ctx =
		(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	struct aml_buf *aml_buf;
	ulong flags;
	int i;

	for (i = 0; i < pbi->used_buf_num; ++i) {
		if (frame_bufs[i].buf.cma_alloc_addr) {
			aml_buf = (struct aml_buf *)pbi->m_BUF[i].v4l_ref_buf_addr;
			if (!frame_bufs[i].buf.vf_ref &&
				!(pbi->vframe_dummy.type & VIDTYPE_V4L_EOS)) {
				aml_buf_put_ref(&ctx->bm, aml_buf);
				aml_buf_put_ref(&ctx->bm, aml_buf);
			} else
				aml_buf_put_ref(&ctx->bm, aml_buf);

			lock_buffer_pool(cm->buffer_pool, flags);
			frame_bufs[i].buf.cma_alloc_addr = 0;
			frame_bufs[i].buf.vf_ref = 0;
			frame_bufs[i].ref_count = 0;
			pbi->m_BUF[i].v4l_ref_buf_addr = 0;

			atomic_add(1, &pbi->vf_put_count);
			unlock_buffer_pool(cm->buffer_pool, flags);
		}
	}

	return 0;
}

static int vp9_recycle_frame_buffer(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	struct aml_vcodec_ctx *ctx =
		(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	struct aml_buf *aml_buf;
	ulong flags;
	int i;

	for (i = 0; i < pbi->used_buf_num; ++i) {
		if (frame_bufs[i].ref_count == 0 &&
			frame_bufs[i].buf.cma_alloc_addr &&
			(frame_bufs[i].buf.repeat_count == 0) &&
			&frame_bufs[i] != cm->cur_frame) {

			if (!frame_bufs[i].buf.vf_ref && frame_bufs[i].buf.repeat_pic)
				continue;
			frame_bufs[i].buf.repeat_pic = NULL;

			aml_buf = (struct aml_buf *)pbi->m_BUF[i].v4l_ref_buf_addr;

			vp9_print(pbi, VP9_DEBUG_BUFMGR,
				"%s buf idx: %d fb: 0x%lx vb idx: %d dma addr: 0x%lx vf_ref %d\n",
				__func__, i, pbi->m_BUF[i].v4l_ref_buf_addr,
				aml_buf->index,
				frame_bufs[i].buf.cma_alloc_addr,
				frame_bufs[i].buf.vf_ref);

			aml_buf_put_ref(&ctx->bm, aml_buf);
			if (!frame_bufs[i].buf.vf_ref) {
				aml_buf_put_ref(&ctx->bm, aml_buf);
			}

			if (pbi->mmu_enable && ctx->no_fbc_output) {
				if (aml_buf->fbc->used[aml_buf->fbc->index] & 1) {
					decoder_mmu_box_free_idx(aml_buf->fbc->mmu,
								aml_buf->fbc->index);
					aml_buf->fbc->used[aml_buf->fbc->index] &= ~0x1;
					vp9_print(pbi, VP9_DEBUG_BUFMGR,
						"free mmu buffer frame idx %d afbc_index: %d, dma addr: 0x%lx\n",
						frame_bufs[i].buf.index,
						aml_buf->fbc->index,
						frame_bufs[i].buf.cma_alloc_addr);
				}
			}

			lock_buffer_pool(cm->buffer_pool, flags);

			frame_bufs[i].buf.cma_alloc_addr = 0;
			frame_bufs[i].buf.vf_ref = 0;
			pbi->m_BUF[i].v4l_ref_buf_addr = 0;

			atomic_add(1, &pbi->vf_put_count);

			unlock_buffer_pool(cm->buffer_pool, flags);
			break;
		}
	}

	return 0;
}


static bool is_available_buffer(struct VP9Decoder_s *pbi)
{
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	struct aml_vcodec_ctx *ctx =
		(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	int i, free_count = 0;
	int free_slot = 0;

	pbi->cur_idx = INVALID_IDX;

	/* Ignore the buffer available check until the head parse done. */
	if (!pbi->v4l_params_parsed) {
		/*
		 * If a resolution change and eos are detected, decoding will
		 * wait until the first valid buffer queue in driver
		 * before scheduling continues.
		 */
		if (ctx->v4l_resolution_change) {
			if (pbi->eos)
				return false;

			/* Wait for buffers ready. */
			if (!ctx->dst_queue_streaming)
				return false;
		} else {
			return true;
		}
	}

	/* Wait for the buffer number negotiation to complete. */
	if (pbi->used_buf_num == 0) {
		struct vdec_pic_info pic;

		vdec_v4l_get_pic_info(ctx, &pic);
		pbi->used_buf_num = pic.dpb_frames + pic.dpb_margin;

		if (pbi->used_buf_num > MAX_BUF_NUM)
			pbi->used_buf_num = MAX_BUF_NUM;

		if (pbi->used_buf_num == 0)
			return false;
	}

	vp9_recycle_frame_buffer(pbi);

	for (i = 0; i < pbi->used_buf_num; ++i) {
		if ((frame_bufs[i].ref_count == 0) &&
			(frame_bufs[i].buf.vf_ref == 0) &&
			!frame_bufs[i].buf.cma_alloc_addr &&
			(frame_bufs[i].buf.repeat_count == 0) &&
			(cm->cur_frame != &frame_bufs[i])) {
			free_slot++;
		}
	}

	if (free_slot < pbi->run_ready_min_buf_num) {
		vp9_print(pbi,
		PRINT_FLAG_VDEC_DETAIL, "%s not enough free_slot %d!\n",
		__func__, free_slot);
		force_recycle_repeat_frame(pbi);

		return false;
	} else if (free_slot < 2)
		force_recycle_repeat_frame(pbi);

	if (atomic_read(&ctx->vpp_cache_num) >= MAX_VPP_BUFFER_CACHE_NUM) {
		vp9_print(pbi, PRINT_FLAG_VDEC_DETAIL,
			"%s vpp cache: %d full!\n",
			__func__, atomic_read(&ctx->vpp_cache_num));

		return false;
	}

	if (!pbi->aml_buf && !aml_buf_empty(&ctx->bm)) {
		pbi->aml_buf = aml_buf_get(&ctx->bm, BUF_USER_DEC, false);
		if (!pbi->aml_buf) {
			return false;
		}

		pbi->aml_buf->task->attach(pbi->aml_buf->task, &task_dec_ops, hw_to_vdec(pbi));
		pbi->aml_buf->state = FB_ST_DECODER;
	}

	if (pbi->aml_buf) {
		free_count++;
		free_count += aml_buf_ready_num(&ctx->bm);
		vp9_print(pbi,
		PRINT_FLAG_VDEC_DETAIL, "%s get fb: 0x%lx fb idx: %d\n",
		__func__, pbi->aml_buf, pbi->aml_buf->index);
	}

	vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_1, free_count);

	return (free_count >= pbi->run_ready_min_buf_num) ? true : false;
}

static unsigned long run_ready(struct vdec_s *vdec, unsigned long mask)
{
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)vdec->private;
	struct aml_vcodec_ctx *ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	int tvp = vdec_secure(hw_to_vdec(pbi)) ? CODEC_MM_FLAGS_TVP : 0;
	unsigned long ret = 0;

	if (!pbi->pic_list_init_done2 || pbi->eos)
		return ret;
	if (!pbi->first_sc_checked && pbi->mmu_enable) {
		int size = decoder_mmu_box_sc_check(ctx->bm.mmu, tvp);

		pbi->first_sc_checked = 1;
		vp9_print(pbi, 0, "vp9 cached=%d  need_size=%d speed= %d ms\n",
			size, (pbi->need_cache_size >> PAGE_SHIFT),
			(int)(get_jiffies_64() - pbi->sc_start_time) * 1000/HZ);
	}

#ifdef SUPPORT_FB_DECODING
	if (pbi->used_stage_buf_num > 0) {
		if (mask & CORE_MASK_HEVC_FRONT) {
			if (get_free_stage_buf_num(pbi) > 0
				&& mv_buf_available(pbi))
				ret |= CORE_MASK_HEVC_FRONT;
		}
		if (mask & CORE_MASK_HEVC_BACK) {
			if (s2_buf_available(pbi) &&
				(get_free_buf_count(pbi) >=
				pbi->run_ready_min_buf_num)) {
				ret |= CORE_MASK_HEVC_BACK;
				pbi->back_not_run_ready = 0;
			} else
				pbi->back_not_run_ready = 1;
		}
	} else if (get_free_buf_count(pbi) >=
		pbi->run_ready_min_buf_num)
		ret = CORE_MASK_VDEC_1 | CORE_MASK_HEVC
			| CORE_MASK_HEVC_FRONT
			| CORE_MASK_HEVC_BACK;

	if (ret & CORE_MASK_HEVC_FRONT)
		not_run_ready[pbi->index] = 0;
	else
		not_run_ready[pbi->index]++;

	if (ret & CORE_MASK_HEVC_BACK)
		not_run2_ready[pbi->index] = 0;
	else
		not_run2_ready[pbi->index]++;

	vp9_print(pbi,
		PRINT_FLAG_VDEC_DETAIL, "%s mask %lx=>%lx (%d %d %d %d)\r\n",
		__func__, mask, ret,
		get_free_stage_buf_num(pbi),
		mv_buf_available(pbi),
		s2_buf_available(pbi),
		get_free_buf_count(pbi)
		);

	return ret;

#else

	ret = is_available_buffer(pbi) ? CORE_MASK_HEVC : 0;
	if (ret)
		not_run_ready[pbi->index] = 0;
	else
		not_run_ready[pbi->index]++;

	vp9_print(pbi,
		PRINT_FLAG_VDEC_DETAIL, "%s mask %lx=>%lx\r\n",
		__func__, mask, ret);
	return ret;
#endif
}

static void vp9_frame_mode_pts_save(struct VP9Decoder_s *pbi)
{
	int i = 0;

	if (pbi->chunk == NULL)
	       return;
	vp9_print(pbi, VP9_DEBUG_OUT_PTS,
	       "run front: pts %d, pts64 %lld, ts: %lld\n",
	       pbi->chunk->pts, pbi->chunk->pts64, pbi->chunk->timestamp);
	for (i = (FRAME_BUFFERS - 1); i > 0; i--) {
		pbi->frame_mode_pts_save[i] = pbi->frame_mode_pts_save[i - 1];
		pbi->frame_mode_pts64_save[i] = pbi->frame_mode_pts64_save[i - 1];
	}
	pbi->frame_mode_pts_save[0] = pbi->chunk->pts;
	pbi->frame_mode_pts64_save[0] = pbi->chunk->pts64;

	if (!v4l_bitstream_id_enable)
		pbi->frame_mode_pts64_save[0] = pbi->chunk->timestamp;
}

static void run_front(struct vdec_s *vdec)
{
	struct VP9Decoder_s *pbi =
		(struct VP9Decoder_s *)vdec->private;
	struct aml_vcodec_ctx *ctx =
		(struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	int ret, size;

	run_count[pbi->index]++;
#if (!defined SUPPORT_FB_DECODING)
	hevc_reset_core(vdec);
#elif (defined FB_DECODING_TEST_SCHEDULE)
	hevc_reset_core(vdec);
#else
	if (pbi->used_stage_buf_num > 0)
		fb_reset_core(vdec, HW_MASK_FRONT);
	else
		hevc_reset_core(vdec);
#endif

	if ((vdec_frame_based(vdec)) &&
		(pbi->dec_result == DEC_RESULT_UNFINISH)) {
		u32 res_byte = pbi->data_size - pbi->consume_byte;

		vp9_print(pbi, VP9_DEBUG_BUFMGR,
			"%s before, consume 0x%x, size 0x%x, offset 0x%x, res 0x%x\n", __func__,
			pbi->consume_byte, pbi->data_size, pbi->data_offset + pbi->consume_byte, res_byte);

		pbi->data_invalid = vdec_offset_prepare_input(vdec, pbi->consume_byte, pbi->data_offset, pbi->data_size);
		pbi->data_offset -= (pbi->data_invalid - pbi->consume_byte);
		pbi->data_size += (pbi->data_invalid - pbi->consume_byte);
		size = pbi->data_size;
		WRITE_VREG(HEVC_ASSIST_SCRATCH_C, pbi->data_invalid);

		vp9_print(pbi, VP9_DEBUG_BUFMGR,
			"%s after, consume 0x%x, size 0x%x, offset 0x%x, invalid 0x%x, res 0x%x\n", __func__,
			pbi->consume_byte, pbi->data_size, pbi->data_offset, pbi->data_invalid, res_byte);
	} else {
		size = vdec_prepare_input(vdec, &pbi->chunk);
		if (size < 0) {
			input_empty[pbi->index]++;

			pbi->dec_result = DEC_RESULT_AGAIN;

			vp9_print(pbi, PRINT_FLAG_VDEC_DETAIL,
				"ammvdec_vp9: Insufficient data\n");

			vdec_schedule_work(&pbi->work);
			return;
		}
		if ((vdec_frame_based(vdec)) &&
			(pbi->chunk != NULL)) {
			pbi->data_offset = pbi->chunk->offset;
			pbi->data_size = size;
		}
		WRITE_VREG(HEVC_ASSIST_SCRATCH_C, 0);
	}

	vdec_tracing(&ctx->vtr, VTRACE_DEC_ST_0, size);

	input_empty[pbi->index] = 0;
	pbi->dec_result = DEC_RESULT_NONE;
	pbi->start_shift_bytes = READ_VREG(HEVC_SHIFT_BYTE_COUNT);

	vp9_frame_mode_pts_save(pbi);

	if (debug & PRINT_FLAG_VDEC_STATUS) {
		int ii;
		vp9_print(pbi, 0,
			"%s (%d): size 0x%x (0x%x 0x%x) sum 0x%x (%x %x %x %x %x) bytes 0x%x",
			__func__,
			pbi->frame_count, size,
			pbi->chunk ? pbi->data_size : 0,
			pbi->chunk ? pbi->data_offset : 0,
			pbi->chunk ? ((vdec_frame_based(vdec) &&
			(debug & PRINT_FLAG_VDEC_STATUS)) ?
			get_data_check_sum(pbi, size) : 0) : 0,
		READ_VREG(HEVC_STREAM_START_ADDR),
		READ_VREG(HEVC_STREAM_END_ADDR),
		READ_VREG(HEVC_STREAM_LEVEL),
		READ_VREG(HEVC_STREAM_WR_PTR),
		READ_VREG(HEVC_STREAM_RD_PTR),
		pbi->start_shift_bytes);

		if (!(vdec_secure(hw_to_vdec(pbi))) &&
			vdec_frame_based(vdec) && pbi->chunk) {
			u8 *data = NULL;

			if (!pbi->chunk->block->is_mapped)
				data = codec_mm_vmap(pbi->chunk->block->start +
					pbi->data_offset, 8);
			else
				data = ((u8 *)pbi->chunk->block->start_virt) +
					pbi->data_offset;

			vp9_print_cont(pbi, 0, "data adr %p:",
				data);
			for (ii = 0; ii < 8; ii++)
				vp9_print_cont(pbi, 0, "%02x ",
					data[ii]);

			if (!pbi->chunk->block->is_mapped)
				codec_mm_unmap_phyaddr(data);
		}
		vp9_print_cont(pbi, 0, "\r\n");
	}
	ATRACE_COUNTER(pbi->trace.decode_run_time_name, TRACE_RUN_LOADING_FW_START);
	if (vdec->mc_loaded) {
	/*firmware have load before,
	  and not changes to another.
	  ignore reload.
	*/
	} else {
			ret = amhevc_loadmc_ex(VFORMAT_VP9, NULL, pbi->fw->data);
			if (ret < 0) {
			amhevc_disable();
			vp9_print(pbi, PRINT_FLAG_ERROR,
				"VP9: the %s fw loading failed, err: %x\n",
				fw_tee_enabled() ? "TEE" : "local", ret);
			pbi->dec_result = DEC_RESULT_FORCE_EXIT;
			vdec_v4l_post_error_event(ctx, DECODER_EMERGENCY_FW_LOAD_ERROR);
			vdec_schedule_work(&pbi->work);
			return;
		}
		vdec->mc_loaded = 1;
		vdec->mc_type = VFORMAT_VP9;
	}
	ATRACE_COUNTER(pbi->trace.decode_run_time_name, TRACE_RUN_LOADING_FW_END);

	ATRACE_COUNTER(pbi->trace.decode_run_time_name, TRACE_RUN_LOADING_RESTORE_START);
	if (vp9_hw_ctx_restore(pbi) < 0) {
		vdec_schedule_work(&pbi->work);
		return;
	}
	ATRACE_COUNTER(pbi->trace.decode_run_time_name, TRACE_RUN_LOADING_RESTORE_END);
	vdec_enable_input(vdec);

	WRITE_VREG(HEVC_DEC_STATUS_REG, HEVC_ACTION_DONE);

	if (vdec_frame_based(vdec)) {
		if (debug & PRINT_FLAG_VDEC_DATA)
			dump_data(pbi, pbi->data_size);

		WRITE_VREG(HEVC_SHIFT_BYTE_COUNT, 0);
		size = pbi->data_size +
			(pbi->data_offset & (VDEC_FIFO_ALIGN - 1));
		if (vdec->mvfrm)
			vdec->mvfrm->frame_size = pbi->data_size;
		ctx->current_timestamp = pbi->chunk->timestamp;
	}
	WRITE_VREG(HEVC_DECODE_SIZE, size);
	WRITE_VREG(HEVC_DECODE_COUNT, pbi->slice_idx);
	pbi->init_flag = 1;
	pbi->cur_idx = INVALID_IDX;

	vp9_print(pbi, PRINT_FLAG_VDEC_DETAIL,
		"%s: start hevc (%x %x %x)\n",
		__func__,
		READ_VREG(HEVC_DEC_STATUS_REG),
		READ_VREG(HEVC_MPC_E),
		READ_VREG(HEVC_MPSR));

	start_process_time(pbi);
	mod_timer(&pbi->timer, jiffies);
	pbi->stat |= STAT_TIMER_ARM;
	pbi->stat |= STAT_ISR_REG;
	amhevc_start();
	pbi->stat |= STAT_VDEC_RUN;
}

#ifdef SUPPORT_FB_DECODING
static void mpred_process(struct VP9Decoder_s *pbi)
{
	union param_u *params = &pbi->s1_param;
	unsigned char use_prev_frame_mvs =
		!params->p.error_resilient_mode &&
		params->p.width == pbi->s1_width &&
		params->p.height == pbi->s1_height &&
		!pbi->s1_intra_only &&
		pbi->s1_last_show_frame &&
		(pbi->s1_frame_type != KEY_FRAME);
	pbi->s1_width = params->p.width;
	pbi->s1_height = params->p.height;
	pbi->s1_frame_type = params->p.frame_type;
	pbi->s1_intra_only =
		(params->p.show_frame ||
		params->p.show_existing_frame)
		? 0 : params->p.intra_only;
	if ((pbi->s1_frame_type != KEY_FRAME)
		&& (!pbi->s1_intra_only)) {
		unsigned int data32;
		int mpred_mv_rd_end_addr;

		mpred_mv_rd_end_addr =
			pbi->s1_mpred_mv_wr_start_addr_pre
			+ (pbi->lcu_total * MV_MEM_UNIT);

		WRITE_VREG(HEVC_MPRED_CTRL3, 0x24122412);
		WRITE_VREG(HEVC_MPRED_ABV_START_ADDR,
			pbi->work_space_buf->
			mpred_above.buf_start);

		data32 = READ_VREG(HEVC_MPRED_CTRL4);

		data32 &=  (~(1 << 6));
		data32 |= (use_prev_frame_mvs << 6);
		WRITE_VREG(HEVC_MPRED_CTRL4, data32);

		WRITE_VREG(HEVC_MPRED_MV_WR_START_ADDR,
				pbi->s1_mpred_mv_wr_start_addr);
		WRITE_VREG(HEVC_MPRED_MV_WPTR,
			pbi->s1_mpred_mv_wr_start_addr);

		WRITE_VREG(HEVC_MPRED_MV_RD_START_ADDR,
			pbi->s1_mpred_mv_wr_start_addr_pre);
		WRITE_VREG(HEVC_MPRED_MV_RPTR,
			pbi->s1_mpred_mv_wr_start_addr_pre);

		WRITE_VREG(HEVC_MPRED_MV_RD_END_ADDR,
			mpred_mv_rd_end_addr);

	} else
		clear_mpred_hw(pbi);

	if (!params->p.show_existing_frame) {
		pbi->s1_mpred_mv_wr_start_addr_pre =
			pbi->s1_mpred_mv_wr_start_addr;
		pbi->s1_last_show_frame =
			params->p.show_frame;
		if (pbi->s1_mv_buf_index_pre_pre != MV_BUFFER_NUM)
			put_mv_buf(pbi, &pbi->s1_mv_buf_index_pre_pre);
		pbi->s1_mv_buf_index_pre_pre =
			pbi->s1_mv_buf_index_pre;
		pbi->s1_mv_buf_index_pre = pbi->s1_mv_buf_index;
	} else
		put_mv_buf(pbi, &pbi->s1_mv_buf_index);
}

static void vp9_s1_work(struct work_struct *s1_work)
{
	struct VP9Decoder_s *pbi = container_of(s1_work,
		struct VP9Decoder_s, s1_work);
	vp9_print(pbi, PRINT_FLAG_VDEC_DETAIL,
		"%s dec_s1_result %d\n",
		__func__,
		pbi->dec_s1_result);

#ifdef FB_DECODING_TEST_SCHEDULE
	if (pbi->dec_s1_result ==
		DEC_S1_RESULT_TEST_TRIGGER_DONE) {
		pbi->s1_test_cmd = TEST_SET_PIC_DONE;
		WRITE_VREG(HEVC_ASSIST_MBOX0_IRQ_REG, 0x1);
	}
#endif
	if (pbi->dec_s1_result == DEC_S1_RESULT_DONE ||
		pbi->dec_s1_result == DEC_S1_RESULT_FORCE_EXIT) {

		vdec_core_finish_run(hw_to_vdec(pbi),
			CORE_MASK_HEVC_FRONT);

		trigger_schedule(pbi);
		/*pbi->dec_s1_result = DEC_S1_RESULT_NONE;*/
	}

}

static void run_back(struct vdec_s *vdec)
{
	struct VP9Decoder_s *pbi =
		(struct VP9Decoder_s *)vdec->private;
	int i;
	run2_count[pbi->index]++;
	if (debug & PRINT_FLAG_VDEC_STATUS) {
		vp9_print(pbi, 0,
			"%s", __func__);
	}
	pbi->run2_busy = 1;
#ifndef FB_DECODING_TEST_SCHEDULE
	fb_reset_core(vdec, HW_MASK_BACK);

	vvp9_prot_init(pbi, HW_MASK_BACK);
#endif
	vp9_recycle_mmu_buf_tail(pbi);

	if (pbi->frame_count > 0)
		vp9_bufmgr_postproc(pbi);

	if (get_s2_buf(pbi) >= 0) {
		for (i = 0; i < (RPM_END - RPM_BEGIN); i += 4) {
			int ii;
			for (ii = 0; ii < 4; ii++)
				pbi->vp9_param.l.data[i + ii] =
					pbi->s2_buf->rpm[i + 3 - ii];
		}
#ifndef FB_DECODING_TEST_SCHEDULE
		WRITE_VREG(HEVC_ASSIST_FBD_MMU_MAP_ADDR,
			pbi->stage_mmu_map_phy_addr +
			pbi->s2_buf->index * STAGE_MMU_MAP_SIZE);
#endif
		continue_decoding(pbi);
	}
	pbi->run2_busy = 0;
}
#endif

static void run(struct vdec_s *vdec, unsigned long mask,
	void (*callback)(struct vdec_s *, void *, int), void *arg)
{
	struct VP9Decoder_s *pbi =
		(struct VP9Decoder_s *)vdec->private;
	ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_RUN_START);
	vp9_print(pbi,
		PRINT_FLAG_VDEC_DETAIL, "%s mask %lx\r\n",
		__func__, mask);

	if (vdec->mvfrm)
		vdec->mvfrm->hw_decode_start = local_clock();
	run_count[pbi->index]++;
	pbi->vdec_cb_arg = arg;
	pbi->vdec_cb = callback;
	pbi->one_package_frame_cnt = 0;
#ifdef SUPPORT_FB_DECODING
	if ((mask & CORE_MASK_HEVC) ||
		(mask & CORE_MASK_HEVC_FRONT))
		run_front(vdec);

	if ((pbi->used_stage_buf_num > 0)
		&& (mask & CORE_MASK_HEVC_BACK))
		run_back(vdec);
#else
	run_front(vdec);
#endif
	ATRACE_COUNTER(pbi->trace.decode_time_name, DECODER_RUN_END);
}

static void  vp9_decoder_ctx_reset(struct VP9Decoder_s *pbi)
{
	struct vdec_s *vdec = hw_to_vdec(pbi);
	struct VP9_Common_s *const cm = &pbi->common;
	struct RefCntBuffer_s *const frame_bufs = cm->buffer_pool->frame_bufs;
	struct BufferPool_s *buffer_pool = cm->buffer_pool;
	int i;

	cm->buffer_pool = buffer_pool;

	for (i = 0; i < FRAME_BUFFERS; ++i) {
		frame_bufs[i].buf.index		= i;
		frame_bufs[i].ref_count		= 0;
		frame_bufs[i].buf.vf_ref	= 0;
		frame_bufs[i].buf.decode_idx	= 0;
		frame_bufs[i].buf.cma_alloc_addr = 0;
		frame_bufs[i].buf.BUF_index	= -1;
		frame_bufs[i].buf.slice_type	= 0;
		frame_bufs[i].buf.repeat_pic = NULL;
		frame_bufs[i].buf.repeat_count = 0;
		pbi->repeat_buf.frame_bufs[i] = NULL;
	}

	pbi->repeat_buf.used_size = 0;

	for (i = 0; i < MV_BUFFER_NUM; ++i) {
		pbi->m_mv_BUF[i].used_flag = 0;
	}

	for (i = 0; i < FRAME_BUFFERS; i++) {
		pbi->buffer_wrap[i] = i;
	}

	if (vdec->parallel_dec == 1) {
		for (i = 0; i < FRAME_BUFFERS; i++) {
			vdec->free_canvas_ex
				(pbi->common.buffer_pool->frame_bufs[i].buf.y_canvas_index,
				vdec->id);
			vdec->free_canvas_ex
				(pbi->common.buffer_pool->frame_bufs[i].buf.uv_canvas_index,
				vdec->id);
		}
	}

	for (i = 0; i < BUF_FBC_NUM_MAX; i++) {
		if (pbi->afbc_buf_table[i].used)
			pbi->afbc_buf_table[i].used = 0;
	}

	pbi->init_flag		= 0;
	pbi->first_sc_checked	= 0;
	pbi->fatal_error	= 0;
	pbi->show_frame_num	= 0;
	pbi->postproc_done	= 0;
	pbi->process_busy	= 0;
	pbi->eos		= false;
	pbi->aml_buf		= NULL;
	pbi->dec_result = DEC_RESULT_NONE;
	pbi->cur_idx = INVALID_IDX;
}

static void reset(struct vdec_s *vdec)
{
	struct VP9Decoder_s *pbi =
		(struct VP9Decoder_s *)vdec->private;

	cancel_work_sync(&pbi->set_clk_work);
	cancel_work_sync(&pbi->work);
	if (pbi->stat & STAT_VDEC_RUN) {
		amhevc_stop();
		pbi->stat &= ~STAT_VDEC_RUN;
	}

	if (pbi->stat & STAT_TIMER_ARM) {
		del_timer_sync(&pbi->timer);
		pbi->stat &= ~STAT_TIMER_ARM;
	}

	reset_process_time(pbi);
	vp9_reset_frame_buffer(pbi);

	vp9_local_uninit(pbi);
	if (vvp9_local_init(pbi) < 0)
		vp9_print(pbi, 0, "%s local_init failed \r\n", __func__);

	vp9_decoder_ctx_reset(pbi);

	atomic_set(&pbi->vf_pre_count, 0);
	atomic_set(&pbi->vf_get_count, 0);
	atomic_set(&pbi->vf_put_count, 0);

	if (pbi->ge2d) {
		vdec_ge2d_destroy(pbi->ge2d);
		pbi->ge2d = NULL;
	}

	vp9_print(pbi, 0, "%s\r\n", __func__);
}

static irqreturn_t vp9_irq_cb(struct vdec_s *vdec, int irq)
{
	struct VP9Decoder_s *pbi =
		(struct VP9Decoder_s *)vdec->private;
	return vvp9_isr(0, pbi);
}

static irqreturn_t vp9_threaded_irq_cb(struct vdec_s *vdec, int irq)
{
	struct VP9Decoder_s *pbi =
		(struct VP9Decoder_s *)vdec->private;
	return vvp9_isr_thread_fn(0, pbi);
}

static void vp9_dump_state(struct vdec_s *vdec)
{
	struct VP9Decoder_s *pbi =
		(struct VP9Decoder_s *)vdec->private;
	struct VP9_Common_s *const cm = &pbi->common;
	int i;
	vp9_print(pbi, 0, "====== %s\n", __func__);

	vp9_print(pbi, 0,
		"width/height (%d/%d), used_buf_num %d video_signal_type 0x%x\n",
		cm->width,
		cm->height,
		pbi->used_buf_num,
		pbi->video_signal_type
		);

	vp9_print(pbi, 0,
		"is_framebase(%d), eos %d, dec_result 0x%x dec_frm %d disp_frm %d run %d not_run_ready %d input_empty %d low_latency %d no_head %d \n",
		input_frame_based(vdec),
		pbi->eos,
		pbi->dec_result,
		decode_frame_count[pbi->index],
		display_frame_count[pbi->index],
		run_count[pbi->index],
		not_run_ready[pbi->index],
		input_empty[pbi->index],
		pbi->low_latency_flag,
		pbi->no_head
		);

	vp9_print(pbi, 0,
	"%s, newq(%d/%d), dispq(%d/%d), vf prepare/get/put (%d/%d/%d), free_buf_count %d (min %d for run_ready)\n",
	__func__,
	kfifo_len(&pbi->newframe_q),
	VF_POOL_SIZE,
	kfifo_len(&pbi->display_q),
	VF_POOL_SIZE,
	pbi->vf_pre_count,
	pbi->vf_get_count,
	pbi->vf_put_count,
	check_free_buf_count(pbi),
	pbi->run_ready_min_buf_num
	);

	dump_pic_list(pbi);

	for (i = 0; i < MAX_BUF_NUM; i++) {
		vp9_print(pbi, 0,
			"mv_Buf(%d) start_adr 0x%x size 0x%x used %d\n",
			i,
			pbi->m_mv_BUF[i].start_adr,
			pbi->m_mv_BUF[i].size,
			pbi->m_mv_BUF[i].used_flag);
	}

	vp9_print(pbi, 0,
		"HEVC_DEC_STATUS_REG=0x%x\n",
		READ_VREG(HEVC_DEC_STATUS_REG));
	vp9_print(pbi, 0,
		"HEVC_MPC_E=0x%x\n",
		READ_VREG(HEVC_MPC_E));
	vp9_print(pbi, 0,
		"DECODE_MODE=0x%x\n",
		READ_VREG(DECODE_MODE));
	vp9_print(pbi, 0,
		"NAL_SEARCH_CTL=0x%x\n",
		READ_VREG(NAL_SEARCH_CTL));
	vp9_print(pbi, 0,
		"HEVC_PARSER_LCU_START=0x%x\n",
		READ_VREG(HEVC_PARSER_LCU_START));
	vp9_print(pbi, 0,
		"HEVC_DECODE_SIZE=0x%x\n",
		READ_VREG(HEVC_DECODE_SIZE));
	vp9_print(pbi, 0,
		"HEVC_SHIFT_BYTE_COUNT=0x%x\n",
		READ_VREG(HEVC_SHIFT_BYTE_COUNT));
	vp9_print(pbi, 0,
		"HEVC_STREAM_START_ADDR=0x%x\n",
		READ_VREG(HEVC_STREAM_START_ADDR));
	vp9_print(pbi, 0,
		"HEVC_STREAM_END_ADDR=0x%x\n",
		READ_VREG(HEVC_STREAM_END_ADDR));
	vp9_print(pbi, 0,
		"HEVC_STREAM_LEVEL=0x%x\n",
		READ_VREG(HEVC_STREAM_LEVEL));
	vp9_print(pbi, 0,
		"HEVC_STREAM_WR_PTR=0x%x\n",
		READ_VREG(HEVC_STREAM_WR_PTR));
	vp9_print(pbi, 0,
		"HEVC_STREAM_RD_PTR=0x%x\n",
		READ_VREG(HEVC_STREAM_RD_PTR));
	vp9_print(pbi, 0,
		"PARSER_VIDEO_RP=0x%x\n",
		STBUF_READ(&vdec->vbuf, get_rp));
	vp9_print(pbi, 0,
		"PARSER_VIDEO_WP=0x%x\n",
		STBUF_READ(&vdec->vbuf, get_wp));

	if (input_frame_based(vdec) &&
		(debug & PRINT_FLAG_VDEC_DATA)
		) {
		int jj;
		if (pbi->chunk && pbi->chunk->block &&
			pbi->data_size > 0) {
			u8 *data = NULL;

			if (!pbi->chunk->block->is_mapped)
				data = codec_mm_vmap(
					pbi->chunk->block->start +
					pbi->data_offset,
					pbi->data_size);
			else
				data = ((u8 *)pbi->chunk->block->start_virt)
					+ pbi->data_offset;
			vp9_print(pbi, 0,
				"frame data size 0x%x\n",
				pbi->data_size);
			for (jj = 0; jj < pbi->data_size; jj++) {
				if ((jj & 0xf) == 0)
					vp9_print(pbi, PRINT_FLAG_VDEC_DATA,
						"%06x:", jj);
				vp9_print_cont(pbi, 0,
					"%02x ", data[jj]);
				if (((jj + 1) & 0xf) == 0)
					vp9_print_cont(pbi, 0,
						"\n");
			}

			if (!pbi->chunk->block->is_mapped)
				codec_mm_unmap_phyaddr(data);
		}
	}

}

static int ammvdec_vp9_probe(struct platform_device *pdev)
{
	struct vdec_s *pdata = *(struct vdec_s **)pdev->dev.platform_data;
	int ret;
	int i;
	int config_val;
	int transfer_val;
	struct vframe_content_light_level_s content_light_level;
	struct vframe_master_display_colour_s vf_dp;

	struct VP9Decoder_s *pbi = NULL;
	struct aml_vcodec_ctx *ctx = NULL;

	if (get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_GXL ||
		get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXL ||
		get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T5) {
		pr_info("vp9 unsupported on cpu 0x%x\n", get_cpu_major_id());
		return -EINVAL;
	}
	pr_debug("%s\n", __func__);

	if (pdata == NULL) {
		pr_info("\nammvdec_vp9 memory resource undefined.\n");
		return -EFAULT;
	}

	memset(&vf_dp, 0, sizeof(struct vframe_master_display_colour_s));
	pbi = vmalloc(sizeof(struct VP9Decoder_s));
	if (pbi == NULL) {
		pr_info("\nammvdec_vp9 device data allocation failed\n");
		return -ENOMEM;
	}
	memset(pbi, 0, sizeof(struct VP9Decoder_s));

	/* the ctx from v4l2 driver. */
	pbi->v4l2_ctx = pdata->private;
	ctx = (struct aml_vcodec_ctx *)(pbi->v4l2_ctx);
	ctx->vdec_recycle_dec_resource = vp9_recycle_dec_resource;
	pdata->private = pbi;
	pdata->dec_status = vvp9_dec_status;
	pdata->run_ready = run_ready;
	pdata->run = run;
	pdata->reset = reset;
	pdata->irq_handler = vp9_irq_cb;
	pdata->threaded_irq_handler = vp9_threaded_irq_cb;
	pdata->dump_state = vp9_dump_state;

	pbi->index = pdev->id;

	if (is_rdma_enable()) {
		pbi->rdma_adr = decoder_dma_alloc_coherent(&pbi->rdma_mem_handle,
							RDMA_SIZE, &pbi->rdma_phy_adr, "VP9_RDMA_BUF");
		for (i = 0; i < SCALELUT_DATA_WRITE_NUM; i++) {
			pbi->rdma_adr[i * 4] = HEVC_IQIT_SCALELUT_WR_ADDR & 0xfff;
			pbi->rdma_adr[i * 4 + 1] = i;
			pbi->rdma_adr[i * 4 + 2] = HEVC_IQIT_SCALELUT_DATA & 0xfff;
			pbi->rdma_adr[i * 4 + 3] = 0;
			if (i == SCALELUT_DATA_WRITE_NUM - 1) {
				pbi->rdma_adr[i * 4 + 2] = (HEVC_IQIT_SCALELUT_DATA & 0xfff) | 0x20000;
			}
		}
	}

	snprintf(pbi->trace.vdec_name, sizeof(pbi->trace.vdec_name),
		"vp9-%d", pbi->index);
	snprintf(pbi->trace.pts_name, sizeof(pbi->trace.pts_name),
		"%s-timestamp", pbi->trace.vdec_name);
	snprintf(pbi->trace.new_q_name, sizeof(pbi->trace.new_q_name),
		"%s-newframe_q", pbi->trace.vdec_name);
	snprintf(pbi->trace.disp_q_name, sizeof(pbi->trace.disp_q_name),
		"%s-dispframe_q", pbi->trace.vdec_name);
	snprintf(pbi->trace.decode_time_name, sizeof(pbi->trace.decode_time_name),
		"decoder_time%d", pdev->id);
	snprintf(pbi->trace.decode_run_time_name, sizeof(pbi->trace.decode_run_time_name),
		"decoder_run_time%d", pdev->id);
	snprintf(pbi->trace.decode_header_memory_time_name, sizeof(pbi->trace.decode_header_memory_time_name),
		"decoder_header_time%d", pdev->id);

	if (pdata->use_vfm_path)
		snprintf(pdata->vf_provider_name, VDEC_PROVIDER_NAME_SIZE,
			VFM_DEC_PROVIDER_NAME);
	else
		snprintf(pdata->vf_provider_name, VDEC_PROVIDER_NAME_SIZE,
			MULTI_INSTANCE_PROVIDER_NAME ".%02x", pdev->id & 0xff);

	pbi->provider_name = pdata->vf_provider_name;
	platform_set_drvdata(pdev, pdata);

	pbi->platform_dev = pdev;
	pbi->video_signal_type = 0;
	pbi->m_ins_flag = 1;
	if (is_support_4k_vp9()) {
		pbi->max_pic_w = 4096;
		pbi->max_pic_h = 2304;
		if (hevc_is_support_8k()) {
			pbi->max_pic_w = 8192;
			pbi->max_pic_h = 4608;
		}
	} else {
		pbi->max_pic_w = 1920;
		pbi->max_pic_h = 1088;
	}

	if ((debug & IGNORE_PARAM_FROM_CONFIG) == 0 &&
			pdata->config_len) {
#ifdef MULTI_INSTANCE_SUPPORT
		int vp9_buf_width = 0;
		int vp9_buf_height = 0;
		/*use ptr config for double_write_mode, etc*/
		vp9_print(pbi, 0, "pdata->config=%s\n", pdata->config);
		if (get_config_int(pdata->config, "vp9_double_write_mode",
				&config_val) == 0)
			pbi->double_write_mode = config_val;
		else
			pbi->double_write_mode = double_write_mode;

		if (get_config_int(pdata->config, "save_buffer_mode",
				&config_val) == 0)
			pbi->save_buffer_mode = config_val;
		else
			pbi->save_buffer_mode = 0;
		if (get_config_int(pdata->config, "vp9_buf_width",
				&config_val) == 0) {
			vp9_buf_width = config_val;
		}
		if (get_config_int(pdata->config, "vp9_buf_height",
				&config_val) == 0) {
			vp9_buf_height = config_val;
		}

		if (get_config_int(pdata->config, "no_head",
				&config_val) == 0)
			pbi->no_head = config_val;
		else
			pbi->no_head = no_head;

		/*use ptr config for max_pic_w, etc*/
		if (get_config_int(pdata->config, "vp9_max_pic_w",
				&config_val) == 0) {
				pbi->max_pic_w = config_val;
		}
		if (get_config_int(pdata->config, "vp9_max_pic_h",
				&config_val) == 0) {
				pbi->max_pic_h = config_val;
		}

		if ((pbi->max_pic_w * pbi->max_pic_h)
			< (vp9_buf_width * vp9_buf_height)) {
			pbi->max_pic_w = vp9_buf_width;
			pbi->max_pic_h = vp9_buf_height;
			vp9_print(pbi, 0, "use buf resolution\n");
		}

		if (get_config_int(pdata->config, "sidebind_type",
				&config_val) == 0)
			pbi->sidebind_type = config_val;

		if (get_config_int(pdata->config, "sidebind_channel_id",
				&config_val) == 0)
			pbi->sidebind_channel_id = config_val;

		if (get_config_int(pdata->config,
			"parm_v4l_codec_enable",
			&config_val) == 0)
			pbi->is_used_v4l = config_val;

		if (get_config_int(pdata->config,
			"parm_v4l_buffer_margin",
			&config_val) == 0)
			pbi->dynamic_buf_num_margin = config_val;

		if (get_config_int(pdata->config,
			"parm_v4l_canvas_mem_mode",
			&config_val) == 0)
			pbi->mem_map_mode = config_val;

		if (get_config_int(pdata->config,
			"parm_enable_fence",
			&config_val) == 0)
			pbi->enable_fence = config_val;

		if (get_config_int(pdata->config,
			"parm_fence_usage",
			&config_val) == 0)
			pbi->fence_usage = config_val;

		if (get_config_int(pdata->config,
			"parm_v4l_low_latency_mode",
			&config_val) == 0) {
			pbi->low_latency_flag = (config_val & 1) ? 1 : 0;
			pbi->enable_fence = (config_val & 2) ? 1 : 0;
		}

		if (get_config_int(pdata->config,
			"parm_v4l_duration",
			&config_val) == 0)
			pbi->v4l_duration = config_val;

		if (get_config_int(pdata->config,
			"parm_v4l_metadata_config_flag",
			&config_val) == 0) {
			pbi->high_bandwidth_flag = config_val & VDEC_CFG_FLAG_HIGH_BANDWIDTH;
			if (pbi->high_bandwidth_flag)
				vp9_print(pbi, 0, "high bandwidth\n");
		}
#endif

		get_config_int(pdata->config, "signal_type",
					&pbi->video_signal_type);
		vp9_print(pbi, 0,"video_signal_type 0x%x\n",
					 pbi->video_signal_type);
		if (get_config_int(pdata->config, "HDRStaticInfo",
				&vf_dp.present_flag) == 0
				&& vf_dp.present_flag == 1) {
			get_config_int(pdata->config, "mG.x",
					&vf_dp.primaries[0][0]);
			get_config_int(pdata->config, "mG.y",
					&vf_dp.primaries[0][1]);
			get_config_int(pdata->config, "mB.x",
					&vf_dp.primaries[1][0]);
			get_config_int(pdata->config, "mB.y",
					&vf_dp.primaries[1][1]);
			get_config_int(pdata->config, "mR.x",
					&vf_dp.primaries[2][0]);
			get_config_int(pdata->config, "mR.y",
					&vf_dp.primaries[2][1]);
			get_config_int(pdata->config, "mW.x",
					&vf_dp.white_point[0]);
			get_config_int(pdata->config, "mW.y",
					&vf_dp.white_point[1]);
			get_config_int(pdata->config, "mMaxDL",
					&vf_dp.luminance[0]);
			get_config_int(pdata->config, "mMinDL",
					&vf_dp.luminance[1]);
			get_config_int(pdata->config, "mMaxCLL",
					&content_light_level.max_content);
			get_config_int(pdata->config, "mMaxFALL",
					&content_light_level.max_pic_average);

			get_config_int(pdata->config, "mTransfer",
					&transfer_val);

			if (transfer_val == 0)
				transfer_val = 16;

			vp9_print(pbi, 0, "transfer_val=%d\n",transfer_val);

			vf_dp.content_light_level = content_light_level;
			vf_dp.content_light_level.present_flag = 1;
			if (!pbi->video_signal_type) {
				pbi->video_signal_type = (1 << 29)
					| (5 << 26)	/* unspecified */
					| (0 << 25)	/* limit */
					| (1 << 24)	/* color available */
					| (9 << 16)	/* 2020 */
					| (transfer_val << 8)	/* 2084 */
					| (9 << 0);	/* 2020 */
			}
		}
		pbi->vf_dp = vf_dp;
	} else {
		if (pdata->sys_info) {
			pbi->vvp9_amstream_dec_info = *pdata->sys_info;
			if ((pbi->vvp9_amstream_dec_info.width != 0) &&
				(pbi->vvp9_amstream_dec_info.height != 0)) {
				pbi->max_pic_w = pbi->vvp9_amstream_dec_info.width;
				pbi->max_pic_h = pbi->vvp9_amstream_dec_info.height;
			}
		}

		pbi->double_write_mode = double_write_mode;
	}

	if (no_head & 0x10) {
		pbi->no_head = (no_head & 0xf);
	}

	pbi->endian = HEVC_CONFIG_LITTLE_ENDIAN;

	if (endian)
		pbi->endian = endian;

	pbi->run_ready_min_buf_num = run_ready_min_buf_num - 1 ;

	if (is_oversize(pbi->max_pic_w, pbi->max_pic_h)) {
		pr_err("over size: %dx%d, probe failed\n",
			pbi->max_pic_w, pbi->max_pic_h);
		vdec_v4l_post_error_event(ctx, DECODER_EMERGENCY_UNSUPPORT);
		return -1;
	}

	if (force_config_fence) {
		pbi->enable_fence = true;
		pbi->fence_usage =
			(force_config_fence >> 4) & 0xf;
		if (force_config_fence & 0x2)
			pbi->enable_fence = false;
		vp9_print(pbi, 0, "enable fence: %d, fence usage: %d\n",
			pbi->enable_fence, pbi->fence_usage);
	}

	if (get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_GXL ||
		pbi->double_write_mode == 0x10)
		pbi->mmu_enable = 0;
	else
		pbi->mmu_enable = 1;

	video_signal_type = pbi->video_signal_type;

	if (pdata->sys_info) {
		pbi->vvp9_amstream_dec_info = *pdata->sys_info;
	} else {
		pbi->vvp9_amstream_dec_info.width = 0;
		pbi->vvp9_amstream_dec_info.height = 0;
		pbi->vvp9_amstream_dec_info.rate = 30;
	}

	pbi->low_latency_flag = 1;
	vp9_print(pbi, 0,
			"no_head %d  low_latency %d video_signal_type 0x%x\n",
			pbi->no_head, pbi->low_latency_flag, pbi->video_signal_type);

	if (amvdec_vp9_mmu_init(pbi) < 0) {
		pr_err("vp9 alloc bmmu box failed!!\n");
		/* devm_kfree(&pdev->dev, (void *)pbi); */
		vfree((void *)pbi);
		pdata->dec_status = NULL;
		return -1;
	}

	pbi->cma_alloc_count = PAGE_ALIGN(work_buf_size) / PAGE_SIZE;
	ret = decoder_bmmu_box_alloc_buf_phy(pbi->bmmu_box, WORK_SPACE_BUF_ID,
			pbi->cma_alloc_count * PAGE_SIZE, DRIVER_NAME,
			&pbi->cma_alloc_addr);
	if (ret < 0) {
		uninit_mmu_buffers(pbi);
		/* devm_kfree(&pdev->dev, (void *)pbi); */
		vfree((void *)pbi);
		pdata->dec_status = NULL;
		return ret;
	}

	if (!vdec_secure(hw_to_vdec(pbi))) {
		vdec_mm_dma_flush(pbi->cma_alloc_addr,
				pbi->cma_alloc_count * PAGE_SIZE);
	}

	pbi->buf_start = pbi->cma_alloc_addr;
	pbi->buf_size = work_buf_size;

	pbi->init_flag = 0;
	pbi->first_sc_checked = 0;
	pbi->fatal_error = 0;
	pbi->show_frame_num = 0;

	if (debug) {
		pr_info("===VP9 decoder mem resource 0x%lx size 0x%x\n",
			   pbi->buf_start,
			   pbi->buf_size);
	}

	pbi->cma_dev = pdata->cma_dev;

	mutex_init(&pbi->fence_mutex);

	if (pbi->enable_fence) {
		pdata->sync = vdec_sync_get();
		if (!pdata->sync) {
			vp9_print(pbi, 0, "alloc fence timeline error\n");
			vp9_local_uninit(pbi);
			uninit_mmu_buffers(pbi);
			/* devm_kfree(&pdev->dev, (void *)pbi); */
			vfree((void *)pbi);
			pdata->dec_status = NULL;
			return -1;
		}
		ctx->sync = pdata->sync;
		pdata->sync->usage = pbi->fence_usage;
		vdec_timeline_create(pdata->sync, DRIVER_NAME);
		vdec_timeline_get(pdata->sync);
	}

	if (vvp9_init(pdata) < 0) {
		pr_info("\namvdec_vp9 init failed.\n");
		if (pbi->enable_fence)
			vdec_timeline_put(pdata->sync);
		vp9_local_uninit(pbi);
		uninit_mmu_buffers(pbi);
		/* devm_kfree(&pdev->dev, (void *)pbi); */
		vfree((void *)pbi);
		pdata->dec_status = NULL;
		return -ENODEV;
	}

	ctx->aux_infos.alloc_buffer(ctx, HDR10P_TYPE);

	vdec_set_prepare_level(pdata, start_decode_buf_level);
	hevc_source_changed(VFORMAT_VP9,
			4096, 2048, 60);
#ifdef SUPPORT_FB_DECODING
	if (pbi->used_stage_buf_num > 0)
		vdec_core_request(pdata,
			CORE_MASK_HEVC_FRONT | CORE_MASK_HEVC_BACK);
	else
		vdec_core_request(pdata, CORE_MASK_VDEC_1 | CORE_MASK_HEVC
			| CORE_MASK_HEVC_FRONT | CORE_MASK_HEVC_BACK
					| CORE_MASK_COMBINE);
#else
	if (pdata->parallel_dec == 1)
		vdec_core_request(pdata, CORE_MASK_HEVC);
	else
		vdec_core_request(pdata, CORE_MASK_VDEC_1 | CORE_MASK_HEVC
					| CORE_MASK_COMBINE);
#endif
	pbi->pic_list_init_done2 = true;

	return 0;
}

static int ammvdec_vp9_remove(struct platform_device *pdev)
{
	struct VP9Decoder_s *pbi = (struct VP9Decoder_s *)
		(((struct vdec_s *)(platform_get_drvdata(pdev)))->private);
	struct vdec_s *vdec = hw_to_vdec(pbi);
	int i;
	if (debug)
		pr_info("amvdec_vp9_remove\n");

	vmvp9_stop(pbi);

	if (pbi->ge2d) {
		vdec_ge2d_destroy(pbi->ge2d);
		pbi->ge2d = NULL;
	}

#ifdef SUPPORT_FB_DECODING
	vdec_core_release(hw_to_vdec(pbi), CORE_MASK_VDEC_1 | CORE_MASK_HEVC
		| CORE_MASK_HEVC_FRONT | CORE_MASK_HEVC_BACK
		);
#else
	if (vdec->parallel_dec == 1)
		vdec_core_release(hw_to_vdec(pbi), CORE_MASK_HEVC);
	else
		vdec_core_release(hw_to_vdec(pbi), CORE_MASK_VDEC_1 | CORE_MASK_HEVC);
#endif
	vdec_set_status(hw_to_vdec(pbi), VDEC_STATUS_DISCONNECTED);

	if (vdec->parallel_dec == 1) {
		for (i = 0; i < FRAME_BUFFERS; i++) {
			vdec->free_canvas_ex
				(pbi->common.buffer_pool->frame_bufs[i].buf.y_canvas_index,
				vdec->id);
			vdec->free_canvas_ex
				(pbi->common.buffer_pool->frame_bufs[i].buf.uv_canvas_index,
				vdec->id);
		}
	}

	if (pbi->enable_fence)
		vdec_fence_release(pbi, vdec->sync);

#ifdef DEBUG_PTS
	pr_info("pts missed %ld, pts hit %ld, duration %d\n",
		   pbi->pts_missed, pbi->pts_hit, pbi->frame_dur);
#endif
	mem_map_mode = 0;
	if (is_rdma_enable())
		decoder_dma_free_coherent(pbi->rdma_mem_handle,
				RDMA_SIZE, pbi->rdma_adr, pbi->rdma_phy_adr);

	vfree((void *)pbi);
	return 0;
}

static struct platform_driver ammvdec_vp9_driver = {
	.probe = ammvdec_vp9_probe,
	.remove = ammvdec_vp9_remove,
#ifdef CONFIG_PM
	.suspend = amhevc_suspend,
	.resume = amhevc_resume,
#endif
	.driver = {
		.name = MULTI_DRIVER_NAME,
	}
};
#endif
static struct mconfig vp9_configs[] = {
	MC_PU32("bit_depth_luma", &bit_depth_luma),
	MC_PU32("bit_depth_chroma", &bit_depth_chroma),
	MC_PU32("frame_width", &frame_width),
	MC_PU32("frame_height", &frame_height),
	MC_PU32("debug", &debug),
	MC_PU32("radr", &radr),
	MC_PU32("rval", &rval),
	MC_PU32("pop_shorts", &pop_shorts),
	MC_PU32("dbg_cmd", &dbg_cmd),
	MC_PU32("dbg_skip_decode_index", &dbg_skip_decode_index),
	MC_PU32("endian", &endian),
	MC_PU32("step", &step),
	MC_PU32("udebug_flag", &udebug_flag),
	MC_PU32("decode_pic_begin", &decode_pic_begin),
	MC_PU32("slice_parse_begin", &slice_parse_begin),
	MC_PU32("i_only_flag", &i_only_flag),
	MC_PU32("error_handle_policy", &error_handle_policy),
	MC_PU32("buf_alloc_width", &buf_alloc_width),
	MC_PU32("buf_alloc_height", &buf_alloc_height),
	MC_PU32("buf_alloc_depth", &buf_alloc_depth),
	MC_PU32("buf_alloc_size", &buf_alloc_size),
	MC_PU32("buffer_mode", &buffer_mode),
	MC_PU32("buffer_mode_dbg", &buffer_mode_dbg),
	MC_PU32("max_buf_num", &max_buf_num),
	MC_PU32("dynamic_buf_num_margin", &dynamic_buf_num_margin),
	MC_PU32("mem_map_mode", &mem_map_mode),
	MC_PU32("double_write_mode", &double_write_mode),
	MC_PU32("enable_mem_saving", &enable_mem_saving),
	MC_PU32("force_w_h", &force_w_h),
	MC_PU32("force_fps", &force_fps),
	MC_PU32("max_decoding_time", &max_decoding_time),
	MC_PU32("on_no_keyframe_skiped", &on_no_keyframe_skiped),
	MC_PU32("start_decode_buf_level", &start_decode_buf_level),
	MC_PU32("decode_timeout_val", &decode_timeout_val),
	MC_PU32("vp9_max_pic_w", &vp9_max_pic_w),
	MC_PU32("vp9_max_pic_h", &vp9_max_pic_h),
};
static struct mconfig_node vp9_node;

static void set_debug_flag(const char *module, int debug_flags)
{
	debug = debug_flags;
}

static int __init amvdec_vp9_driver_init_module(void)
{

	struct BuffInfo_s *p_buf_info;

	if ((get_cpu_major_id() <= AM_MESON_CPU_MAJOR_ID_TM2 && !is_cpu_tm2_revb()) ||
		(get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXHD2)) {
		if (is_support_4k_vp9()) {
			if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_SM1)
				p_buf_info = &amvvp9_workbuff_spec[2];
			else
				p_buf_info = &amvvp9_workbuff_spec[1];
		} else
			p_buf_info = &amvvp9_workbuff_spec[0];
	} else { //get_cpu_major_id() > AM_MESON_CPU_MAJOR_ID_TM2 || is_cpu_tm2_revb()
		if (is_support_4k_vp9())
			p_buf_info = &amvvp9_workbuff_spec[5];
		else
			p_buf_info = &amvvp9_workbuff_spec[4];
	}

	register_set_debug_flag_func(DEBUG_AMVDEC_VP9_V4L, set_debug_flag);
	init_buff_spec(NULL, p_buf_info);
	work_buf_size =
		(p_buf_info->end_adr - p_buf_info->start_adr
		 + 0xffff) & (~0xffff);

	if (debug & PRINT_FLAG_VDEC_DETAIL)
		pr_info("amvdec_vp9 module init\n");

	error_handle_policy = 0;

#ifdef ERROR_HANDLE_DEBUG
	dbg_nal_skip_flag = 0;
	dbg_nal_skip_count = 0;
#endif
	udebug_flag = 0;
	decode_pic_begin = 0;
	slice_parse_begin = 0;
	step = 0;
	buf_alloc_size = 0;

	if (platform_driver_register(&ammvdec_vp9_driver)) {
		pr_err("failed to register ammvdec_vp9 driver\n");
		return -ENODEV;
	}

	if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_G12A)
		max_buf_num = MAX_BUF_NUM_LESS;

	vcodec_profile_register_v2("VP9-V4L", VFORMAT_VP9, 1);
	INIT_REG_NODE_CONFIGS("media.decoder", &vp9_node,
		"vp9-v4l", vp9_configs, CONFIG_FOR_RW);
	vcodec_feature_register(VFORMAT_VP9, 1);

	return 0;
}

static void __exit amvdec_vp9_driver_remove_module(void)
{
	pr_debug("amvdec_vp9 module remove.\n");

	platform_driver_unregister(&ammvdec_vp9_driver);
}

/****************************************/

module_param(bit_depth_luma, uint, 0664);
MODULE_PARM_DESC(bit_depth_luma, "\n amvdec_vp9 bit_depth_luma\n");

module_param(bit_depth_chroma, uint, 0664);
MODULE_PARM_DESC(bit_depth_chroma, "\n amvdec_vp9 bit_depth_chroma\n");

module_param(frame_width, uint, 0664);
MODULE_PARM_DESC(frame_width, "\n amvdec_vp9 frame_width\n");

module_param(frame_height, uint, 0664);
MODULE_PARM_DESC(frame_height, "\n amvdec_vp9 frame_height\n");

module_param(debug, uint, 0664);
MODULE_PARM_DESC(debug, "\n amvdec_vp9 debug\n");

module_param(radr, uint, 0664);
MODULE_PARM_DESC(radr, "\n radr\n");

module_param(rval, uint, 0664);
MODULE_PARM_DESC(rval, "\n rval\n");

module_param(pop_shorts, uint, 0664);
MODULE_PARM_DESC(pop_shorts, "\n rval\n");

module_param(dbg_cmd, uint, 0664);
MODULE_PARM_DESC(dbg_cmd, "\n dbg_cmd\n");

module_param(dbg_skip_decode_index, uint, 0664);
MODULE_PARM_DESC(dbg_skip_decode_index, "\n dbg_skip_decode_index\n");

module_param(endian, uint, 0664);
MODULE_PARM_DESC(endian, "\n rval\n");

module_param(step, uint, 0664);
MODULE_PARM_DESC(step, "\n amvdec_vp9 step\n");

module_param(decode_pic_begin, uint, 0664);
MODULE_PARM_DESC(decode_pic_begin, "\n amvdec_vp9 decode_pic_begin\n");

module_param(slice_parse_begin, uint, 0664);
MODULE_PARM_DESC(slice_parse_begin, "\n amvdec_vp9 slice_parse_begin\n");

module_param(i_only_flag, uint, 0664);
MODULE_PARM_DESC(i_only_flag, "\n amvdec_vp9 i_only_flag\n");

module_param(low_latency_flag, uint, 0664);
MODULE_PARM_DESC(low_latency_flag, "\n amvdec_vp9 low_latency_flag\n");

module_param(no_head, uint, 0664);
MODULE_PARM_DESC(no_head, "\n amvdec_vp9 no_head\n");

module_param(error_handle_policy, uint, 0664);
MODULE_PARM_DESC(error_handle_policy, "\n amvdec_vp9 error_handle_policy\n");

module_param(buf_alloc_width, uint, 0664);
MODULE_PARM_DESC(buf_alloc_width, "\n buf_alloc_width\n");

module_param(buf_alloc_height, uint, 0664);
MODULE_PARM_DESC(buf_alloc_height, "\n buf_alloc_height\n");

module_param(buf_alloc_depth, uint, 0664);
MODULE_PARM_DESC(buf_alloc_depth, "\n buf_alloc_depth\n");

module_param(buf_alloc_size, uint, 0664);
MODULE_PARM_DESC(buf_alloc_size, "\n buf_alloc_size\n");

module_param(buffer_mode, uint, 0664);
MODULE_PARM_DESC(buffer_mode, "\n buffer_mode\n");

module_param(buffer_mode_dbg, uint, 0664);
MODULE_PARM_DESC(buffer_mode_dbg, "\n buffer_mode_dbg\n");
/*USE_BUF_BLOCK*/
module_param(max_buf_num, uint, 0664);
MODULE_PARM_DESC(max_buf_num, "\n max_buf_num\n");

module_param(dynamic_buf_num_margin, uint, 0664);
MODULE_PARM_DESC(dynamic_buf_num_margin, "\n dynamic_buf_num_margin\n");

module_param(mv_buf_margin, uint, 0664);
MODULE_PARM_DESC(mv_buf_margin, "\n mv_buf_margin\n");

module_param(mv_buf_dynamic_alloc, uint, 0664);
MODULE_PARM_DESC(mv_buf_dynamic_alloc, "\n mv_buf_dynamic_alloc\n");

module_param(run_ready_min_buf_num, uint, 0664);
MODULE_PARM_DESC(run_ready_min_buf_num, "\n run_ready_min_buf_num\n");

/**/

module_param(mem_map_mode, uint, 0664);
MODULE_PARM_DESC(mem_map_mode, "\n mem_map_mode\n");

#ifdef SUPPORT_10BIT
module_param(double_write_mode, uint, 0664);
MODULE_PARM_DESC(double_write_mode, "\n double_write_mode\n");

module_param(enable_mem_saving, uint, 0664);
MODULE_PARM_DESC(enable_mem_saving, "\n enable_mem_saving\n");

module_param(force_w_h, uint, 0664);
MODULE_PARM_DESC(force_w_h, "\n force_w_h\n");
#endif

module_param(force_fps, uint, 0664);
MODULE_PARM_DESC(force_fps, "\n force_fps\n");

module_param(max_decoding_time, uint, 0664);
MODULE_PARM_DESC(max_decoding_time, "\n max_decoding_time\n");

module_param(on_no_keyframe_skiped, uint, 0664);
MODULE_PARM_DESC(on_no_keyframe_skiped, "\n on_no_keyframe_skiped\n");

module_param(mcrcc_cache_alg_flag, uint, 0664);
MODULE_PARM_DESC(mcrcc_cache_alg_flag, "\n mcrcc_cache_alg_flag\n");

#ifdef MULTI_INSTANCE_SUPPORT
module_param(start_decode_buf_level, int, 0664);
MODULE_PARM_DESC(start_decode_buf_level,
		"\n vp9 start_decode_buf_level\n");

module_param(decode_timeout_val, uint, 0664);
MODULE_PARM_DESC(decode_timeout_val,
	"\n vp9 decode_timeout_val\n");

module_param(vp9_max_pic_w, uint, 0664);
MODULE_PARM_DESC(vp9_max_pic_w, "\n vp9_max_pic_w\n");

module_param(vp9_max_pic_h, uint, 0664);
MODULE_PARM_DESC(vp9_max_pic_h, "\n vp9_max_pic_h\n");

module_param_array(decode_frame_count, uint,
	&max_decode_instance_num, 0664);

module_param_array(display_frame_count, uint,
	&max_decode_instance_num, 0664);

module_param_array(max_process_time, uint,
	&max_decode_instance_num, 0664);

module_param_array(run_count, uint,
	&max_decode_instance_num, 0664);

module_param_array(input_empty, uint,
	&max_decode_instance_num, 0664);

module_param_array(not_run_ready, uint,
	&max_decode_instance_num, 0664);
#endif

#ifdef SUPPORT_FB_DECODING
module_param_array(not_run2_ready, uint,
	&max_decode_instance_num, 0664);

module_param_array(run2_count, uint,
	&max_decode_instance_num, 0664);

module_param(stage_buf_num, uint, 0664);
MODULE_PARM_DESC(stage_buf_num, "\n amvdec_vp9 stage_buf_num\n");
#endif
module_param(force_bufspec, uint, 0664);
MODULE_PARM_DESC(force_bufspec, "\n amvdec_vp9 force_bufspec\n");

module_param(udebug_flag, uint, 0664);
MODULE_PARM_DESC(udebug_flag, "\n amvdec_vp9 udebug_flag\n");

module_param(udebug_pause_pos, uint, 0664);
MODULE_PARM_DESC(udebug_pause_pos, "\n udebug_pause_pos\n");

module_param(udebug_pause_val, uint, 0664);
MODULE_PARM_DESC(udebug_pause_val, "\n udebug_pause_val\n");

module_param(udebug_pause_decode_idx, uint, 0664);
MODULE_PARM_DESC(udebug_pause_decode_idx, "\n udebug_pause_decode_idx\n");

module_param(without_display_mode, uint, 0664);
MODULE_PARM_DESC(without_display_mode, "\n without_display_mode\n");

module_param(force_config_fence, uint, 0664);
MODULE_PARM_DESC(force_config_fence, "\n force enable fence\n");

module_param(force_pts_unstable, uint, 0664);
MODULE_PARM_DESC(force_pts_unstable, "\n force_pts_unstable\n");

module_param(v4l_bitstream_id_enable, uint, 0664);
MODULE_PARM_DESC(v4l_bitstream_id_enable, "\n v4l_bitstream_id_enable\n");

module_init(amvdec_vp9_driver_init_module);
module_exit(amvdec_vp9_driver_remove_module);

MODULE_DESCRIPTION("AMLOGIC vp9 Video Decoder Driver");
MODULE_LICENSE("GPL");

