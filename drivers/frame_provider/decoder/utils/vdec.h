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
#ifndef VDEC_H
#define VDEC_H
#include <linux/amlogic/media/utils/amports_config.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/irqreturn.h>
#include <linux/videodev2.h>
#include <linux/amlogic/media/utils/amstream.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#define KERNEL_ATRACE_TAG KERNEL_ATRACE_TAG_VDEC
#include <trace/events/meson_atrace.h>
/*#define CONFIG_AM_VDEC_DV*/
#include "../../../stream_input/amports/streambuf.h"
#include "../../../stream_input/amports/stream_buffer_base.h"
#include "../../../framerate_adapter/video_framerate_adapter.h"
#include "../../../include/regs/dos_registers.h"
#include "../../../common/register/register.h"

#include "vdec_input.h"
#include "frame_check.h"
#include "vdec_sync.h"
#include "vdec_canvas_utils.h"
#include "decoder_report.h"
#include "../../../media_sync/pts_server/pts_server_core.h"
#include "../../../amvdec_ports/utils/common.h"

#define NEW_FB_CODE
#define FB_DEBUG_ON_OLD_CHIP
#ifdef NEW_FB_CODE
#define NEW_FRONT_BACK_CODE
#endif

s32 vdec_dev_register(void);
s32 vdec_dev_unregister(void);

int vdec_source_changed(int format, int width, int height, int fps);
int vdec2_source_changed(int format, int width, int height, int fps);
int hevc_source_changed(int format, int width, int height, int fps);
struct device *get_vdec_device(void);
int vdec_module_init(void);
void vdec_module_exit(void);

#ifdef LIMIT_DECODE_INSTANCE
#define MAX_INSTANCE_MUN     LIMIT_DECODE_INSTANCE
#else
#define MAX_INSTANCE_MUN     9
#endif

#define VDEC_DEBUG_SUPPORT

#define DEC_FLAG_HEVC_WORKAROUND 0x01

#define VDEC_FIFO_ALIGN 8
#define VDEC_DBG_ENABLE_TIME_DEBUG (0x400)
#define VDEC_DBG_ENABLE_HW_TIME_DEBUG (0x2000)

enum vdec_type_e {
	VDEC_1 = 0,
	VDEC_HCODEC,
	VDEC_2,
	VDEC_HEVC,
	VDEC_HEVCB,
	VDEC_WAVE,
	VDEC_MAX
};

struct trace_decoder_name {
	char set_canvas0_addr[32];
	char get_canvas0_addr[32];
	char put_canvas0_addr[32];
	char vf_put_name[32];
	char vf_get_name[32];
	char vdec_name[32];
	char pts_name[32];
	char new_q_name[32];
	char disp_q_name[32];
	char decode_time_name[32];
	char decode_run_time_name[32];
	char decode_header_time_name[32];
	char decode_work_time_name[32];
	char decode_header_memory_time_name[32];
	char decode_back_time_name[32];
	char decode_back_run_time_name[32];
	char decode_back_work_time_name[32];
	char decode_back_ready_name[32];
};


enum e_trace_decoder_status {
	DECODER_RUN_START = 1,
	DECODER_RUN_END   = 2,
	DECODER_ISR_HEAD_DONE = 3,
	DECODER_ISR_PIC_DONE = 4,
	DECODER_ISR_END   = 5,
	DECODER_ISR_THREAD_HEAD_START = 6,
	DECODER_ISR_THREAD_PIC_DONE_START = 7,
	DECODER_ISR_THREAD_EDN = 8,
	DECODER_WORKER_START   = 9,
	DECODER_WORKER_END = 10,
	DECODER_WORKER_AGAIN = 11,
	DECODER_ISR_SEI_DONE = 12,
	DECODER_ISR_THREAD_SEI_START = 13,
	DECODER_ISR_AUX_DONE = 14,
	DECODER_ISR_THREAD_AUX_START = 15,
	DECODER_ISR_THREAD_HEAD_END = 16,
};

enum e_trace_run_status {
	TRACE_RUN_LOADING_FW_START = 1,
	TRACE_RUN_LOADING_FW_END   = 2,
	TRACE_RUN_LOADING_RESTORE_START = 3,
	TRACE_RUN_LOADING_RESTORE_END = 4,
	TRACE_RUN_BACK_ALLOC_MMU_START = 5,
	TRACE_RUN_BACK_ALLOC_MMU_END = 6,
	TRACE_RUN_BACK_CONFIGURE_REGISTER_START = 7,
	TRACE_RUN_BACK_CONFIGURE_REGISTER_END = 8,
	TRACE_RUN_BACK_FW_START = 9,
	TRACE_RUN_BACK_FW_END = 10,
};

enum e_trace_header_status {
	TRACE_HEADER_MEMORY_START = 1,
	TRACE_HEADER_MEMORY_END   = 2,
	TRACE_HEADER_REGISTER_START = 3,
	TRACE_HEADER_REGISTER_END   = 4,
	TRACE_HEADER_RPM_START = 5,
	TRACE_HEADER_RPM_END   = 6,
};

enum e_trace_work_status {
	TRACE_WORK_WAIT_SEARCH_DONE_START = 1,
	TRACE_WORK_WAIT_SEARCH_DONE_END   = 2,
};


#define VDEC_CFG_FLAG_DV_TWOLAYER (1 << 0)
#define VDEC_CFG_FLAG_DV_NEGATIVE  (1 << 1)
#define VDEC_CFG_FLAG_HIGH_BANDWIDTH  (1 << 2)
#define VDEC_CFG_FLAG_DIS_ERR_POLICY (1 << 11)
#define VDEC_CFG_FLAG_MMU_COPY_DISABLE (1 << 12)

#define VDEC_CFG_FLAG_PROG_ONLY (1 << 16)

#define UVM_META_DATA_VF_BASE_INFOS (1 << 0)
#define UVM_META_DATA_HDR10P_DATA (1 << 1)

//WARNING(0-15)
#define	DECODER_WARNING_DECODER_TIMEOUT		1 << 0
#define	DECODER_WARNING_DATA_ERROR		1 << 1

//ERROR(16-23)
#define	DECODER_ERROR_ALLOC_BUFFER_FAIL		1 << 16
#define	DECODER_ERROR_PARAM_ERROR		1 << 17

//EMERGENCY(24-31)
#define	DECODER_EMERGENCY_NO_MEM		1 << 24
#define	DECODER_EMERGENCY_UNSUPPORT		1 << 25
#define	DECODER_EMERGENCY_FW_LOAD_ERROR		1 << 26

#define	IS_WARNING_TYPE(error_type)		((error_type) & 0xffff)
#define	IS_ERROR_TYPE(error_type)		(((error_type) >> 16) & 0xff)
#define	IS_EMERGENCY_TYPE(error_type)		(((error_type) >> 24) & 0xff)

#define CORE_MASK_VDEC_1 (1 << VDEC_1)
#define CORE_MASK_HCODEC (1 << VDEC_HCODEC)
#define CORE_MASK_VDEC_2 (1 << VDEC_2)
#define CORE_MASK_HEVC (1 << VDEC_HEVC)
#define CORE_MASK_HEVC_FRONT (1 << VDEC_HEVC)
#define CORE_MASK_HEVC_BACK (1 << VDEC_HEVCB)
#define CORE_MASK_COMBINE (1UL << 31)

#define mask_front_core(mask) ((mask & ~CORE_MASK_HEVC_BACK) || (mask == 0))
#define mask_back_core(mask) ((mask & CORE_MASK_HEVC_BACK))

#define PRE_LEVEL_NOT_ENOUGH		0xff

#define DECODER_ON_PROCESS		0x1
#define VDEC_NOT_READY			0x2

#define VDEC_META_DATA_SIZE	(256)
#define HDR10P_BUF_SIZE	(128)
#define SEI_ITU_DATA_SIZE		(5*1024)

/* same size with SEI_BUF_SIZE in aml_vcodec_dec.h */
#define AUX_DATA_SIZE1 (24 * 1024)

#define SEI_TYPE	(1)
#define DV_TYPE		(2)

extern void vdec2_power_mode(int level);
extern void vdec_poweron(enum vdec_type_e core);
extern void vdec_poweroff(enum vdec_type_e core);
extern bool vdec_on(enum vdec_type_e core);
extern void vdec_power_reset(void);

extern int rate_time_avg_cnt;
extern int rate_time_avg_threshold_hi;
extern int rate_time_avg_threshold_lo;

/*irq num as same as .dts*/

/*
 *	interrupts = <0 3 1
 *		0 23 1
 *		0 32 1
 *		0 43 1
 *		0 44 1
 *		0 45 1>;
 *	interrupt-names = "vsync",
 *		"demux",
 *		"parser",
 *		"mailbox_0",
 *		"mailbox_1",
 *		"mailbox_2";
 */
enum vdec_irq_num {
	VSYNC_IRQ = 0,
	DEMUX_IRQ,
	PARSER_IRQ,
	VDEC_IRQ_0,
	VDEC_IRQ_1,
	VDEC_IRQ_2,
	ASSIST_MAILBOX_IRQ0,
	ASSIST_MAILBOX_IRQ1,
	ASSIST_MAILBOX_IRQ2,
	ASSIST_MAILBOX_IRQ3,
	VDEC_IRQ_HEVC_BACK,
	VDEC_IRQ_MAX,
};

enum vdec_fr_hint_state {
	VDEC_NO_NEED_HINT = 0,
	VDEC_NEED_HINT,
	VDEC_HINTED,
};

enum frame_type_flag {
	KEYFRAME_FLAG = 1,	/* Frame is a keyframe (I-frame) */
	PFRAME_FLAG,		/* Frame is a P-frame */
	BFRAME_FLAG,		/* Frame is a B-frame */
};

enum vdec_stuck_state {
	VDEC_NORMAL = 0,
	VDEC_FRONT_SLOW,
	VDEC_BACK_SLOW,
	VDEC_NO_INPUT,
	VDEC_NO_YUV_BUF,
};

extern s32 vdec_request_threaded_irq(enum vdec_irq_num num,
			irq_handler_t handler,
			irq_handler_t thread_fn,
			unsigned long irqflags,
			const char *devname, void *dev);
extern s32 vdec_request_irq(enum vdec_irq_num num, irq_handler_t handler,
	const char *devname, void *dev);
extern void vdec_free_irq(enum vdec_irq_num num, void *dev);

extern void dma_contiguous_early_fixup(phys_addr_t base, unsigned long size);
unsigned int get_vdec_clk_config_settings(void);
void update_vdec_clk_config_settings(unsigned int config);
//unsigned int get_mmu_mode(void);//DEBUG_TMP
//extern void vdec_fill_frame_info(struct vframe_qos_s *vframe_qos, int debug);
extern void vdec_fill_vdec_frame(struct vdec_s *vdec,
				struct vframe_qos_s *vframe_qos,
				struct vdec_info *vinfo,
				struct vframe_s *vf, u32 hw_dec_time);

extern void vdec_vframe_ready(struct vdec_s *vdec, struct vframe_s *vf);
extern void vdec_set_vframe_comm(struct vdec_s *vdec, char *n);

typedef long (*pfun_ptsserver_peek_pts_offset)(int pServerInsId,
	checkout_pts_offset* mCheckoutPtsOffset);

long ptsserver_peek_pts_offset(s32 pServerInsId,checkout_pts_offset* mCheckoutPtsOffset);

struct vdec_s;
enum vformat_t;

/* stream based with single instance decoder driver */
#define VDEC_TYPE_SINGLE           0

/* stream based with multi-instance decoder with HW resource sharing */
#define VDEC_TYPE_STREAM_PARSER    1

/* frame based with multi-instance decoder, input block list based */
#define VDEC_TYPE_FRAME_BLOCK      2

/* frame based with multi-instance decoder, single circular input block */
#define VDEC_TYPE_FRAME_CIRCULAR   3

/* decoder status: uninitialized */
#define VDEC_STATUS_UNINITIALIZED  0

/* decoder status: before the decoder can start consuming data */
#define VDEC_STATUS_DISCONNECTED   1

/* decoder status: decoder should become disconnected once it's not active */
#define VDEC_STATUS_CONNECTED      2

/* decoder status: decoder owns HW resource and is running */
#define VDEC_STATUS_ACTIVE         3

#define VDEC_PROVIDER_NAME_SIZE 16
#define VDEC_RECEIVER_NAME_SIZE 16
#define VDEC_MAP_NAME_SIZE      90

#define VDEC_FLAG_OTHER_INPUT_CONTEXT 0x0
#define VDEC_FLAG_SELF_INPUT_CONTEXT 0x01

#define VDEC_NEED_MORE_DATA_RUN   0x01
#define VDEC_NEED_MORE_DATA_DIRTY 0x02
#define VDEC_NEED_MORE_DATA       0x04

#define SCALELUT_DATA_WRITE_NUM   1024
#define RDMA_SIZE                 (1024 * 4 * 4)

#define VDEC_DATA_MAX_INSTANCE_NUM (MAX_INSTANCE_MUN * 2)
#define VDEC_DATA_NUM 64

#define VDEC_DBG_SCHED_PRIO	(0x1)
#define VDEC_DBG_ALWAYS_LOAD_FW	(0x2)
#define VDEC_DBG_CANVAS_STATUS	(0x4)
#define VDEC_DBG_DETAIL_INFO	(0x8)
#define VDEC_DBG_ENABLE_FENCE	(0x100)

#define ALLOC_AUX_BUF         0x1
#define ALLOC_USER_BUF        0x2
#define ALLOC_HDR10P_BUF      0x4

struct vdec_data_buf_s {
	int alloc_policy; /*bit0:aux buf, bit1:user buf, bit2:hdr10p buf */
	u32 aux_buf_size;
	u32 user_buf_size;
	u32 hdr10p_buf_size;
};

struct vdec_data_s {
	void *private_vdata;
	atomic_t  use_count;
	char *aux_data_buf;
	char *user_data_buf;
	char *hdr10p_data_buf;
	/* alloc_flag:
	 * 0, none allocated
	 * 1, allocated, used
	 * 2, allocated, free
	 */
	u32 alloc_flag;
	u32 aux_buf_size;
	u32 user_buf_size;
	u32 hdr10p_buf_size;
};

struct vdec_data_info_s {
	atomic_t  buffer_count;
	atomic_t use_flag;
	struct codec_mm_cb_s release_callback[VDEC_DATA_NUM];
	struct vdec_data_s data[VDEC_DATA_NUM];
};

struct vdec_data_core_s {
	struct vdec_data_info_s vdata[VDEC_DATA_MAX_INSTANCE_NUM];
	spinlock_t vdec_data_lock;
};

struct mmu_copy_params {
	dma_addr_t mmu_copy_map_phy_addr;
	unsigned long mmu_copy_pre_header_adr;
	unsigned long mmu_copy_err_header_adr;
	int pic_w;
	int pic_h;
	int err_width;
	int err_height;
	u32 x_location;
	u32 y_location;
	int count;
};

struct vdec_info_statistic_s {
	struct vdec_info vstatus;
	struct aspect_ratio_info aspect_ratio;
	int ext_info_valid;
};

struct vdec_s {
	u32 magic;
	struct list_head list;
	unsigned long core_mask;
	unsigned long active_mask;
	unsigned long sched_mask;
	int id;

	struct vdec_s *master;
	struct vdec_s *slave;
	struct stream_port_s *port;
	struct stream_buf_s vbuf;
	int status;
	int next_status;
	int type;
	int port_flag;
	int format;
	u32 pts;
	u64 pts64;
	bool pts_valid;
	u64 timestamp;
	bool timestamp_valid;
	int flag;
	int sched;
	int need_more_data;
	u32 canvas_mode;	//canvas block mode

	struct completion inactive_done;

	/* config (temp) */
	unsigned long mem_start;
	unsigned long mem_end;

	void *mm_blk_handle;

	struct device *cma_dev;
	struct platform_device *dev;
	struct dec_sysinfo sys_info_store;
	struct dec_sysinfo *sys_info;

	/* input */
	struct vdec_input_s input;

	/*frame check*/
	struct pic_check_mgr_t vfc;

	/* mc cache */
	u32 mc[4096 * 4];
	bool mc_loaded;
	u32 mc_type;
#ifdef NEW_FB_CODE
	u32 mc_back[4096 * 4];
	bool mc_back_loaded;
	u32 mc_back_type;
#endif
	/* frame provider/receiver interface */
	char vf_provider_name[VDEC_PROVIDER_NAME_SIZE];
	struct vframe_provider_s vframe_provider;
	char *vf_receiver_name;
	char vfm_map_id[VDEC_MAP_NAME_SIZE];
	char vfm_map_chain[VDEC_MAP_NAME_SIZE];
	int vf_receiver_inst;
	enum FRAME_BASE_VIDEO_PATH frame_base_video_path;
	enum vdec_fr_hint_state fr_hint_state;
	bool use_vfm_path;
	char config[PAGE_SIZE];
	int config_len;
	bool is_reset;
	bool dolby_meta_with_el;

	/* canvas */
	int (*get_canvas)(unsigned int index, unsigned int base);
	int (*get_canvas_ex)(int type, int id);
	void (*free_canvas_ex)(int index, int id);

	int (*dec_status)(struct vdec_s *vdec, struct vdec_info *vstatus);
	int (*set_trickmode)(struct vdec_s *vdec, unsigned long trickmode);
	int (*set_isreset)(struct vdec_s *vdec, int isreset);
	void (*vdec_fps_detec)(int id);

	unsigned long (*run_ready)(struct vdec_s *vdec, unsigned long mask);
	unsigned long (*check_input_data)(struct vdec_s *vdec, unsigned long mask);
	void (*run)(struct vdec_s *vdec, unsigned long mask,
			void (*callback)(struct vdec_s *, void *, int), void *);
	void (*reset)(struct vdec_s *vdec);
	void (*dump_state)(struct vdec_s *vdec);
	irqreturn_t (*irq_handler)(struct vdec_s *vdec, int irq);
	irqreturn_t (*threaded_irq_handler)(struct vdec_s *vdec, int irq);
	irqreturn_t (*back_irq_handler)(struct vdec_s *vdec, int irq);
	irqreturn_t (*back_threaded_irq_handler)(struct vdec_s *vdec, int irq);
	int (*user_data_read)(struct vdec_s *vdec,
			struct userdata_param_t *puserdata_para);
	void (*reset_userdata_fifo)(struct vdec_s *vdec, int bInit);
	void (*wakeup_userdata_poll)(struct vdec_s *vdec);
	/* private */
	void *private;       /* decoder per instance specific data */
#ifdef VDEC_DEBUG_SUPPORT
	u64 profile_start_clk[VDEC_MAX];
	u64 total_clk[VDEC_MAX];
	u32 check_count[VDEC_MAX];
	u32 not_run_ready_count[VDEC_MAX];
	u32 input_underrun_count[VDEC_MAX];
	u32 run_count[VDEC_MAX];
	u64 run_clk[VDEC_MAX];
	u64 start_run_clk[VDEC_MAX];
#endif
	u64 irq_thread_cnt;
	u64 irq_cnt;
	int parallel_dec;
	struct vdec_frames_s *mvfrm;
	struct vdec_sync *sync;

	/*aux data check*/
	struct aux_data_check_mgr_t adc;

	u32 hdr10p_data_size;
	char hdr10p_data_buf[PAGE_SIZE];
	bool hdr10p_data_valid;
	u32 profile_idc;
	u32 level_idc;
	bool prog_only;
	bool disable_vfm;
	char name[32];
	char dec_spend_time[32];
	char dec_spend_time_ave[32];
	char dec_back_spend_time[32];
	char dec_back_spend_time_ave[32];
	char dec_event[32];
	char dec_back_event[32];
	char stream_buffer_level[32];
	u32 discard_start_data_flag;
	u32 video_id;
	int is_v4l;
	bool is_stream_mode_dv_multi;
	int pts_server_id;
	u32 afd_video_id;
	u32 inst_cnt;
	wait_queue_head_t idle_wait;
	struct vdec_data_info_s *vdata;
	char frame_size[32];
	spinlock_t power_lock;
	bool suspend;
#ifdef NEW_FB_CODE
	u64 back_irq_thread_cnt;
	u64 back_irq_cnt;
#endif
#ifdef VDEC_DEBUG_SUPPORT
	u32 ready_to_run_count[VDEC_MAX];
#endif
#ifdef NEW_FB_CODE
	bool front_pic_done;
	bool back_pic_done;
	u64 hw_front_decode_start;
	u64 hw_back_decode_start;
#endif
	char frame_code_rate_name[32];
	char decode_hw_front_time_name[32];
	char decode_hw_back_time_name[32];
	char decode_hw_front_spend_time_avg[32];
	char decode_hw_back_spend_time_avg[32];
	u64 code_rate[16];
	uint32_t decoded_count;
	int have_frame_num;
	u64 back_run2cb_time;
	u64 front_run2cb_time;
	char vdec_stuck_state_name[32];
};

#define CODEC_MODE(a, b, c, d)\
	(((u8)(a) << 24) | ((u8)(b) << 16) | ((u8)(c) << 8) | (u8)(d))

#define META_DATA_MAGIC		CODEC_MODE('M', 'E', 'T', 'A')
#define AML_META_HEAD_NUM	8
#define AML_META_HEAD_SIZE	(AML_META_HEAD_NUM * sizeof(u32))


struct aml_meta_head_s {
	u32 magic;
	u32 type;
	u32 data_size;
	u32 data[5];
};

struct aml_vf_base_info_s {
	u32 width;
	u32 height;
	u32 duration;
	u32 frame_type;
	u32 type;
	u32 data[12];
};

struct aml_meta_info_s {
	union {
		struct aml_meta_head_s head;
		u32 buf[AML_META_HEAD_NUM];
	};
	u8 data[0];
};

typedef int (*post_task_handler)(void *args);

struct post_task_mgr_s {
	struct list_head	task_recycle;
	struct task_struct	*task;
	struct semaphore        sem;
	struct mutex		mutex;
	bool 			running;
	void			*private;
};

struct vdec_post_task_parms_s {
	struct list_head	recycle;
	struct task_struct	*task;
	struct completion	park;
	post_task_handler	func;
	void			*private;
	int			scheduled;
};

#define MAX_USERDATA_CHANNEL_NUM 9

typedef struct {
	struct mutex mutex;
	wait_queue_head_t userdata_wait;
	u32 set_id_flag;
	u32 ready_flag[MAX_USERDATA_CHANNEL_NUM];
	int used[MAX_USERDATA_CHANNEL_NUM];
	u32 id[MAX_USERDATA_CHANNEL_NUM];
} st_userdata;

/* common decoder vframe provider name to use default vfm path */
#define VFM_DEC_PROVIDER_NAME "decoder"
#define VFM_DEC_DVBL_PROVIDER_NAME "dvbldec"
#define VFM_DEC_DVEL_PROVIDER_NAME "dveldec"

#define VFM_DEC_DVBL_PROVIDER_NAME2 "dvbldec2"
#define VFM_DEC_DVEL_PROVIDER_NAME2 "dveldec2"

#define hw_to_vdec(hw) ((struct vdec_s *) \
	(platform_get_drvdata(hw->platform_dev)))

#define canvas_y(canvas) ((canvas) & 0xff)
#define canvas_u(canvas) (((canvas) >> 8) & 0xff)
#define canvas_v(canvas) (((canvas) >> 16) & 0xff)
#define canvas_y2(canvas) (((canvas) >> 16) & 0xff)
#define canvas_u2(canvas) (((canvas) >> 24) & 0xff)

#define vdec_frame_based(vdec) \
	(((vdec)->type == VDEC_TYPE_FRAME_BLOCK) || \
	 ((vdec)->type == VDEC_TYPE_FRAME_CIRCULAR))
#define vdec_stream_based(vdec) \
	(((vdec)->type == VDEC_TYPE_STREAM_PARSER) || \
	 ((vdec)->type == VDEC_TYPE_SINGLE))
#define vdec_single(vdec) \
	((vdec)->type == VDEC_TYPE_SINGLE)
#define vdec_dual(vdec) \
	(((vdec)->port->type & PORT_TYPE_DUALDEC) ||\
	 (vdec_get_debug_flags() & 0x100))
#define vdec_secure(vdec) \
	(((vdec)->port_flag & PORT_FLAG_DRM))
#define vdec_dmabuf(vdec) \
	(((vdec)->port_flag & PORT_FLAG_DMABUF))

#define PR_INIT(s)					\
	int __len = 0, __size = s;			\
	u8 __buf[s] = {0}

#define PR_FILL(args...)				\
	do {						\
		if ((__size - __len) <= 0) break;	\
		__len += snprintf(__buf + __len,	\
		__size - __len, ##args);		\
	} while (0)

#define PR_INFO(id)					\
	do {						\
		if (__len == 0) break;			\
		pr_info("[%d] %s\n", id, __buf);	\
		__len = 0;				\
	} while (0)

/* construct vdec structure */
extern struct vdec_s *vdec_create(struct stream_port_s *port,
				struct vdec_s *master);

/* set video format */
extern int vdec_set_format(struct vdec_s *vdec, int format);

/* set PTS */
extern int vdec_set_pts(struct vdec_s *vdec, u32 pts);

extern int vdec_set_pts64(struct vdec_s *vdec, u64 pts64);

/* set vfm map when use frame base decoder */
extern int vdec_set_video_path(struct vdec_s *vdec, int video_path);

/* set receive id when receive is ionvideo or amlvideo */
extern int vdec_set_receive_id(struct vdec_s *vdec, int receive_id);

/* add frame data to input chain */
extern int vdec_write_vframe(struct vdec_s *vdec, const char *buf,
				size_t count, chunk_free free, void* priv);

extern int vdec_write_vframe_with_dma(struct vdec_s *vdec,
	ulong addr, size_t count, u32 handle, chunk_free free, void* priv);

/* mark the vframe_chunk as consumed */
extern void vdec_vframe_dirty(struct vdec_s *vdec,
				struct vframe_chunk_s *chunk);

/* prepare decoder input */
extern int vdec_prepare_input(struct vdec_s *vdec, struct vframe_chunk_s **p);

extern u32 vdec_offset_prepare_input(struct vdec_s *vdec, u32 consume_byte,
	u32 data_offset, u32 data_size);

/* clean decoder input */
extern void vdec_clean_input(struct vdec_s *vdec);

/* sync decoder input */
extern int vdec_sync_input(struct vdec_s *vdec);

/* enable decoder input */
extern void vdec_enable_input(struct vdec_s *vdec);

/* set decoder input prepare level */
extern void vdec_set_prepare_level(struct vdec_s *vdec, int level);

/* set vdec input */
extern int vdec_set_input_buffer(struct vdec_s *vdec, u32 start, u32 size);

/* check if decoder can get more input */
extern bool vdec_has_more_input(struct vdec_s *vdec);

/* allocate input chain
 * register vdec_device
 * create output, vfm or create ionvideo output
 * insert vdec to vdec_manager for scheduling
 */
extern int vdec_connect(struct vdec_s *vdec);

/* remove vdec from vdec_manager scheduling
 * release input chain
 * disconnect video output from ionvideo
 */
extern int vdec_disconnect(struct vdec_s *vdec);

/* release vdec structure */
extern int vdec_destroy(struct vdec_s *vdec);

/* reset vdec */
extern int vdec_reset(struct vdec_s *vdec);

extern int vdec_v4l2_reset(struct vdec_s *vdec, int flag);
extern void vdec_set_status(struct vdec_s *vdec, int status);

extern void vdec_set_next_status(struct vdec_s *vdec, int status);

extern int vdec_set_decinfo(struct vdec_s *vdec, struct dec_sysinfo *p);

extern int vdec_init(struct vdec_s *vdec, int is_4k, bool is_v4l);

extern void vdec_release(struct vdec_s *vdec);

extern int vdec_status(struct vdec_s *vdec, struct vdec_info_statistic_s *vstat);

extern int vdec_set_trickmode(struct vdec_s *vdec, unsigned long trickmode);

extern int vdec_set_isreset(struct vdec_s *vdec, int isreset);

extern int vdec_set_dv_metawithel(struct vdec_s *vdec, int isdvmetawithel);

extern void vdec_set_no_powerdown(int flag);

extern void vdec_set_flag(struct vdec_s *vdec, u32 flag);

extern void vdec_set_eos(struct vdec_s *vdec, bool eos);

extern void vdec_set_next_sched(struct vdec_s *vdec, struct vdec_s *next_vdec);

extern const char *vdec_status_str(struct vdec_s *vdec);

extern const char *vdec_type_str(struct vdec_s *vdec);

extern const char *vdec_device_name_str(struct vdec_s *vdec);

extern void vdec_schedule_work(struct work_struct *work);

extern void  vdec_count_info(struct vdec_info *vs, unsigned int err,
	unsigned int offset);

extern bool vdec_need_more_data(struct vdec_s *vdec);

extern void vdec_reset_core(struct vdec_s *vdec);

extern void hevc_reset_core(struct vdec_s *vdec);

extern void hevc_arb_ctrl_front_or_back(bool enable, bool front_flag);

extern void vdec_set_suspend_clk(int mode, int hevc);

extern unsigned long vdec_ready_to_run(struct vdec_s *vdec, unsigned long mask);

extern void vdec_prepare_run(struct vdec_s *vdec, unsigned long mask);

extern void vdec_core_request(struct vdec_s *vdec, unsigned long mask);

extern int vdec_core_release(struct vdec_s *vdec, unsigned long mask);

extern bool vdec_core_with_input(unsigned long mask);

extern void vdec_core_finish_run(struct vdec_s *vdec, unsigned long mask);

extern u32 vdec_get_debug(void);

#ifdef VDEC_DEBUG_SUPPORT
extern void vdec_set_step_mode(void);
#endif

extern void hevc_mmu_dma_check(struct vdec_s *vdec);

#ifdef NEW_FB_CODE
extern void fb_hw_status_clear(bool is_front);
#endif

int vdec_read_user_data(struct vdec_s *vdec,
				struct userdata_param_t *p_userdata_param);

int vdec_wakeup_userdata_poll(struct vdec_s *vdec);

void vdec_reset_userdata_fifo(struct vdec_s *vdec, int bInit);

struct vdec_s *vdec_get_vdec_by_video_id(int video_id);
struct vdec_s *vdec_get_vdec_by_id(int vdec_id);

#ifdef VDEC_DEBUG_SUPPORT
extern void vdec_set_step_mode(void);
ssize_t dump_vdec_debug(char *buf);
#endif
int vdec_get_debug_flags(void);
u32 vdec_get_debug(void);
void vdec_code_rate(struct vdec_s *vdec, uint32_t size);

void VDEC_PRINT_FUN_LINENO(const char *fun, int line);

unsigned char is_mult_inc(unsigned int);

int vdec_get_status(struct vdec_s *vdec);

void vdec_set_timestamp(struct vdec_s *vdec, u64 timestamp);
void vdec_set_metadata(struct vdec_s *vdec, ulong meta_ptr);

extern u32  vdec_get_frame_vdec(struct vdec_s *vdec,  struct vframe_counter_s *tmpbuf);

int vdec_get_frame_num(struct vdec_s *vdec);

int show_stream_buffer_status(char *buf,
	int (*callback) (struct stream_buf_s *, char *));

extern int get_double_write_ratio(int dw_mode);

bool is_support_interlace_avbc(void);

int vdec_resource_checking(struct vdec_s *vdec);

void set_meta_data_to_vf(struct vframe_s *vf, u32 type, void *v4l2_ctx);

void vdec_set_profile_level(struct vdec_s *vdec, u32 profile_idc, u32 level_idc);

extern void vdec_stream_skip_data(struct vdec_s *vdec, int skip_size);
void vdec_set_vld_wp(struct vdec_s *vdec, u32 wp);
void vdec_reset_vld_stbuf(struct vdec_s *vdec);
void vdec_config_vld_reg(struct vdec_s *vdec, u32 addr, u32 size);

extern u32 timestamp_avsync_counter_get(void);

void vdec_canvas_unlock(unsigned long flags);

unsigned long vdec_canvas_lock(void);

int vdec_get_core_nr(void);

bool vdec_has_single_mode(void);

int vdec_post_task(post_task_handler func, void *args);

void vdec_mm_dma_flush(ulong phys, u32 size);

void rdma_front_end_wrok(dma_addr_t ddr_phy_addr, u32 size);

void rdma_back_end_work(dma_addr_t back_ddr_phy_addr, u32 size);

int is_rdma_enable(void);

st_userdata *get_vdec_userdata_ctx(void);

void vdec_frame_rate_uevent(int dur);

void vdec_set_vf_dur(int dur);

int vdec_get_vf_dur(void);

void vdec_sync_irq(enum vdec_irq_num num);

void vdec_data_buffer_count_increase(ulong data, int index, int cb_index);

struct vdec_data_info_s *vdec_data_get(void);

int vdec_data_get_index(ulong data, struct vdec_data_buf_s *vdata_buf);

int vdec_init_stbuf_info(struct vdec_s *vdec);

void vdec_data_release(struct codec_mm_s *mm, struct codec_mm_cb_s *cb);

void register_frame_rate_uevent_func(vdec_frame_rate_event_func func);

unsigned long vdec_power_lock(struct vdec_s *vdec);

void vdec_power_unlock(struct vdec_s *vdec, unsigned long flags);

void vdec_up(struct vdec_s *vdec);

#define DEBUG_PORT
#ifdef DEBUG_PORT
typedef int (*dbg_info_up)(int, int, struct vframe_s *);
typedef int (*dbg_data_wr)(const void *, int, int, int);

extern dbg_data_wr debug_port_func_data_wr;
extern dbg_info_up debug_port_func_info_up;

void vdec_debug_port_register(dbg_data_wr data_write, dbg_info_up info_update);

void vdec_debug_port_unregister(void);

ssize_t dump_decoder_state(char *buf);

ssize_t dump_vdec_blocks(char *buf);

ssize_t dump_vdec_chunks(char *buf);

ssize_t dump_vdec_core(char *buf);

#endif

u64 vdec_get_stream_size(struct vdec_s *vdec);

void use_t5d_driver_set(bool value);

int vcodec_profile_register_v2(char *name, enum vformat_e vformat, int is_v4l);

int is_mmu_copy_enable(void);

void mmu_copy_work(struct mmu_copy_params params);

void vdec_set_mmu_copy_flag(bool need_copy);

struct firmware_s *fw_firmare_s_creat(int fw_size);

#endif				/* VDEC_H */
