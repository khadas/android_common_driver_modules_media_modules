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
//#define LOG_LINE()
//pr_err("[%s:%d]\n", __FUNCTION__, __LINE__);
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/compat.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/ctype.h>
#include <linux/amlogic/media/frame_sync/ptsserv.h>
#include <linux/amlogic/media/utils/amstream.h>
#include <linux/amlogic/media/canvas/canvas.h>
#include <linux/amlogic/media/canvas/canvas_mgr.h>
#include <linux/amlogic/media/utils/vdec_reg.h>
#include "../../../frame_provider/decoder/utils/vdec_canvas_utils.h"
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include "../../../common/chips/decoder_cpu_ver_info.h"
#include "../../../frame_provider/decoder/utils/amvdec.h"
#include "../../../stream_input/amports/amports_priv.h"
#include "../../../frame_provider/decoder/utils/firmware.h"
#include "../../../frame_provider/decoder/utils/vdec.h"
#include "../../../frame_provider/decoder/utils/vdec_power_ctrl.h"
#include "../common/encoder_report.h"

#include <linux/amlogic/media/codec_mm/codec_mm.h>
#include "jpegenc.h"
#include <linux/of_reserved_mem.h>

//#include <linux/amlogic/power_ctrl.h>
#include <dt-bindings/power/t7-pd.h>
#include <dt-bindings/power/sc2-pd.h>
#include <linux/amlogic/power_domain.h>

#include <linux/clk.h>
#include <linux/err.h>

#define HCODEC_MFDIN_REG17                       0x101f
#define HCODEC_MFDIN_REG18                       0x1020
#define HCODEC_MFDIN_REG19                       0x1021

#ifdef CONFIG_AM_ENCODER
#include "encoder.h"
#endif

#define JPEGENC_CANVAS_INDEX 0xE4
#define JPEGENC_CANVAS_MAX_INDEX 0xE7

#define ENC_CANVAS_OFFSET  0x64

#define LOG_ALL 0
#define LOG_INFO 1
#define LOG_DEBUG 2
#define LOG_ERROR 3

#define jenc_pr(level, x...) \
    do { \
        if (level >= jpegenc_print_level) \
            printk(x); \
    } while (0)

#define DRIVER_NAME "jpegenc"
#define CLASS_NAME "jpegenc"
#define DEVICE_NAME "jpegenc"

/* #define EXTERN_QUANT_TABLE */

/*######### DEBUG-BRINGUP#########*/
static u32 manual_clock;
static u32 manual_irq_num = 2;
static u32 manual_interrupt = 0;
/*################################*/

static s32 jpegenc_device_major;
static struct device *jpegenc_dev;
static u32 jpegenc_print_level = LOG_ERROR;

static s32 reg_offset;

static u32 use_dma_io = 1;

static u32 use_quality=1;
static u32 legacy_load=0;

static u32 dumpmem_line = 0;
static u32 pointer = 0;

static u32 clock_level = 1;
static u16 gQuantTable[2][DCTSIZE2];
#ifdef EXTERN_QUANT_TABLE
static u16 *gExternalQuantTablePtr;
static bool external_quant_table_available;
#endif

static u32 simulation_enable;
static u32 g_block_mode;
static u32 g_canv0_stride;
static u32 g_canv1_stride;
static u32 g_canv2_stride;
static u32 g_canvas_height;

static u32 jpeg_in_full_hcodec;
static u32 mfdin_ambus_canv_conv;
static u32 dump_input;
static unsigned int enc_canvas_offset;

#define MHz (1000000)

//static DEFINE_SPINLOCK(lock);

#define JPEGENC_BUFFER_LEVEL_VGA   0
#define JPEGENC_BUFFER_LEVEL_2M     1
#define JPEGENC_BUFFER_LEVEL_3M     2
#define JPEGENC_BUFFER_LEVEL_5M     3
#define JPEGENC_BUFFER_LEVEL_8M     4
#define JPEGENC_BUFFER_LEVEL_13M   5
#define JPEGENC_BUFFER_LEVEL_HD     6

const s8 *glevel_str[] = {
    "VGA",
    "2M",
    "3M",
    "5M",
    "8M",
    "13M",
    "HD",
};

const struct Jpegenc_BuffInfo_s jpegenc_buffspec[] = {
    {
        .lev_id = JPEGENC_BUFFER_LEVEL_VGA,
        .max_width = 640,
        .max_height = 640,
        .min_buffsize = 0x330000,
        .input = {
            .buf_start = 0,
            .buf_size = 0x12c000,
        },
        .assist = {
            .buf_start = 0x12d000,
            .buf_size = 0x2000,
        },
        .bitstream = {
            .buf_start = 0x130000,
            .buf_size = 0x200000,
        }
    }, {
        .lev_id = JPEGENC_BUFFER_LEVEL_2M,
        .max_width = 1600,
        .max_height = 1600,
        .min_buffsize = 0x960000,
        .input = {
            .buf_start = 0,
            .buf_size = 0x753000,
        },
        .assist = {
            .buf_start = 0x754000,
            .buf_size = 0x2000,
        },
        .bitstream = {
            .buf_start = 0x760000,
            .buf_size = 0x200000,
        }
    }, {
        .lev_id = JPEGENC_BUFFER_LEVEL_3M,
        .max_width = 2048,
        .max_height = 2048,
        .min_buffsize = 0xe10000,
        .input = {
            .buf_start = 0,
            .buf_size = 0xc00000,
        },
        .assist = {
            .buf_start = 0xc01000,
            .buf_size = 0x2000,
        },
        .bitstream = {
            .buf_start = 0xc10000,
            .buf_size = 0x200000,
        }
    }, {
        .lev_id = JPEGENC_BUFFER_LEVEL_5M,
        .max_width = 2624,
        .max_height = 2624,
        .min_buffsize = 0x1800000,
        .input = {
            .buf_start = 0,
            .buf_size = 0x13B3000,
        },
        .assist = {
            .buf_start = 0x13B4000,
            .buf_size = 0x2000,
        },
        .bitstream = {
            .buf_start = 0x1400000,
            .buf_size = 0x400000,
        }
    }, {
        .lev_id = JPEGENC_BUFFER_LEVEL_8M,
        .max_width = 3264,
        .max_height = 3264,
        .min_buffsize = 0x2300000,
        .input = {
            .buf_start = 0,
            .buf_size = 0x1e7b000,
        },
        .assist = {
            .buf_start = 0x1e7c000,
            .buf_size = 0x2000,
        },
        .bitstream = {
            .buf_start = 0x1f00000,
            .buf_size = 0x400000,
        }
    }, {
        .lev_id = JPEGENC_BUFFER_LEVEL_13M,
        .max_width = 3840,
        .max_height = 3840,
        .min_buffsize = 0x6270000,
        .input = {
            .buf_start = 0,
            .buf_size = 0x2a30000,
        },
        .assist = {
            .buf_start = 0x2a31000,
            .buf_size = 0x2000,
        },
        .bitstream = {
            .buf_start = 0x2a80000,
            .buf_size = 0x37f0000,
        }
    }, {
        .lev_id = JPEGENC_BUFFER_LEVEL_HD,
        .max_width = 8192,
        .max_height = 8192,
        .min_buffsize = 0xc400000,
        .input = {
            .buf_start = 0,
            .buf_size = 0xc000000,
        },
        .assist = {
            .buf_start = 0xc001000,
            .buf_size = 0x2000,
        },
        .bitstream = {
            .buf_start = 0xc010000,
            .buf_size = 0x3f0000,
        }
    }
};

const char *jpegenc_ucode[] = {
    "jpegenc_mc",
};

static struct jpegenc_manager_s gJpegenc;

static const u16 jpeg_quant[7][DCTSIZE2] = {
    { /* jpeg_quant[0][] : Luma, Canon */
        0x06, 0x06, 0x08, 0x0A, 0x0A, 0x10, 0x15, 0x19,
        0x06, 0x0A, 0x0A, 0x0E, 0x12, 0x1F, 0x29, 0x29,
        0x08, 0x0A, 0x0E, 0x12, 0x21, 0x29, 0x29, 0x29,
        0x0A, 0x0E, 0x12, 0x14, 0x23, 0x29, 0x29, 0x29,
        0x0A, 0x12, 0x21, 0x23, 0x27, 0x29, 0x29, 0x29,
        0x10, 0x1F, 0x29, 0x29, 0x29, 0x29, 0x29, 0x29,
        0x15, 0x29, 0x29, 0x29, 0x29, 0x29, 0x29, 0x29,
        0x19, 0x29, 0x29, 0x29, 0x29, 0x29, 0x29, 0x29
    },
    { /* jpeg_quant[1][] : Chroma, Canon */
        0x0A, 0x0E, 0x10, 0x14, 0x15, 0x1D, 0x2B, 0x35,
        0x0E, 0x12, 0x14, 0x1D, 0x25, 0x3E, 0x54, 0x54,
        0x10, 0x14, 0x19, 0x25, 0x40, 0x54, 0x54, 0x54,
        0x14, 0x1D, 0x25, 0x27, 0x48, 0x54, 0x54, 0x54,
        0x15, 0x25, 0x40, 0x48, 0x4E, 0x54, 0x54, 0x54,
        0x1D, 0x3E, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54,
        0x2B, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54,
        0x35, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54
    },
    { /* jpeg_quant[2][] : Luma, spec example Table K.1 */
        16, 11, 10, 16, 24, 40, 51, 61,
        12, 12, 14, 19, 26, 58, 60, 55,
        14, 13, 16, 24, 40, 57, 69, 56,
        14, 17, 22, 29, 51, 87, 80, 62,
        18, 22, 37, 56, 68, 109, 103, 77,
        24, 35, 55, 64, 81, 104, 113, 92,
        49, 64, 78, 87, 103, 121, 120, 101,
        72, 92, 95, 98, 112, 100, 103, 99
    },
    { /* jpeg_quant[3][] : Chroma, spec example Table K.2 */
        17, 18, 24, 47, 99, 99, 99, 99,
        18, 21, 26, 66, 99, 99, 99, 99,
        24, 26, 56, 99, 99, 99, 99, 99,
        47, 66, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99
    },
    { /* jpeg_quant[4][] : Luma, spec example Table K.1,
        modified to create long ZRL */
        16, 11, 10, 16, 24, 40, 51, 61,
        12, 12, 14, 19, 26, 58, 60, 55,
        14, 13, 16, 24, 40, 57, 69, 56,
        14, 17, 22, 29, 51, 87, 80, 62,
        18, 22, 37, 56, 68, 109, 103, 77,
        24, 35, 55, 64, 81, 104, 113, 92,
        49, 64, 78, 87, 103, 121, 120, 101,
        72, 92, 95, 98, 112, 100, 103, 16
    },
    { /* jpeg_quant[5][] : Chroma, spec example Table K.2,
        modified to create long ZRL */
        17, 18, 24, 47, 99, 99, 99, 99,
        18, 21, 26, 66, 99, 99, 99, 99,
        24, 26, 56, 99, 99, 99, 99, 99,
        47, 66, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 17
    },
    { /* jpeg_quant[6][] : no compression */
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1
    }
}; /* jpeg_quant */

static const u8 jpeg_huffman_dc[2][16 + 12] = {
    { /* jpeg_huffman_dc[0][] */
        0x00, /* number of code length=1 */
        0x01,
        0x05,
        0x01,
        0x01,
        0x01,
        0x01,
        0x01,
        0x01,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00, /* number of code length=16 */

        /* Entry index for code with
            minimum code length (=2 in this case) */
        0x00,
        0x01, 0x02, 0x03, 0x04, 0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0A,
        0x0B
    },
    { /* jpeg_huffman_dc[1][] */
        0x00, /* number of code length=1 */
        0x03,
        0x01,
        0x01,
        0x01,
        0x01,
        0x01,
        0x01,
        0x01,
        0x01,
        0x01,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00, /* number of code length=16 */

        /* Entry index for code with
            minimum code length (=2 in this case) */
        0x00, 0x01, 0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0x0A,
        0x0B
    }
}; /* jpeg_huffman_dc */

static const u8 jpeg_huffman_ac[2][16 + 162] = {
    { /* jpeg_huffman_ac[0][] */
        0x00, /* number of code length=1 */
        0x02,
        0x01,
        0x03,
        0x03,
        0x02,
        0x04,
        0x03,
        0x05,
        0x05,
        0x04,
        0x04,
        0x00,
        0x00,
        0x01,
        0x7D, /* number of code length=16 */

        /* Entry index for code with
            minimum code length (=2 in this case) */
        0x01, 0x02,
        0x03,
        0x00, 0x04, 0x11,
        0x05, 0x12, 0x21,
        0x31, 0x41,
        0x06, 0x13, 0x51, 0x61,
        0x07, 0x22, 0x71,
        0x14, 0x32, 0x81, 0x91, 0xA1,
        0x08, 0x23, 0x42, 0xB1, 0xC1,
        0x15, 0x52, 0xD1, 0xF0,
        0x24, 0x33, 0x62, 0x72,
        0x82,
        0x09, 0x0A, 0x16, 0x17, 0x18, 0x19,
        0x1A, 0x25, 0x26, 0x27, 0x28, 0x29,
        0x2A, 0x34, 0x35, 0x36,
        0x37, 0x38, 0x39, 0x3A, 0x43, 0x44,
        0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
        0x53, 0x54, 0x55, 0x56,
        0x57, 0x58, 0x59, 0x5A, 0x63, 0x64,
        0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
        0x73, 0x74, 0x75, 0x76,
        0x77, 0x78, 0x79, 0x7A, 0x83, 0x84,
        0x85, 0x86, 0x87, 0x88, 0x89, 0x8A,
        0x92, 0x93, 0x94, 0x95,
        0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2,
        0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
        0xA9, 0xAA, 0xB2, 0xB3,
        0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9,
        0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6,
        0xC7, 0xC8, 0xC9, 0xCA,
        0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
        0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3,
        0xE4, 0xE5, 0xE6, 0xE7,
        0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3,
        0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9,
        0xFA
    },
    { /* jpeg_huffman_ac[1][] */
        0x00, /* number of code length=1 */
        0x02,
        0x01,
        0x02,
        0x04,
        0x04,
        0x03,
        0x04,
        0x07,
        0x05,
        0x04,
        0x04,
        0x00,
        0x01,
        0x02,
        0x77, /* number of code length=16 */

        /* Entry index for code with
            minimum code length (=2 in this case) */
        0x00, 0x01,
        0x02,
        0x03, 0x11,
        0x04, 0x05, 0x21, 0x31,
        0x06, 0x12, 0x41, 0x51,
        0x07, 0x61, 0x71,
        0x13, 0x22, 0x32, 0x81,
        0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1,
        0x09, 0x23, 0x33, 0x52, 0xF0,
        0x15, 0x62, 0x72, 0xD1,
        0x0A, 0x16, 0x24, 0x34,

        0xE1,
        0x25, 0xF1,
        0x17, 0x18, 0x19, 0x1A, 0x26, 0x27,
        0x28, 0x29, 0x2A, 0x35, 0x36, 0x37,
        0x38, 0x39, 0x3A, 0x43,
        0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
        0x4A, 0x53, 0x54, 0x55, 0x56, 0x57,
        0x58, 0x59, 0x5A, 0x63,
        0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
        0x6A, 0x73, 0x74, 0x75, 0x76, 0x77,
        0x78, 0x79, 0x7A, 0x82,
        0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
        0x89, 0x8A, 0x92, 0x93, 0x94, 0x95,
        0x96, 0x97, 0x98, 0x99,
        0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
        0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3,
        0xB4, 0xB5, 0xB6, 0xB7,
        0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
        0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
        0xD2, 0xD3, 0xD4, 0xD5,
        0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2,
        0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
        0xE9, 0xEA, 0xF2, 0xF3,
        0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA
    }
}; /* jpeg_huffman_ac */

static u64 time_cnt = 0;

static spinlock_t s_dma_buf_lock = __SPIN_LOCK_UNLOCKED(s_dma_buf_lock);
static struct list_head s_dma_bufp_head = LIST_HEAD_INIT(s_dma_bufp_head);

static spinlock_t s_vpu_lock = __SPIN_LOCK_UNLOCKED(s_vpu_lock);
static DEFINE_SEMAPHORE(s_vpu_sem);
static struct list_head s_vbp_head = LIST_HEAD_INIT(s_vbp_head);

static s32 enc_dma_buf_release(struct file *filp);
static s32 enc_src_addr_config(struct encdrv_dma_buf_info_t *pinfo,
        struct file *filp);
static s32 enc_free_buffers(struct file *filp);
static int enc_dma_buf_get_phys(struct enc_dma_cfg *cfg, unsigned long *addr);

static void set_log_level(const char *module, int level)
{
    jpegenc_print_level = level;
}

static void dump_request(struct jpegenc_request_s *request) {
    jenc_pr(LOG_DEBUG, "jpegenc: dump request start\n");
    jenc_pr(LOG_DEBUG, "src=%u\n", request->src);
    jenc_pr(LOG_DEBUG, "encoder_width=%u\n", request->encoder_width);
    jenc_pr(LOG_DEBUG, "encoder_height=%u\n", request->encoder_height);
    jenc_pr(LOG_DEBUG, "framesize=%u\n", request->framesize);
    jenc_pr(LOG_DEBUG, "jpeg_quality=%u\n", request->jpeg_quality);
    jenc_pr(LOG_DEBUG, "QuantTable_id=%u\n", request->QuantTable_id);
    jenc_pr(LOG_DEBUG, "flush_flag=%u\n", request->flush_flag);
    jenc_pr(LOG_DEBUG, "block_mode=%u\n", request->block_mode);
    jenc_pr(LOG_DEBUG, "type=%d\n", request->type);
    jenc_pr(LOG_DEBUG, "input_fmt=%d\n", request->input_fmt);
    jenc_pr(LOG_DEBUG, "output_fmt=%d\n", request->output_fmt);

    jenc_pr(LOG_DEBUG, "y_off=%u\n", request->y_off);
    jenc_pr(LOG_DEBUG, "u_off=%u\n", request->u_off);
    jenc_pr(LOG_DEBUG, "v_off=%u\n", request->v_off);
    jenc_pr(LOG_DEBUG, "y_stride=%u\n", request->y_stride);
    jenc_pr(LOG_DEBUG, "u_stride=%u\n", request->u_stride);
    jenc_pr(LOG_DEBUG, "v_stride=%u\n", request->v_stride);
    jenc_pr(LOG_DEBUG, "h_stride=%u\n", request->h_stride);
    jenc_pr(LOG_DEBUG, "jpegenc: dump request end\n");
}

static void canvas_config_proxy(u32 index, ulong addr, u32 width, u32 height,
        u32 wrap, u32 blkmode) {
    unsigned long datah_temp, datal_temp;

    if (!is_support_vdec_canvas()) {
        canvas_config(index, addr, width, height, wrap, blkmode);
    } else {
#if 1
        ulong start_addr = addr >> 3;
        u32 cav_width = (((width + 31)>>5)<<2);
        u32 cav_height = height;
        u32 x_wrap_en = 0;
        u32 y_wrap_en = 0;
        u32 blk_mode = 0;//blkmode;
        u32 cav_endian = 0;

        datal_temp = (start_addr & 0x1fffffff) |
                    ((cav_width & 0x7 ) << 29 );

        datah_temp = ((cav_width  >> 3) & 0x1ff) |
                    ((cav_height & 0x1fff) <<9 ) |
                    ((x_wrap_en & 1) << 22 ) |
                    ((y_wrap_en & 1) << 23) |
                    ((blk_mode & 0x3) << 24) |
                    ( cav_endian << 26);

#else
        u32 endian = 0;
        u32 addr_bits_l = ((((addr + 7) >> 3) & CANVAS_ADDR_LMASK) << CAV_WADDR_LBIT);
        u32 width_l     = ((((width    + 7) >> 3) & CANVAS_WIDTH_LMASK) << CAV_WIDTH_LBIT);
        u32 width_h     = ((((width    + 7) >> 3) >> CANVAS_WIDTH_LWID) << CAV_WIDTH_HBIT);
        u32 height_h    = (height & CANVAS_HEIGHT_MASK) << CAV_HEIGHT_HBIT;
        u32 blkmod_h    = (blkmode & CANVAS_BLKMODE_MASK) << CAV_BLKMODE_HBIT;
        u32 switch_bits_ctl = (endian & 0xf) << CAV_ENDIAN_HBIT;
        u32 wrap_h      = (0 << 23);
        datal_temp = addr_bits_l | width_l;
        datah_temp = width_h | height_h | wrap_h | blkmod_h | switch_bits_ctl;
#endif
        /*
        if (core == VDEC_1) {
            WRITE_VREG(MDEC_CAV_CFG0, 0);   //[0]canv_mode, by default is non-canv-mode
            WRITE_VREG(MDEC_CAV_LUT_DATAL, datal_temp);
            WRITE_VREG(MDEC_CAV_LUT_DATAH, datah_temp);
            WRITE_VREG(MDEC_CAV_LUT_ADDR,  index);
        } else if (core == VDEC_HCODEC) */ {
            WRITE_HREG(HCODEC_MDEC_CAV_CFG0, 0);    //[0]canv_mode, by default is non-canv-mode
            WRITE_HREG(HCODEC_MDEC_CAV_LUT_DATAL, datal_temp);
            WRITE_HREG(HCODEC_MDEC_CAV_LUT_DATAH, datah_temp);
            WRITE_HREG(HCODEC_MDEC_CAV_LUT_ADDR,  index);
        }

        /*
        cav_lut_info_store(index, addr, width, height, wrap, blkmode, 0);

        if (vdec_get_debug() & 0x40000000) {
            jenc_pr(LOG_INFO, "(%s %2d) addr: %lx, width: %d, height: %d, blkm: %d, endian: %d\n",
                __func__, index, addr, width, height, blkmode, 0);
            jenc_pr(LOG_INFO, "data(h,l): 0x%8lx, 0x%8lx\n", datah_temp, datal_temp);
        }
        */
    }
}

static u64 jpegenc_time_count_start(void)
{
    //struct timeval    tv;

    //do_gettimeofday(&tv);
    //efi_gettimeofday(&tv);
    //return div64_u64(timeval_to_ns(&tv), 1000);
    return 0;
}

static void jpegenc_time_count_end(u64 *time)
{
    jenc_pr(LOG_INFO, "the encoder takes time %lld us.\n",
        jpegenc_time_count_start() - *time);
    *time = 0;
}

static int is_oversize(int w, int h, int max)
{
    if (w < 0 || h < 0)
        return true;

    if (h != 0 && (w > max / h))
        return true;

    return false;
}

struct jpeg_enc_clks {
    struct clk *dos_clk;
    struct clk *dos_apb_clk;
    struct clk *jpeg_enc_clk;

};

static struct jpeg_enc_clks g_jpeg_enc_clks;

static void jpeg_enc_clk_put(struct device *dev, struct jpeg_enc_clks *clks)
{
    if (!(clks->jpeg_enc_clk == NULL || IS_ERR(clks->jpeg_enc_clk)))
        devm_clk_put(dev, clks->jpeg_enc_clk);

    if (!(clks->dos_apb_clk == NULL || IS_ERR(clks->dos_apb_clk)))
        devm_clk_put(dev, clks->dos_apb_clk);

    if (!(clks->dos_clk == NULL || IS_ERR(clks->dos_clk)))
        devm_clk_put(dev, clks->dos_clk);
}

static int jpeg_enc_clk_get(struct device *dev, struct jpeg_enc_clks *clks)
{
    //int ret = 0;

    clks->dos_clk = devm_clk_get(dev, "clk_dos");
    if (IS_ERR(clks->dos_clk)) {
        jenc_pr(LOG_DEBUG, "cannot get clk_dos clock\n");
        clks->dos_clk = NULL;
        //ret = -ENOENT;
        //goto err;
    } else
        jenc_pr(LOG_INFO, "jpeg_enc_clk_get: get clk_dos OK\n");

    clks->dos_apb_clk = devm_clk_get(dev, "clk_apb_dos");
    if (IS_ERR(clks->dos_apb_clk)) {
        jenc_pr(LOG_DEBUG, "cannot get clk_apb_dos clock\n");
        clks->dos_apb_clk = NULL;
        //ret = -ENOENT;
        //goto err;
    } else
        jenc_pr(LOG_INFO, "jpeg_enc_clk_get: get clk_apb_dos OK\n");

    clks->jpeg_enc_clk = devm_clk_get(dev, "clk_jpeg_enc");
    if (IS_ERR(clks->jpeg_enc_clk)) {
        jenc_pr(LOG_DEBUG, "cannot get clk_jpeg_enc clock\n");
        clks->jpeg_enc_clk = NULL;
        //ret = -ENOENT;
        //goto err;
    } else
        jenc_pr(LOG_INFO, "jpeg_enc_clk_get: get clk_jpeg_enc OK\n");

    return 0;
//err:
//    jpeg_enc_clk_put(dev, clks);

//    return ret;
}

static void jpeg_enc_clk_enable(struct jpeg_enc_clks *clks, u32 frq)
{
    if (clks->dos_clk != NULL) {
        clk_set_rate(clks->dos_clk, 400 * MHz);
        clk_prepare_enable(clks->dos_clk);
        jenc_pr(LOG_INFO, "dos clk: %ld\n", clk_get_rate(clks->dos_clk));
    }

    if (clks->dos_apb_clk != NULL) {
        clk_set_rate(clks->dos_apb_clk, 400 * MHz);
        clk_prepare_enable(clks->dos_apb_clk);
        jenc_pr(LOG_INFO, "apb clk: %ld\n", clk_get_rate(clks->dos_apb_clk));
    }

    if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_C1) {
        if (clks->jpeg_enc_clk != NULL) {
            clk_set_rate(clks->jpeg_enc_clk, 400 * MHz);
            clk_prepare_enable(clks->jpeg_enc_clk);
            jenc_pr(LOG_INFO, "jpegenc clk: %ld\n", clk_get_rate(clks->jpeg_enc_clk));
        }
    }
    else {
        if (clks->jpeg_enc_clk != NULL) {
            clk_set_rate(clks->jpeg_enc_clk, 666666666);
            clk_prepare_enable(clks->jpeg_enc_clk);
            jenc_pr(LOG_INFO, "jpegenc clk: %ld\n", clk_get_rate(clks->jpeg_enc_clk));
        }
    }

    /*
    clk_prepare_enable(clks->dos_clk);
    clk_prepare_enable(clks->dos_apb_clk);
    clk_prepare_enable(clks->jpeg_enc_clk);
    */
    jenc_pr(LOG_INFO, "dos: %ld, dos_apb: %ld, jpeg clk: %ld\n",
        clk_get_rate(clks->dos_clk),
        clk_get_rate(clks->dos_apb_clk),
        clk_get_rate(clks->jpeg_enc_clk));

}

static void jpeg_enc_clk_disable(struct jpeg_enc_clks *clks)
{
    jenc_pr(LOG_INFO, "set jpeg_enc_clk rate to 0\n");
    clk_set_rate(clks->jpeg_enc_clk, 0);
    clk_disable_unprepare(clks->jpeg_enc_clk);

    //clk_disable_unprepare(clks->dos_apb_clk);
    //clk_disable_unprepare(clks->dos_clk);
}

static void dma_flush(u32 buf_start, u32 buf_size);

static s32 zigzag(s32 i)
{
    s32 zigzag_i;
    switch (i) {
    case 0:
        zigzag_i = 0;
        break;
    case 1:
        zigzag_i = 1;
        break;
    case 2:
        zigzag_i = 8;
        break;
    case 3:
        zigzag_i = 16;
        break;
    case 4:
        zigzag_i = 9;
        break;
    case 5:
        zigzag_i = 2;
        break;
    case 6:
        zigzag_i = 3;
        break;
    case 7:
        zigzag_i = 10;
        break;
    case 8:
        zigzag_i = 17;
        break;
    case 9:
        zigzag_i = 24;
        break;
    case 10:
        zigzag_i = 32;
        break;
    case 11:
        zigzag_i = 25;
        break;
    case 12:
        zigzag_i = 18;
        break;
    case 13:
        zigzag_i = 11;
        break;
    case 14:
        zigzag_i = 4;
        break;
    case 15:
        zigzag_i = 5;
        break;
    case 16:
        zigzag_i = 12;
        break;
    case 17:
        zigzag_i = 19;
        break;
    case 18:
        zigzag_i = 26;
        break;
    case 19:
        zigzag_i = 33;
        break;
    case 20:
        zigzag_i = 40;
        break;
    case 21:
        zigzag_i = 48;
        break;
    case 22:
        zigzag_i = 41;
        break;
    case 23:
        zigzag_i = 34;
        break;
    case 24:
        zigzag_i = 27;
        break;
    case 25:
        zigzag_i = 20;
        break;
    case 26:
        zigzag_i = 13;
        break;
    case 27:
        zigzag_i = 6;
        break;
    case 28:
        zigzag_i = 7;
        break;
    case 29:
        zigzag_i = 14;
        break;
    case 30:
        zigzag_i = 21;
        break;
    case 31:
        zigzag_i = 28;
        break;
    case 32:
        zigzag_i = 35;
        break;
    case 33:
        zigzag_i = 42;
        break;
    case 34:
        zigzag_i = 49;
        break;
    case 35:
        zigzag_i = 56;
        break;
    case 36:
        zigzag_i = 57;
        break;
    case 37:
        zigzag_i = 50;
        break;
    case 38:
        zigzag_i = 43;
        break;
    case 39:
        zigzag_i = 36;
        break;
    case 40:
        zigzag_i = 29;
        break;
    case 41:
        zigzag_i = 22;
        break;
    case 42:
        zigzag_i = 15;
        break;
    case 43:
        zigzag_i = 23;
        break;
    case 44:
        zigzag_i = 30;
        break;
    case 45:
        zigzag_i = 37;
        break;
    case 46:
        zigzag_i = 44;
        break;
    case 47:
        zigzag_i = 51;
        break;
    case 48:
        zigzag_i = 58;
        break;
    case 49:
        zigzag_i = 59;
        break;
    case 50:
        zigzag_i = 52;
        break;
    case 51:
        zigzag_i = 45;
        break;
    case 52:
        zigzag_i = 38;
        break;
    case 53:
        zigzag_i = 31;
        break;
    case 54:
        zigzag_i = 39;
        break;
    case 55:
        zigzag_i = 46;
        break;
    case 56:
        zigzag_i = 53;
        break;
    case 57:
        zigzag_i = 60;
        break;
    case 58:
        zigzag_i = 61;
        break;
    case 59:
        zigzag_i = 54;
        break;
    case 60:
        zigzag_i = 47;
        break;
    case 61:
        zigzag_i = 55;
        break;
    case 62:
        zigzag_i = 62;
        break;
    default:
        zigzag_i = 63;
        break;
    }
    return zigzag_i;
}

/* Perform convertion from Q to 1/Q */
u32 reciprocal(u32 q)
{
    u32 q_recip;

    /* 65535 * (1/q) */
    switch (q) {
    case 0:
        q_recip = 0;
        break;
    case 1:
        q_recip = 65535;
        break;
    case 2:
        q_recip = 32768;
        break;
    case 3:
        q_recip = 21845;
        break;
    case 4:
        q_recip = 16384;
        break;
    case 5:
        q_recip = 13107;
        break;
    case 6:
        q_recip = 10923;
        break;
    case 7:
        q_recip = 9362;
        break;
    case 8:
        q_recip = 8192;
        break;
    case 9:
        q_recip = 7282;
        break;
    case 10:
        q_recip = 6554;
        break;
    case 11:
        q_recip = 5958;
        break;
    case 12:
        q_recip = 5461;
        break;
    case 13:
        q_recip = 5041;
        break;
    case 14:
        q_recip = 4681;
        break;
    case 15:
        q_recip = 4369;
        break;
    case 16:
        q_recip = 4096;
        break;
    case 17:
        q_recip = 3855;
        break;
    case 18:
        q_recip = 3641;
        break;
    case 19:
        q_recip = 3449;
        break;
    case 20:
        q_recip = 3277;
        break;
    case 21:
        q_recip = 3121;
        break;
    case 22:
        q_recip = 2979;
        break;
    case 23:
        q_recip = 2849;
        break;
    case 24:
        q_recip = 2731;
        break;
    case 25:
        q_recip = 2621;
        break;
    case 26:
        q_recip = 2521;
        break;
    case 27:
        q_recip = 2427;
        break;
    case 28:
        q_recip = 2341;
        break;
    case 29:
        q_recip = 2260;
        break;
    case 30:
        q_recip = 2185;
        break;
    case 31:
        q_recip = 2114;
        break;
    case 32:
        q_recip = 2048;
        break;
    case 33:
        q_recip = 1986;
        break;
    case 34:
        q_recip = 1928;
        break;
    case 35:
        q_recip = 1872;
        break;
    case 36:
        q_recip = 1820;
        break;
    case 37:
        q_recip = 1771;
        break;
    case 38:
        q_recip = 1725;
        break;
    case 39:
        q_recip = 1680;
        break;
    case 40:
        q_recip = 1638;
        break;
    case 41:
        q_recip = 1598;
        break;
    case 42:
        q_recip = 1560;
        break;
    case 43:
        q_recip = 1524;
        break;
    case 44:
        q_recip = 1489;
        break;
    case 45:
        q_recip = 1456;
        break;
    case 46:
        q_recip = 1425;
        break;
    case 47:
        q_recip = 1394;
        break;
    case 48:
        q_recip = 1365;
        break;
    case 49:
        q_recip = 1337;
        break;
    case 50:
        q_recip = 1311;
        break;
    case 51:
        q_recip = 1285;
        break;
    case 52:
        q_recip = 1260;
        break;
    case 53:
        q_recip = 1237;
        break;
    case 54:
        q_recip = 1214;
        break;
    case 55:
        q_recip = 1192;
        break;
    case 56:
        q_recip = 1170;
        break;
    case 57:
        q_recip = 1150;
        break;
    case 58:
        q_recip = 1130;
        break;
    case 59:
        q_recip = 1111;
        break;
    case 60:
        q_recip = 1092;
        break;
    case 61:
        q_recip = 1074;
        break;
    case 62:
        q_recip = 1057;
        break;
    case 63:
        q_recip = 1040;
        break;
    case 64:
        q_recip = 1024;
        break;
    case 65:
        q_recip = 1008;
        break;
    case 66:
        q_recip = 993;
        break;
    case 67:
        q_recip = 978;
        break;
    case 68:
        q_recip = 964;
        break;
    case 69:
        q_recip = 950;
        break;
    case 70:
        q_recip = 936;
        break;
    case 71:
        q_recip = 923;
        break;
    case 72:
        q_recip = 910;
        break;
    case 73:
        q_recip = 898;
        break;
    case 74:
        q_recip = 886;
        break;
    case 75:
        q_recip = 874;
        break;
    case 76:
        q_recip = 862;
        break;
    case 77:
        q_recip = 851;
        break;
    case 78:
        q_recip = 840;
        break;
    case 79:
        q_recip = 830;
        break;
    case 80:
        q_recip = 819;
        break;
    case 81:
        q_recip = 809;
        break;
    case 82:
        q_recip = 799;
        break;
    case 83:
        q_recip = 790;
        break;
    case 84:
        q_recip = 780;
        break;
    case 85:
        q_recip = 771;
        break;
    case 86:
        q_recip = 762;
        break;
    case 87:
        q_recip = 753;
        break;
    case 88:
        q_recip = 745;
        break;
    case 89:
        q_recip = 736;
        break;
    case 90:
        q_recip = 728;
        break;
    case 91:
        q_recip = 720;
        break;
    case 92:
        q_recip = 712;
        break;
    case 93:
        q_recip = 705;
        break;
    case 94:
        q_recip = 697;
        break;
    case 95:
        q_recip = 690;
        break;
    case 96:
        q_recip = 683;
        break;
    case 97:
        q_recip = 676;
        break;
    case 98:
        q_recip = 669;
        break;
    case 99:
        q_recip = 662;
        break;
    case 100:
        q_recip = 655;
        break;
    case 101:
        q_recip = 649;
        break;
    case 102:
        q_recip = 643;
        break;
    case 103:
        q_recip = 636;
        break;
    case 104:
        q_recip = 630;
        break;
    case 105:
        q_recip = 624;
        break;
    case 106:
        q_recip = 618;
        break;
    case 107:
        q_recip = 612;
        break;
    case 108:
        q_recip = 607;
        break;
    case 109:
        q_recip = 601;
        break;
    case 110:
        q_recip = 596;
        break;
    case 111:
        q_recip = 590;
        break;
    case 112:
        q_recip = 585;
        break;
    case 113:
        q_recip = 580;
        break;
    case 114:
        q_recip = 575;
        break;
    case 115:
        q_recip = 570;
        break;
    case 116:
        q_recip = 565;
        break;
    case 117:
        q_recip = 560;
        break;
    case 118:
        q_recip = 555;
        break;
    case 119:
        q_recip = 551;
        break;
    case 120:
        q_recip = 546;
        break;
    case 121:
        q_recip = 542;
        break;
    case 122:
        q_recip = 537;
        break;
    case 123:
        q_recip = 533;
        break;
    case 124:
        q_recip = 529;
        break;
    case 125:
        q_recip = 524;
        break;
    case 126:
        q_recip = 520;
        break;
    case 127:
        q_recip = 516;
        break;
    case 128:
        q_recip = 512;
        break;
    case 129:
        q_recip = 508;
        break;
    case 130:
        q_recip = 504;
        break;
    case 131:
        q_recip = 500;
        break;
    case 132:
        q_recip = 496;
        break;
    case 133:
        q_recip = 493;
        break;
    case 134:
        q_recip = 489;
        break;
    case 135:
        q_recip = 485;
        break;
    case 136:
        q_recip = 482;
        break;
    case 137:
        q_recip = 478;
        break;
    case 138:
        q_recip = 475;
        break;
    case 139:
        q_recip = 471;
        break;
    case 140:
        q_recip = 468;
        break;
    case 141:
        q_recip = 465;
        break;
    case 142:
        q_recip = 462;
        break;
    case 143:
        q_recip = 458;
        break;
    case 144:
        q_recip = 455;
        break;
    case 145:
        q_recip = 452;
        break;
    case 146:
        q_recip = 449;
        break;
    case 147:
        q_recip = 446;
        break;
    case 148:
        q_recip = 443;
        break;
    case 149:
        q_recip = 440;
        break;
    case 150:
        q_recip = 437;
        break;
    case 151:
        q_recip = 434;
        break;
    case 152:
        q_recip = 431;
        break;
    case 153:
        q_recip = 428;
        break;
    case 154:
        q_recip = 426;
        break;
    case 155:
        q_recip = 423;
        break;
    case 156:
        q_recip = 420;
        break;
    case 157:
        q_recip = 417;
        break;
    case 158:
        q_recip = 415;
        break;
    case 159:
        q_recip = 412;
        break;
    case 160:
        q_recip = 410;
        break;
    case 161:
        q_recip = 407;
        break;
    case 162:
        q_recip = 405;
        break;
    case 163:
        q_recip = 402;
        break;
    case 164:
        q_recip = 400;
        break;
    case 165:
        q_recip = 397;
        break;
    case 166:
        q_recip = 395;
        break;
    case 167:
        q_recip = 392;
        break;
    case 168:
        q_recip = 390;
        break;
    case 169:
        q_recip = 388;
        break;
    case 170:
        q_recip = 386;
        break;
    case 171:
        q_recip = 383;
        break;
    case 172:
        q_recip = 381;
        break;
    case 173:
        q_recip = 379;
        break;
    case 174:
        q_recip = 377;
        break;
    case 175:
        q_recip = 374;
        break;
    case 176:
        q_recip = 372;
        break;
    case 177:
        q_recip = 370;
        break;
    case 178:
        q_recip = 368;
        break;
    case 179:
        q_recip = 366;
        break;
    case 180:
        q_recip = 364;
        break;
    case 181:
        q_recip = 362;
        break;
    case 182:
        q_recip = 360;
        break;
    case 183:
        q_recip = 358;
        break;
    case 184:
        q_recip = 356;
        break;
    case 185:
        q_recip = 354;
        break;
    case 186:
        q_recip = 352;
        break;
    case 187:
        q_recip = 350;
        break;
    case 188:
        q_recip = 349;
        break;
    case 189:
        q_recip = 347;
        break;
    case 190:
        q_recip = 345;
        break;
    case 191:
        q_recip = 343;
        break;
    case 192:
        q_recip = 341;
        break;
    case 193:
        q_recip = 340;
        break;
    case 194:
        q_recip = 338;
        break;
    case 195:
        q_recip = 336;
        break;
    case 196:
        q_recip = 334;
        break;
    case 197:
        q_recip = 333;
        break;
    case 198:
        q_recip = 331;
        break;
    case 199:
        q_recip = 329;
        break;
    case 200:
        q_recip = 328;
        break;
    case 201:
        q_recip = 326;
        break;
    case 202:
        q_recip = 324;
        break;
    case 203:
        q_recip = 323;
        break;
    case 204:
        q_recip = 321;
        break;
    case 205:
        q_recip = 320;
        break;
    case 206:
        q_recip = 318;
        break;
    case 207:
        q_recip = 317;
        break;
    case 208:
        q_recip = 315;
        break;
    case 209:
        q_recip = 314;
        break;
    case 210:
        q_recip = 312;
        break;
    case 211:
        q_recip = 311;
        break;
    case 212:
        q_recip = 309;
        break;
    case 213:
        q_recip = 308;
        break;
    case 214:
        q_recip = 306;
        break;
    case 215:
        q_recip = 305;
        break;
    case 216:
        q_recip = 303;
        break;
    case 217:
        q_recip = 302;
        break;
    case 218:
        q_recip = 301;
        break;
    case 219:
        q_recip = 299;
        break;
    case 220:
        q_recip = 298;
        break;
    case 221:
        q_recip = 297;
        break;
    case 222:
        q_recip = 295;
        break;
    case 223:
        q_recip = 294;
        break;
    case 224:
        q_recip = 293;
        break;
    case 225:
        q_recip = 291;
        break;
    case 226:
        q_recip = 290;
        break;
    case 227:
        q_recip = 289;
        break;
    case 228:
        q_recip = 287;
        break;
    case 229:
        q_recip = 286;
        break;
    case 230:
        q_recip = 285;
        break;
    case 231:
        q_recip = 284;
        break;
    case 232:
        q_recip = 282;
        break;
    case 233:
        q_recip = 281;
        break;
    case 234:
        q_recip = 280;
        break;
    case 235:
        q_recip = 279;
        break;
    case 236:
        q_recip = 278;
        break;
    case 237:
        q_recip = 277;
        break;
    case 238:
        q_recip = 275;
        break;
    case 239:
        q_recip = 274;
        break;
    case 240:
        q_recip = 273;
        break;
    case 241:
        q_recip = 272;
        break;
    case 242:
        q_recip = 271;
        break;
    case 243:
        q_recip = 270;
        break;
    case 244:
        q_recip = 269;
        break;
    case 245:
        q_recip = 267;
        break;
    case 246:
        q_recip = 266;
        break;
    case 247:
        q_recip = 265;
        break;
    case 248:
        q_recip = 264;
        break;
    case 249:
        q_recip = 263;
        break;
    case 250:
        q_recip = 262;
        break;
    case 251:
        q_recip = 261;
        break;
    case 252:
        q_recip = 260;
        break;
    case 253:
        q_recip = 259;
        break;
    case 254:
        q_recip = 258;
        break;
    default:
        q_recip = 257;
        break;
    }
    return q_recip;
} /* reciprocal */

static void push_word(u8 *base, s32 *offset, u32 word)
{
    u8 *ptr;
    s32 i;
    s32 bytes = (word >> 24) & 0xff;
    for (i = bytes - 1; i >= 0; i--) {
        ptr = base + *offset;
        (*offset)++;
        if (i == 0)
            *ptr = word & 0xff;
        else if (i == 1)
            *ptr = (word >> 8) & 0xff;
        else if (i == 2)
            *ptr = (word >> 16) & 0xff;
    }
}

static s32 jpeg_quality_scaling(s32 quality)
{
    if (quality <= 0)
        quality = 1;
    if (quality > 100)
        quality = 100;

    if (quality < 50)
        quality = 5000 / quality;
    else
        quality = 200 - quality * 2;
    return quality;
}

static void _convert_quant_table(u16 *qtable, u16 *basic_table,
    s32 scale_factor, bool force_baseline)
{
    s32 i = 0;
    s32 temp;
    for (i = 0; i < DCTSIZE2; i++) {
        if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1) {
            if (use_quality) {
                //jenc_pr(LOG_ALL, "c1 using quality factor %d\n", scale_factor);
                temp = ((s32)basic_table[i] * scale_factor + 50) / 100;
            } else {
                //jenc_pr(LOG_INFO, "c1 ignore quality factor\n");
                temp = (s32)basic_table[i];
            }
        }else
            temp = ((s32)basic_table[i] * scale_factor + 50) / 100;
        /* limit the values to the valid range */
        if (temp <= 0)
            temp = 1;
        /* max quantizer needed for 12 bits */
        if (temp > 32767)
            temp = 32767;
        /* limit to baseline range if requested */
        if (force_baseline && temp > 255)
            temp = 255;
        qtable[i] = (u16)temp;
    }
}

static void convert_quant_table(u16 *qtable, u16 *basic_table,
    s32 scale_factor)
{
    _convert_quant_table(qtable, basic_table, scale_factor, true);
}

static void write_jpeg_quant_lut(s32 table_num)
{
    s32 i;
    u32 data32;

    for (i = 0; i < DCTSIZE2; i += 2) {
        data32 = reciprocal(gQuantTable[table_num][i]);
        data32 |= reciprocal(gQuantTable[table_num][i + 1]) << 16;
        WRITE_HREG(HCODEC_QDCT_JPEG_QUANT_DATA, data32);
    }
}

static void write_jpeg_huffman_lut_dc(s32 table_num)
{
    u32 code_len, code_word, pos, addr;
    u32 num_code_len;
    u32 lut[12];
    u32 i, j;

    code_len = 1;
    code_word = 1;
    pos = 16;

    /* Construct DC Huffman table */
    for (i = 0; i < 16; i++) {
        num_code_len = jpeg_huffman_dc[table_num][i];
        for (j = 0; j < num_code_len; j++) {
            code_word = (code_word + 1) & ((1 << code_len) - 1);
            if (code_len < i + 1) {
                code_word <<= (i + 1 - code_len);
                code_len = i + 1;
            }
            addr = jpeg_huffman_dc[table_num][pos];
            lut[addr] = ((code_len - 1) << 16) | (code_word);
            pos++;
        }
    }

    /* Write DC Huffman table to HW */
    for (i = 0; i < 12; i++)
        WRITE_HREG(HCODEC_VLC_HUFFMAN_DATA, lut[i]);
}

static void write_jpeg_huffman_lut_ac(s32 table_num)
{
    u32 code_len, code_word, pos;
    u32 num_code_len;
    u32 run, size;
    u32 data, addr = 0;
    u32 *lut = NULL;
    u32 i, j;
    code_len = 1;
    code_word = 1;
    pos = 16;

    lut = (u32 *)vmalloc(162 * sizeof(u32));
    if (!lut) {
        jenc_pr(LOG_ERROR, "alloc lut failed.\n");
        return;
    }

    /* Construct AC Huffman table */
    for (i = 0; i < 16; i++) {
        num_code_len = jpeg_huffman_ac[table_num][i];
        for (j = 0; j < num_code_len; j++) {
            code_word = (code_word + 1) & ((1 << code_len) - 1);
            if (code_len < i + 1) {
                code_word <<= (i + 1 - code_len);
                code_len = i + 1;
            }
            run = jpeg_huffman_ac[table_num][pos] >> 4;
            size = jpeg_huffman_ac[table_num][pos] & 0xf;
            data = ((code_len - 1) << 16) | (code_word);
            if (size == 0) {
                if (run == 0)
                    addr = 0;     /* EOB */
                else if (run == 0xf)
                    addr = 161; /* ZRL */
                else
                    jenc_pr(LOG_ERROR,
                        "Error: Illegal AC Huffman table format!\n");
            } else if (size <= 0xa)
                addr = 1 + 16 * (size - 1) + run;
            else
                jenc_pr(LOG_ERROR,
                    "Error: Illegal AC Huffman table format!\n");
            lut[addr] = data;
            pos++;
        }
    }

    /* Write AC Huffman table to HW */
    for (i = 0; i < 162; i++)
        WRITE_HREG(HCODEC_VLC_HUFFMAN_DATA, lut[i]);

    vfree(lut);
}

static void prepare_jpeg_header(struct jpegenc_wq_s *wq)
{
    s32 pic_format;
    s32 pic_width, pic_height;
    s32 q_sel_comp0, q_sel_comp1, q_sel_comp2;
    s32 dc_huff_sel_comp0, dc_huff_sel_comp1, dc_huff_sel_comp2;
    s32 ac_huff_sel_comp0, ac_huff_sel_comp1, ac_huff_sel_comp2;
    s32 lastcoeff_sel;
    s32 jdct_intr_sel;
    s32 h_factor_comp0, v_factor_comp0;
    s32 h_factor_comp1, v_factor_comp1;
    s32 h_factor_comp2, v_factor_comp2;
    s32 q_num;
    s32 tq[2];
    s32 dc_huff_num, ac_huff_num;
    s32 dc_th[2], ac_th[2];
    u32 header_bytes = 0;
    u32 bak_header_bytes = 0;
    s32 i, j;
    u8 *assist_buf = (u8 *)wq->AssiststreamStartVirtAddr;

    if (wq->cmd.output_fmt >= JPEGENC_MAX_FRAME_FMT)
        jenc_pr(LOG_ERROR, "Input format is wrong!\n");
    switch (wq->cmd.output_fmt) {
    case JPEGENC_FMT_NV21:
    case JPEGENC_FMT_NV12:
    case JPEGENC_FMT_YUV420:
        pic_format = 3;
        break;
    case JPEGENC_FMT_YUV422_SINGLE:
        pic_format = 2;
        break;
    case JPEGENC_FMT_YUV444_SINGLE:
    case JPEGENC_FMT_YUV444_PLANE:
        pic_format = 1;
        break;
    default:
        pic_format = 0;
        break;
    }

    pic_width = wq->cmd.encoder_width;
    pic_height = wq->cmd.encoder_height;

    q_sel_comp0 = QUANT_SEL_COMP0;
    q_sel_comp1 = QUANT_SEL_COMP1;
    q_sel_comp2 = QUANT_SEL_COMP2;

    dc_huff_sel_comp0 = DC_HUFF_SEL_COMP0;
    dc_huff_sel_comp1 = DC_HUFF_SEL_COMP1;
    dc_huff_sel_comp2 = DC_HUFF_SEL_COMP2;
    ac_huff_sel_comp0 = AC_HUFF_SEL_COMP0;
    ac_huff_sel_comp1 = AC_HUFF_SEL_COMP1;
    ac_huff_sel_comp2 = AC_HUFF_SEL_COMP2;
    lastcoeff_sel = JDCT_LASTCOEFF_SEL;
    jdct_intr_sel = JDCT_INTR_SEL;

    if (pic_format == 2) {
        /* YUV422 */
        h_factor_comp0 = 1;
        v_factor_comp0 = 0;
        h_factor_comp1 = 0;
        v_factor_comp1 = 0;
        h_factor_comp2 = 0;
        v_factor_comp2 = 0;
    } else if (pic_format == 3) {
        /* YUV420 */
        h_factor_comp0 = 1;
        v_factor_comp0 = 1;
        h_factor_comp1 = 0;
        v_factor_comp1 = 0;
        h_factor_comp2 = 0;
        v_factor_comp2 = 0;
    } else {
        /* RGB or YUV */
        h_factor_comp0 = 0;
        v_factor_comp0 = 0;
        h_factor_comp1 = 0;
        v_factor_comp1 = 0;
        h_factor_comp2 = 0;
        v_factor_comp2 = 0;
    }

    /* SOI marke */
    push_word(assist_buf, &header_bytes,
        (2 << 24) | /* Number of bytes */
        (0xffd8 << 0)); /* data: SOI marker */

    /* Define quantization tables */
    q_num = 1;
#if 0
    if ((q_sel_comp0 != q_sel_comp1) ||
        (q_sel_comp0 != q_sel_comp2) ||
        (q_sel_comp1 != q_sel_comp2))
#endif
        q_num++;
#if 0
    tq[0] = q_sel_comp0;
    tq[1] = (q_sel_comp0 != q_sel_comp1) ? q_sel_comp1 :
        (q_sel_comp0 != q_sel_comp2) ? q_sel_comp2 :
        q_sel_comp0;
#endif
    tq[0] = 0;
    tq[1] = q_num - 1;

    /* data: DQT marker */
    push_word(assist_buf, &header_bytes,
        (2 << 24) | (0xffdb << 0));
    /* data: Lq */
    push_word(assist_buf, &header_bytes,
        (2 << 24) | ((2 + 65 * q_num) << 0));

    /* Add Quantization table bytes */
    /* header_bytes += (2 + (2 + 65 * q_num)); */
    for (i = 0; i < q_num; i++) {
        /* data: {Pq,Tq} */
        push_word(assist_buf, &header_bytes,
            (1 << 24) | (i << 0));
        for (j = 0; j < DCTSIZE2; j++) {
            /* data: Qk */
            push_word(assist_buf, &header_bytes,
                (1 << 24) |
                ((gQuantTable[tq[i]][zigzag(j)]) << 0));
        }
    }

    /* Define Huffman tables */
    dc_huff_num = 1;
    if ((dc_huff_sel_comp0 != dc_huff_sel_comp1) ||
        (dc_huff_sel_comp0 != dc_huff_sel_comp2) ||
        (dc_huff_sel_comp1 != dc_huff_sel_comp2))
        dc_huff_num++;

    ac_huff_num = 1;
    if ((ac_huff_sel_comp0 != ac_huff_sel_comp1) ||
        (ac_huff_sel_comp0 != ac_huff_sel_comp2) ||
        (ac_huff_sel_comp1 != ac_huff_sel_comp2))
        ac_huff_num++;

    dc_th[0] = dc_huff_sel_comp0;
    dc_th[1] = (dc_huff_sel_comp0 != dc_huff_sel_comp1) ?
        dc_huff_sel_comp1 : (dc_huff_sel_comp0 != dc_huff_sel_comp2) ?
        dc_huff_sel_comp2 : dc_huff_sel_comp0;

    ac_th[0] = ac_huff_sel_comp0;
    ac_th[1] = (ac_huff_sel_comp0 != ac_huff_sel_comp1) ?
        ac_huff_sel_comp1 : (ac_huff_sel_comp0 != ac_huff_sel_comp2) ?
        ac_huff_sel_comp2 : ac_huff_sel_comp0;

    /* data: DHT marker */
    push_word(assist_buf, &header_bytes,
        (2 << 24) | (0xffc4 << 0));
    /* data: Lh */
    push_word(assist_buf, &header_bytes,
        (2 << 24) |
        ((2 + (1 + 16 + 12) * dc_huff_num +
        (1 + 16 + 162) * ac_huff_num) << 0));

    /* Add Huffman table bytes */
    /* data: {Tc,Th} */
    for (i = 0; i < dc_huff_num; i++) {
        push_word(assist_buf, &header_bytes,
            (1 << 24) | (i << 0));
        for (j = 0; j < 16 + 12; j++) {
            /* data: Li then Vi,j */
            push_word(assist_buf, &header_bytes,
                (1 << 24) |
                ((jpeg_huffman_dc[dc_th[i]][j]) << 0));
        }
    }
    for (i = 0; i < ac_huff_num; i++) {
        push_word(assist_buf, &header_bytes,
            (1 << 24) |
            (1 << 4) | /* data: Tc */
            (i << 0)); /* data: Th */
        for (j = 0; j < 16 + 162; j++) {
            /* data: Li then Vi,j */
            push_word(assist_buf, &header_bytes,
                (1 << 24) |
                ((jpeg_huffman_ac[ac_th[i]][j]) << 0));
        }
    }

    /* Frame header */
    /* Add Frame header bytes */
    /* header_bytes += (2 + (8 + 3 * 3)); */
    push_word(assist_buf, &header_bytes,
        (2 << 24) | /* Number of bytes */
        (0xffc0 << 0)); /* data: SOF_0 marker */
    /* data: Lf */
    push_word(assist_buf, &header_bytes,
        (2 << 24) | ((8 + 3 * 3) << 0));
    /* data: P -- Sample precision */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (8 << 0));
    /* data: Y -- Number of lines */
    push_word(assist_buf,
        &header_bytes, (2 << 24) | (pic_height << 0));
    /* data: X -- Number of samples per line */
    push_word(assist_buf,
        &header_bytes, (2 << 24) | (pic_width << 0));
    /* data: Nf -- Number of components in a frame */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (3 << 0));
    /* data: C0 -- Comp0 identifier */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (0 << 0));
    push_word(assist_buf,
        &header_bytes, (1 << 24) |
        /* data: H0 -- Comp0 horizontal sampling factor */
        ((h_factor_comp0 + 1) << 4) |
        /* data: V0 -- Comp0 vertical sampling factor */
        ((v_factor_comp0 + 1) << 0));

    /* data: Tq0 -- Comp0 quantization table selector */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (0 << 0));
    /* data: C1 -- Comp1 identifier */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (1 << 0));
    push_word(assist_buf,
        &header_bytes, (1 << 24) |
        /* data: H1 -- Comp1 horizontal sampling factor */
        ((h_factor_comp1 + 1) << 4) |
        /* data: V1 -- Comp1 vertical sampling factor */
        ((v_factor_comp1 + 1) << 0));
    /* data: Tq1 -- Comp1 quantization table selector */
    push_word(assist_buf,
        &header_bytes, (1 << 24) |
        (((q_sel_comp0 != q_sel_comp1) ? 1 : 0) << 0));
    /* data: C2 -- Comp2 identifier */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (2 << 0));
    push_word(assist_buf,
        &header_bytes, (1 << 24) |
        /* data: H2 -- Comp2 horizontal sampling factor */
        ((h_factor_comp2 + 1) << 4) |
        /* data: V2 -- Comp2 vertical sampling factor */
        ((v_factor_comp2 + 1) << 0));
    /* data: Tq2 -- Comp2 quantization table selector */
    push_word(assist_buf,
        &header_bytes, (1 << 24) |
        (((q_sel_comp0 != q_sel_comp2) ? 1 : 0) << 0));

    /* Scan header */
    bak_header_bytes = header_bytes + (2 + (6 + 2 * 3));

    /* Add Scan header bytes */
    /* header_bytes += (2 + (6+2*3)); */
    /* If total header bytes is not multiple of 8,
         then fill 0xff byte between Frame header segment
         and the Scan header segment. */
    /* header_bytes = ((header_bytes + 7)/8)*8; */
    bak_header_bytes = ((bak_header_bytes + 7) / 8) * 8 - bak_header_bytes;
    for (i = 0; i < bak_header_bytes; i++)
        push_word(assist_buf,
            &header_bytes,
            (1 << 24) | (0xff << 0)); /* 0xff filler */

    push_word(assist_buf,
        &header_bytes,
        (2 << 24) | /* Number of bytes */
        (0xffda << 0)); /* data: SOS marker */

    /* data: Ls */
    push_word(assist_buf,
        &header_bytes, (2 << 24) | ((6 + 2 * 3) << 0));
    /* data: Ns -- Number of components in a scan */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (3 << 0));
    /* data: Cs0 -- Comp0 identifier */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (0 << 0));
    push_word(assist_buf,
        &header_bytes, (1 << 24) |
        (0 << 4) | /* data: Td0 -- Comp0 DC Huffman table selector */
        (0 << 0)); /* data: Ta0 -- Comp0 AC Huffman table selector */
    /* data: Cs1 -- Comp1 identifier */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (1 << 0));
    push_word(assist_buf,
        &header_bytes, (1 << 24) |
        /* data: Td1 -- Comp1 DC Huffman table selector */
        (((dc_huff_sel_comp0 != dc_huff_sel_comp1) ? 1 : 0) << 4) |
        /* data: Ta1 -- Comp1 AC Huffman table selector */
        (((ac_huff_sel_comp0 != ac_huff_sel_comp1) ? 1 : 0) << 0));
    /* data: Cs2 -- Comp2 identifier */
    push_word(assist_buf,
        &header_bytes, (1 << 24) | (2 << 0));
    push_word(assist_buf,
        &header_bytes, (1 << 24) |
        /* data: Td2 -- Comp2 DC Huffman table selector */
        (((dc_huff_sel_comp0 != dc_huff_sel_comp2) ? 1 : 0) << 4) |
        /* data: Ta2 -- Comp2 AC Huffman table selector */
        (((ac_huff_sel_comp0 != ac_huff_sel_comp2) ? 1 : 0) << 0));
    push_word(assist_buf, &header_bytes,
        (3 << 24) |
        (0 << 16) | /* data: Ss = 0 */
        (63 << 8) | /* data: Se = 63 */
        (0 << 4) | /* data: Ah = 0 */
        (0 << 0)); /* data: Al = 0 */
    jenc_pr(LOG_INFO, "jpeg header bytes is %d\n", header_bytes);
    wq->headbytes = header_bytes;
}

static void init_jpeg_encoder(struct jpegenc_wq_s *wq)
{
    u32 data32;
    s32 pic_format; /* 0=RGB; 1=YUV; 2=YUV422; 3=YUV420 */
    s32 pic_x_start, pic_x_end, pic_y_start, pic_y_end;
    s32 pic_width, pic_height;
    u32 q_sel_comp0, q_sel_comp1, q_sel_comp2;
    s32 dc_huff_sel_comp0, dc_huff_sel_comp1, dc_huff_sel_comp2;
    s32 ac_huff_sel_comp0, ac_huff_sel_comp1, ac_huff_sel_comp2;
    s32 lastcoeff_sel;
    s32 jdct_intr_sel;
    s32 h_factor_comp0, v_factor_comp0;
    s32 h_factor_comp1, v_factor_comp1;
    s32 h_factor_comp2, v_factor_comp2;

    jenc_pr(LOG_INFO, "Initialize JPEG Encoder ....\n");
    if (wq->cmd.output_fmt >= JPEGENC_MAX_FRAME_FMT)
        jenc_pr(LOG_ERROR, "Input format is wrong!\n");
    switch (wq->cmd.output_fmt) {
    case JPEGENC_FMT_NV21:
    case JPEGENC_FMT_NV12:
    case JPEGENC_FMT_YUV420:
        pic_format = 3;
        break;
    case JPEGENC_FMT_YUV422_SINGLE:
        pic_format = 2;
        break;
    case JPEGENC_FMT_YUV444_SINGLE:
    case JPEGENC_FMT_YUV444_PLANE:
        pic_format = 1;
        break;
    default:
        pic_format = 0;
        break;
    }

    pic_x_start = 0;
    pic_x_end = wq->cmd.encoder_width - 1;

    pic_y_start = 0;
    pic_y_end = wq->cmd.encoder_height - 1;

    pic_width = wq->cmd.encoder_width;
    pic_height = wq->cmd.encoder_height;

    if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
        && (get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_SC2)) {
        q_sel_comp0 = QUANT_SEL_COMP0 & 0xff;
        q_sel_comp1 = QUANT_SEL_COMP1 & 0xff;
        q_sel_comp2 = QUANT_SEL_COMP2 & 0xff;
    } else {
        q_sel_comp0 = wq->cmd.QuantTable_id * 2;
        q_sel_comp1 = q_sel_comp0 + 1;
        q_sel_comp2 = q_sel_comp1;
    }
    if (q_sel_comp0 >= 6 || q_sel_comp1 >= 6)
    {
        jenc_pr(LOG_ERROR, "error, q_sel_comp0, q_sel_comp1 is invalid %d,%d\n",
            q_sel_comp0, q_sel_comp1);
        return;
    }
    dc_huff_sel_comp0 = DC_HUFF_SEL_COMP0;
    dc_huff_sel_comp1 = DC_HUFF_SEL_COMP1;
    dc_huff_sel_comp2 = DC_HUFF_SEL_COMP2;
    ac_huff_sel_comp0 = AC_HUFF_SEL_COMP0;
    ac_huff_sel_comp1 = AC_HUFF_SEL_COMP1;
    ac_huff_sel_comp2 = AC_HUFF_SEL_COMP2;
    lastcoeff_sel = JDCT_LASTCOEFF_SEL;
    jdct_intr_sel = JDCT_INTR_SEL;

    if (pic_format == 2) {
        /* YUV422 */
        h_factor_comp0 = 1;
        v_factor_comp0 = 0;
        h_factor_comp1 = 0;
        v_factor_comp1 = 0;
        h_factor_comp2 = 0;
        v_factor_comp2 = 0;
    } else if (pic_format == 3) {
        /* YUV420 */
        h_factor_comp0 = 1;
        v_factor_comp0 = 1;
        h_factor_comp1 = 0;
        v_factor_comp1 = 0;
        h_factor_comp2 = 0;
        v_factor_comp2 = 0;
    } else {
        /* RGB or YUV */
        h_factor_comp0 = 0;
        v_factor_comp0 = 0;
        h_factor_comp1 = 0;
        v_factor_comp1 = 0;
        h_factor_comp2 = 0;
        v_factor_comp2 = 0;
    }

    /* Configure picture size and format */
    WRITE_HREG(HCODEC_VLC_PIC_SIZE, pic_width | (pic_height << 16));
    WRITE_HREG(HCODEC_VLC_PIC_POSITION, pic_format | (lastcoeff_sel << 4));
    WRITE_HREG(HCODEC_QDCT_JPEG_X_START_END,
           ((pic_x_end << 16) | (pic_x_start << 0)));
    WRITE_HREG(HCODEC_QDCT_JPEG_Y_START_END,
           ((pic_y_end << 16) | (pic_y_start << 0)));

    /* Configure quantization tables */
#ifdef EXTERN_QUANT_TABLE
    if (external_quant_table_available) {
        convert_quant_table(&gQuantTable[0][0],
            &gExternalQuantTablePtr[0],
            wq->cmd.jpeg_quality);
        convert_quant_table(&gQuantTable[1][0],
            &gExternalQuantTablePtr[DCTSIZE2],
            wq->cmd.jpeg_quality);
        q_sel_comp0 = 0;
        q_sel_comp1 = 1;
        q_sel_comp2 = 1;
    } else
#endif
    {
        s32 tq[2];
        tq[0] = q_sel_comp0;
        tq[1] = (q_sel_comp0 != q_sel_comp1) ?
            q_sel_comp1 : (q_sel_comp0 != q_sel_comp2) ?
            q_sel_comp2 : q_sel_comp0;
        convert_quant_table(&gQuantTable[0][0],
            (u16 *)&jpeg_quant[tq[0]],
            wq->cmd.jpeg_quality);
        if (tq[0] != tq[1])
            convert_quant_table(&gQuantTable[1][0],
                (u16 *)&jpeg_quant[tq[1]],
                wq->cmd.jpeg_quality);
        q_sel_comp0 = tq[0];
        q_sel_comp1 = tq[1];
        q_sel_comp2 = tq[1];
    }

    /* Set Quantization LUT start address */
    data32 = 0;
    data32 |= 0 << 8; /* [8] 0=Write LUT, 1=Read */
    data32 |= 0 << 0; /* [5:0] Start addr = 0 */

    WRITE_HREG(HCODEC_QDCT_JPEG_QUANT_ADDR, data32);

    /* Burst-write Quantization LUT data */
    if ((get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_C1)
        || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2)) {
        write_jpeg_quant_lut(0);
        if (q_sel_comp0 != q_sel_comp1)
            write_jpeg_quant_lut(1);
    } else {
        write_jpeg_quant_lut(q_sel_comp0);
        if (q_sel_comp1 != q_sel_comp0)
            write_jpeg_quant_lut(q_sel_comp1);
        if ((q_sel_comp2 != q_sel_comp0) && (q_sel_comp2 != q_sel_comp1))
            write_jpeg_quant_lut(q_sel_comp2);
    }

    /* Configure Huffman tables */

    /* Set DC Huffman LUT start address */
    data32 = 0;
    data32 |= 0 << 16; /* [16] 0=Write LUT, 1=Read */
    data32 |= 0 << 0; /* [8:0] Start addr = 0 */
    WRITE_HREG(HCODEC_VLC_HUFFMAN_ADDR, data32);

    /* Burst-write DC Huffman LUT data */
    write_jpeg_huffman_lut_dc(dc_huff_sel_comp0);
    if (dc_huff_sel_comp1 != dc_huff_sel_comp0)
        write_jpeg_huffman_lut_dc(dc_huff_sel_comp1);

    if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
        && (get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_SC2)) {
        if ((dc_huff_sel_comp2 != dc_huff_sel_comp0)
            && (dc_huff_sel_comp2 != dc_huff_sel_comp1))
            write_jpeg_huffman_lut_dc(dc_huff_sel_comp2);
    }

    /* Set AC Huffman LUT start address */
    data32 = 0;
    data32 |= 0 << 16; /* [16] 0=Write LUT, 1=Read */
    data32 |= 24 << 0; /* [8:0] Start addr = 0 */
    WRITE_HREG(HCODEC_VLC_HUFFMAN_ADDR, data32);

    /* Burst-write AC Huffman LUT data */
    write_jpeg_huffman_lut_ac(ac_huff_sel_comp0);
    if (ac_huff_sel_comp1 != ac_huff_sel_comp0)
        write_jpeg_huffman_lut_ac(ac_huff_sel_comp1);

    if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
        && (get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_SC2)) {
        if ((ac_huff_sel_comp2 != ac_huff_sel_comp0)
            && (ac_huff_sel_comp2 != ac_huff_sel_comp1))
            write_jpeg_huffman_lut_ac(ac_huff_sel_comp2);
    }

    /* Configure general control registers */
    data32 = 0;
    /* [19:18] dct_inflow_ctrl: 0=No halt; */
    /* 1=DCT halts request at end of each 8x8 block; */
    /* 2=DCT halts request at end of each MCU. */
    data32 |= 0 << 18;
    /* [17:16] jpeg_coeff_last_sel: */
    /* 0=Mark last coeff at the end of an 8x8 block, */
    /* 1=Mark last coeff at the end of an MCU */
    /* 2=Mark last coeff at the end of a scan */
    data32 |= lastcoeff_sel << 16;
    /* [15] jpeg_quant_sel_comp2 */
    data32 |= ((q_sel_comp2 == q_sel_comp0) ? 0 : 1) << 15;
    /* [14] jpeg_v_factor_comp2 */
    data32 |= v_factor_comp2 << 14;
    /* [13] jpeg_h_factor_comp2 */
    data32 |= h_factor_comp2 << 13;
    /* [12] jpeg_comp2_en */
    data32 |= 1 << 12;
    /* [11] jpeg_quant_sel_comp1 */
    data32 |= ((q_sel_comp1 == q_sel_comp0) ? 0 : 1) << 11;
    /* [10] jpeg_v_factor_comp1 */
    data32 |= v_factor_comp1 << 10;
    /* [9] jpeg_h_factor_comp1 */
    data32 |= h_factor_comp1 << 9;
    /* [8] jpeg_comp1_en */
    data32 |= 1 << 8;
    /* [7] jpeg_quant_sel_comp0 */
    data32 |= 0 << 7;
    /* [6] jpeg_v_factor_comp0 */
    data32 |= v_factor_comp0 << 6;
    /* [5] jpeg_h_factor_comp0 */
    data32 |= h_factor_comp0 << 5;
    /* [4] jpeg_comp0_en */
    data32 |= 1 << 4;
    /* [3:1] jdct_intr_sel:0=Disable intr; */
    /* 1=Intr at end of each 8x8 block of DCT input; */
    /* 2=Intr at end of each MCU of DCT input; */
    /* 3=Intr at end of a scan of DCT input; */
    /* 4=Intr at end of each 8x8 block of DCT output; */
    /* 5=Intr at end of each MCU of DCT output; */
    /* 6=Intr at end of a scan of DCT output. */
    data32 |= jdct_intr_sel << 1;
    /* [0] jpeg_en */
    data32 |= 1 << 0;
    WRITE_HREG(HCODEC_QDCT_JPEG_CTRL, data32);

    data32 = 0;
    data32 |= ((ac_huff_sel_comp2 == ac_huff_sel_comp0)? 0 : 1) << 29;  // [   29] jpeg_comp2_ac_table_sel
    data32 |= ((dc_huff_sel_comp2 == dc_huff_sel_comp0)? 0 : 1) << 28;  // [   28] jpeg_comp2_dc_table_sel
    /* [26:25] jpeg_comp2_cnt_max */
    data32 |= ((h_factor_comp2 + 1) * (v_factor_comp2 + 1) - 1) << 25;
    /* [24] jpeg_comp2_en */
    data32 |= 1 << 24;
    data32 |= ((ac_huff_sel_comp1 == ac_huff_sel_comp0)? 0 : 1) << 21;  // [   21] jpeg_comp1_ac_table_sel
    data32 |= ((dc_huff_sel_comp1 == dc_huff_sel_comp0)? 0 : 1) << 20;  // [   20] jpeg_comp1_dc_table_sel
    /* [18:17] jpeg_comp1_cnt_max */
    data32 |= ((h_factor_comp1 + 1) * (v_factor_comp1 + 1) - 1) << 17;
    /* [16] jpeg_comp1_en */
    data32 |= 1 << 16;
    /* [13] jpeg_comp0_ac_table_sel */
    data32 |= 0 << 13;
    /* [12] jpeg_comp0_dc_table_sel */
    data32 |= 0 << 12;
    /* [10:9] jpeg_comp0_cnt_max */
    data32 |= ((h_factor_comp0 + 1) * (v_factor_comp0 + 1) - 1) << 9;
    /* [8] jpeg_comp0_en */
    data32 |= 1 << 8;
    /* [0] jpeg_en, will be enbled by amrisc */
    data32 |= 0 << 0;
    WRITE_HREG(HCODEC_VLC_JPEG_CTRL, data32);

    WRITE_HREG(HCODEC_QDCT_MB_CONTROL,
        (1 << 9) | /* mb_info_soft_reset */
        (1 << 0)); /* mb read buffer soft reset */

    WRITE_HREG(HCODEC_QDCT_MB_CONTROL,
        (0 << 28) | /* ignore_t_p8x8 */
        (0 << 27) | /* zero_mc_out_null_non_skipped_mb */
        (0 << 26) | /* no_mc_out_null_non_skipped_mb */
        (0 << 25) | /* mc_out_even_skipped_mb */
        (0 << 24) | /* mc_out_wait_cbp_ready */
        (0 << 23) | /* mc_out_wait_mb_type_ready */
        (0 << 29) | /* ie_start_int_enable */
        (0 << 19) | /* i_pred_enable */
        (0 << 20) | /* ie_sub_enable */
        (0 << 18) | /* iq_enable */
        (0 << 17) | /* idct_enable */
        (0 << 14) | /* mb_pause_enable */
        (1 << 13) | /* q_enable */
        (1 << 12) | /* dct_enable */
        (0 << 10) | /* mb_info_en */
        (0 << 3) | /* endian */
        (0 << 1) | /* mb_read_en */
        (0 << 0)); /* soft reset */

    if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
        && (get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_SC2)) {
        // INIT_ENCODER
        WRITE_HREG(HCODEC_VLC_TOTAL_BYTES, 0);
        WRITE_HREG(HCODEC_VLC_INT_CONTROL, 0);// disable vlc interrupt

        WRITE_HREG(HCODEC_HENC_SCRATCH_0, 0);// mtspi   ENCODER_IDLE = ( ENCODER_STATUS
        WRITE_HREG(HCODEC_HENC_SCRATCH_1, 0xffffffff);// reset MCU_XY_REG

        WRITE_HREG(HCODEC_ASSIST_AMR1_INT0, 0x15);// vb_full_isr
        WRITE_HREG(HCODEC_ASSIST_AMR1_INT1, 8);// vlc_isr
        WRITE_HREG(HCODEC_ASSIST_AMR1_INT3, 0x14);// qdct_isr
    }

    /* Assember JPEG file header */
    prepare_jpeg_header(wq);
}

static void jpegenc_init_output_buffer(struct jpegenc_wq_s *wq)
{
    WRITE_HREG(HCODEC_VLC_VB_MEM_CTL,
        ((1 << 31) | (0x3f << 24) |
        (0x20 << 16) | (2 << 0)));

    WRITE_HREG(HCODEC_VLC_VB_START_PTR, wq->BitstreamStart);
    WRITE_HREG(HCODEC_VLC_VB_WR_PTR,    wq->BitstreamStart);
    WRITE_HREG(HCODEC_VLC_VB_SW_RD_PTR, wq->BitstreamStart);
    WRITE_HREG(HCODEC_VLC_VB_END_PTR,   wq->BitstreamEnd);
    WRITE_HREG(HCODEC_VLC_VB_CONTROL,   1);
    WRITE_HREG(HCODEC_VLC_VB_CONTROL,
        ((0 << 14) | (7 << 3) |
        (1 << 1) | (0 << 0)));
}

static s32 jpegenc_buffspec_init(struct jpegenc_wq_s *wq)
{
    s32 ret = 0;
    struct encdrv_buffer_pool_t *vbp;

    ret = down_interruptible(&s_vpu_sem);
    if (ret == 0) {
        vbp = kzalloc(sizeof(*vbp), GFP_KERNEL);
        if (!vbp) {
            up(&s_vpu_sem);
            return -ENOMEM;
        }

        /* input dct buffer config */
        wq->InputBuffStart = wq->buf_start + gJpegenc.mem.bufspec->input.buf_start;
        wq->InputBuffEnd = wq->InputBuffStart + gJpegenc.mem.bufspec->input.buf_size - 1;
        jenc_pr(LOG_INFO, "InputBuffStart is 0x%x\n", wq->InputBuffStart);

        /* assist stream buffer config */
        wq->AssistStart =  wq->buf_start + gJpegenc.mem.bufspec->assist.buf_start;
        wq->AssistEnd = wq->AssistStart + gJpegenc.mem.bufspec->assist.buf_size - 1;
        /* output stream buffer config */
        wq->BitstreamStart =  wq->buf_start + gJpegenc.mem.bufspec->bitstream.buf_start;
        wq->BitstreamEnd = wq->BitstreamStart + gJpegenc.mem.bufspec->bitstream.buf_size - 1;
        jenc_pr(LOG_INFO, "BitstreamStart is 0x%x\n", wq->BitstreamStart);

        wq->AssiststreamStartVirtAddr = phys_to_virt(wq->AssistStart);
        jenc_pr(LOG_INFO, "AssiststreamStartVirtAddr is %p\n", wq->AssiststreamStartVirtAddr);

        vbp->vb.phys_addr = wq->buf_start;
        vbp->vb.size = wq->buf_size;

        spin_lock(&s_vpu_lock);
        list_add(&vbp->list, &s_vbp_head);
        spin_unlock(&s_vpu_lock);

        up(&s_vpu_sem);
    }
    return 0;
}

/* for temp */
#define HCODEC_MFDIN_REGC_MBLP        (HCODEC_MFDIN_REGB_AMPC + 0x1)
#define HCODEC_MFDIN_REG0D            (HCODEC_MFDIN_REGB_AMPC + 0x2)
#define HCODEC_MFDIN_REG0E            (HCODEC_MFDIN_REGB_AMPC + 0x3)
#define HCODEC_MFDIN_REG0F            (HCODEC_MFDIN_REGB_AMPC + 0x4)
#define HCODEC_MFDIN_REG10            (HCODEC_MFDIN_REGB_AMPC + 0x5)
#define HCODEC_MFDIN_REG11            (HCODEC_MFDIN_REGB_AMPC + 0x6)
#define HCODEC_MFDIN_REG12            (HCODEC_MFDIN_REGB_AMPC + 0x7)
#define HCODEC_MFDIN_REG13            (HCODEC_MFDIN_REGB_AMPC + 0x8)
#define HCODEC_MFDIN_REG14            (HCODEC_MFDIN_REGB_AMPC + 0x9)
#define HCODEC_MFDIN_REG15            (HCODEC_MFDIN_REGB_AMPC + 0xa)
#define HCODEC_MFDIN_REG16            (HCODEC_MFDIN_REGB_AMPC + 0xb)

static void mfdin_basic_jpeg(
    u32 input, u8 iformat, u8 oformat, u32 picsize_x,
    u32 picsize_y, u8 r2y_en, u8 ifmt_extra,
    int mfdin_canvas0_stride,
    int mfdin_canvas1_stride,
    int mfdin_canvas2_stride,
    int mfdin_canvas0_blkmode,
    int mfdin_canvas1_blkmode,
    int mfdin_canvas2_blkmode,
    int mfdin_canvas0_addr,
    int mfdin_canvas1_addr,
    int mfdin_canvas2_addr,
    int mfdin_canvas_bias,
    bool mfdin_big_endian)
{
    u8 dsample_en; /* Downsample Enable */
    u8 interp_en; /* Interpolation Enable */
    u8 y_size; /* 0:16 Pixels for y direction pickup; 1:8 pixels */
    u8 r2y_mode; /* RGB2YUV Mode, range(0~3) */
    /* mfdin_reg3_canv[25:24]; */
    /* bytes per pixel in x direction for index0, 0:half 1:1 2:2 3:3 */
    u8 canv_idx0_bppx;
    /* mfdin_reg3_canv[27:26]; */
    /* bytes per pixel in x direction for index1-2, 0:half 1:1 2:2 3:3 */
    u8 canv_idx1_bppx;
    /* mfdin_reg3_canv[29:28]; */
    /* bytes per pixel in y direction for index0, 0:half 1:1 2:2 3:3 */
    u8 canv_idx0_bppy;
    /* mfdin_reg3_canv[31:30]; */
    /* bytes per pixel in y direction for index1-2, 0:half 1:1 2:2 3:3 */
    u8 canv_idx1_bppy;
    u8 ifmt444, ifmt422, ifmt420, linear_bytes4p;
    u32 linear_bytesperline;
    int mfdin_input_mode = 0;
    //s32 reg_offset;
    bool format_err = false;
    u32 data32;

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_TXL) {
        if ((iformat == 7) && (ifmt_extra > 2))
            format_err = true;
    } else if (iformat == 7)
        format_err = true;

    if (format_err) {
        jenc_pr(LOG_ERROR,
            "mfdin format err, iformat:%d, ifmt_extra:%d\n",
            iformat, ifmt_extra);
        return;
    }
    if (iformat != 7)
        ifmt_extra = 0;

    ifmt444 = ((iformat == 1) || (iformat == 5) || (iformat == 8)
        || (iformat == 9) || (iformat == 12)) ? 1 : 0;
    if (iformat == 7 && ifmt_extra == 1)
        ifmt444 = 1;
    ifmt422 = ((iformat == 0) || (iformat == 10)) ? 1 : 0;
    if (iformat == 7 && ifmt_extra != 1)
        ifmt422 = 1;
    ifmt420 = ((iformat == 2) || (iformat == 3) || (iformat == 4)
        || (iformat == 11)) ? 1 : 0;
    dsample_en = ((ifmt444 && (oformat != 2))
        || (ifmt422 && (oformat == 0))) ? 1 : 0;
    interp_en = ((ifmt422 && (oformat == 2))
             || (ifmt420 && (oformat != 0))) ? 1 : 0;
    y_size = (oformat != 0) ? 1 : 0;
    /* r2y_mode = (r2y_en == 1) ? 1 : 0; */
    r2y_mode = 1;
    canv_idx0_bppx = (iformat == 1) ? 3 : (iformat == 0) ? 2 : 1;
    canv_idx1_bppx = (iformat == 4) ? 0 : 1;
    canv_idx0_bppy = 1;
    canv_idx1_bppy = (iformat == 5) ? 1 : 0;

    if ((iformat == 8) || (iformat == 9) || (iformat == 12))
        linear_bytes4p = 3;
    else if (iformat == 10)
        linear_bytes4p = 2;
    else if (iformat == 11)
        linear_bytes4p = 1;
    else
        linear_bytes4p = 0;
    linear_bytesperline = picsize_x * linear_bytes4p;

    if (iformat < 8)
        mfdin_input_mode = 0;
    else
        mfdin_input_mode = 1;

    if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
        && (get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_SC2))
        mfdin_input_mode = 2;

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_GXBB) {
        reg_offset = -8;
        if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
            WRITE_HREG((HCODEC_MFDIN_REG8_DMBL + reg_offset),
                (picsize_x << 16) | (picsize_y << 0));
        else
            WRITE_HREG((HCODEC_MFDIN_REG8_DMBL + reg_offset),
                (picsize_x << 14) | (picsize_y << 0));
    } else {
        reg_offset = 0;
        WRITE_HREG((HCODEC_MFDIN_REG8_DMBL + reg_offset),
            (picsize_x << 12) | (picsize_y << 0));
    }

    WRITE_HREG((HCODEC_MFDIN_REG1_CTRL + reg_offset),
        (iformat << 0) |
        (oformat << 4) |
        (dsample_en << 6) |
        (y_size << 8) |
        (interp_en << 9) |
        (r2y_en << 12) |
        (r2y_mode << 13) |
        (ifmt_extra << 16) |
        (0 <<19) | // 0:NR Not Enabled
        (2 <<29) | // 0:H264_I_PIC_ALL_4x4, 1:H264_P_PIC_Y_16x16_C_8x8, 2:JPEG_ALL_8x8, 3:Reserved
        (0 <<31));  // 0:YC interleaved 1:YC non-interleaved(for JPEG)

    if (mfdin_input_mode == 0) {
        WRITE_HREG((HCODEC_MFDIN_REG3_CANV + reg_offset),
            (input & 0xffffff) |
            (canv_idx1_bppy << 30) |
            (canv_idx0_bppy << 28) |
            (canv_idx1_bppx << 26) |
            (canv_idx0_bppx << 24));
        WRITE_HREG((HCODEC_MFDIN_REG4_LNR0 + reg_offset),
            (0 << 16) | (0 << 0));
        WRITE_HREG((HCODEC_MFDIN_REG5_LNR1 + reg_offset), 0);
    } else if (mfdin_input_mode == 1) {
        WRITE_HREG((HCODEC_MFDIN_REG3_CANV + reg_offset),
            (canv_idx1_bppy << 30) |
            (canv_idx0_bppy << 28) |
            (canv_idx1_bppx << 26) |
            (canv_idx0_bppx << 24));
        WRITE_HREG((HCODEC_MFDIN_REG4_LNR0 + reg_offset),
            (linear_bytes4p << 16) | (linear_bytesperline << 0));
        WRITE_HREG((HCODEC_MFDIN_REG5_LNR1 + reg_offset), input);
    } else if (mfdin_input_mode == 2) {
        WRITE_HREG((HCODEC_MFDIN_REG3_CANV + reg_offset),
            (canv_idx1_bppy << 30) |
            (canv_idx0_bppy << 28) |
            (canv_idx1_bppx << 26) |
            (canv_idx0_bppx << 24));
        WRITE_HREG((HCODEC_MFDIN_REG4_LNR0 + reg_offset),
            mfdin_canvas0_stride << 0);
        WRITE_HREG((HCODEC_MFDIN_REG5_LNR1 + reg_offset), mfdin_canvas0_addr);

        WRITE_HREG(HCODEC_MFDIN_REG17, mfdin_canvas1_addr);        // U canvas initial address
        WRITE_HREG(HCODEC_MFDIN_REG18, mfdin_canvas2_addr);        // V canvas initial address
        WRITE_HREG(HCODEC_MFDIN_REG19, (mfdin_canvas1_stride << 16) |    // U canvas stride
                                   (mfdin_canvas2_stride << 0));

        data32 = READ_HREG(HCODEC_MFDIN_REG6_DCFG + reg_offset);
        data32 = data32 & 0x3ff;

        if (jpeg_in_full_hcodec) {
            jenc_pr(LOG_INFO, "JPEG_IN_FULL_HCODEC\n");
            data32 |= (0<<16);

            if (mfdin_ambus_canv_conv) {
                data32 |= (1<<17); // AMBUS
            }
        } else {
            data32 |= (1 << 16); // AXI Enable
        }

        data32 |= (mfdin_canvas0_blkmode << 14) |        // V canvas block mode
                  (mfdin_canvas1_blkmode << 12) |        // U canvas block mode
                  (mfdin_canvas2_blkmode << 10);         // Y canvas block mode

        WRITE_HREG(HCODEC_MFDIN_REG6_DCFG + reg_offset, data32);

        if (mfdin_canvas_bias)
            WRITE_HREG(HCODEC_MFDIN_REGA_CAV1 + reg_offset, mfdin_canvas_bias);
    }

    if (!mfdin_big_endian) {
        WRITE_HREG((HCODEC_MFDIN_REG9_ENDN + reg_offset),
            (7 << 0) | (6 << 3) | (5 << 6) |
            (4 << 9) | (3 << 12) | (2 << 15) |
            (1 << 18) | (0 << 21));
    }

    if (jpeg_in_full_hcodec) {//#ifdef JPEG_IN_FULL_HCODEC
        data32 = READ_HREG(HCODEC_MFDIN_REG3_CANV + reg_offset);
        WRITE_HREG(HCODEC_MFDIN_REG3_CANV + reg_offset, data32|(0x1 << 8)|(0x2 << 16));
    }

    data32 = READ_HREG(HCODEC_MFDIN_REG7_SCMD + reg_offset);
    WRITE_HREG(HCODEC_MFDIN_REG7_SCMD + reg_offset, data32 | (0x1 << 28)); // MFDIN Enabled

    jenc_pr(LOG_INFO, "MFDIN Enabled\n");

    return;
}

static struct file *file_open(const char *path, int flags, int rights)
{
    //return filp_open(path, flags, rights);
    return NULL;
}

static int file_close(struct file *file)
{
    //return filp_close(file, NULL);
    return 0;
}

static int file_write(struct file *file, unsigned long long offset, unsigned char *data, unsigned int size)
{
    //return kernel_write(file, data, size, &offset);
    return 0;
}

static s32 dump_raw_input(struct jpegenc_wq_s *wq, u32 y_addr, u32 u_addr, u32 v_addr) {
    u32 canvas_w, picsize_y;
    struct file *filp;

    canvas_w = ((wq->cmd.encoder_width + 31) >> 5) << 5;
    picsize_y = wq->cmd.encoder_height;
    jenc_pr(LOG_DEBUG, "canvas_w,picsize_y %d,%d\n", canvas_w,picsize_y);
    filp = file_open("/data/encoder.yuv", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (filp) {
        file_write(filp, 0, (u8*)phys_to_virt(y_addr), canvas_w * picsize_y);
        file_write(filp, canvas_w * picsize_y, (u8*)phys_to_virt(u_addr), canvas_w * picsize_y / 2);
        file_close(filp);
    } else{
        jenc_pr(LOG_ERROR, "dump file open fail\n");
    }

    return 0;
}

//#define CONFIG_AMLOGIC_MEDIA_CANVAS

static s32 set_jpeg_input_format(struct jpegenc_wq_s *wq,
    struct jpegenc_request_s *cmd)
{
    s32 ret = 0;
    u8 iformat = JPEGENC_MAX_FRAME_FMT;
    u8 oformat = JPEGENC_MAX_FRAME_FMT;
    u8 r2y_en = 0;
    u32 picsize_x = 0, picsize_y = 0;
    u32 input = cmd->src;
    u8 ifmt_extra = 0;
    int mfdin_canvas0_stride = 0, mfdin_canvas1_stride = 0, mfdin_canvas2_stride = 0;
    int mfdin_canvas0_blkmode = 0, mfdin_canvas1_blkmode = 0, mfdin_canvas2_blkmode = 0;
    int mfdin_canvas0_addr = 0, mfdin_canvas1_addr = 0, mfdin_canvas2_addr = 0;
    int mfdin_canvas_height = 0;
    int mfdin_canvas_bias = 0;
    bool mfdin_big_endian = false;
    u32 block_mode = 0;

    struct encdrv_buffer_pool_t *pool, *n;
    struct encdrv_buffer_t vb, buf;
    bool find = false;
    u32 cached = 0;

    u32 input_y = 0;
    u32 input_u = 0;
    u32 input_v = 0;

#ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
    u32 canvas_w = 0;
#endif

    jenc_pr(LOG_INFO, "************begin set input format**************\n");
    jenc_pr(LOG_INFO, "type is %d\n", cmd->type);
    jenc_pr(LOG_INFO, "input_fmt is %d\n", cmd->input_fmt);
    jenc_pr(LOG_INFO, "output_fmt is %d\n", cmd->output_fmt);
    jenc_pr(LOG_INFO, "input is 0x%x\n", cmd->src);
    jenc_pr(LOG_INFO, "size is %d\n", cmd->framesize);
    jenc_pr(LOG_INFO, "quality is %d\n", cmd->jpeg_quality);
    jenc_pr(LOG_INFO, "quant tbl_id is %d\n", cmd->QuantTable_id);
    jenc_pr(LOG_INFO, "flush flag is %d\n", cmd->flush_flag);
    jenc_pr(LOG_INFO, "block mode is %d\n", cmd->block_mode);
    jenc_pr(LOG_INFO, "************end set input format**************\n");

    if ((cmd->type == JPEGENC_LOCAL_BUFF) ||
        (cmd->type == JPEGENC_DMA_BUFF) ||
        (cmd->type == JPEGENC_PHYSICAL_BUFF)) {

        if (cmd->type == JPEGENC_LOCAL_BUFF) {
            if (cmd->flush_flag & JPEGENC_FLUSH_FLAG_INPUT) {
            buf.phys_addr = wq->InputBuffStart;
            buf.size = cmd->framesize;

            spin_lock(&s_vpu_lock);
            list_for_each_entry_safe(pool, n,
                &s_vbp_head, list) {
                //if (pool->filp == filp) {
                    vb = pool->vb;
                    if ((vb.phys_addr <= buf.phys_addr)
                        && ((vb.phys_addr + vb.size)
                            > buf.phys_addr)
                        && ((vb.phys_addr + vb.size)
                            >= buf.phys_addr + buf.size)
                        && find == false){
                        cached = vb.cached;
                        find = true;
                        break;
                    }
                //}
            }
            spin_unlock(&s_vpu_lock);
            //if (find && cached)
            if (find)
                dma_flush(
                    (u32)buf.phys_addr,
                    (u32)buf.size);
            }
        }

        picsize_x = ((cmd->encoder_width + 15) >> 4) << 4;
        picsize_y = cmd->encoder_height;

        if (cmd->type == JPEGENC_LOCAL_BUFF || cmd->type == JPEGENC_DMA_BUFF) {
            if (cmd->type == JPEGENC_LOCAL_BUFF) {
                input = wq->InputBuffStart;
            } else if (cmd->type == JPEGENC_DMA_BUFF) {
                if (cmd->plane_num == 3) {
                    input_y = (unsigned long)cmd->dma_cfg[0].paddr;
                    input_u = (unsigned long)cmd->dma_cfg[1].paddr;
                    input_v = (unsigned long)cmd->dma_cfg[2].paddr;
                } else if (cmd->plane_num == 2) {
                    input_y = (unsigned long)cmd->dma_cfg[0].paddr;
                    input_u = (unsigned long)cmd->dma_cfg[1].paddr;
                    input_v = input_u;
                } else if (cmd->plane_num == 1) {
                    input = (unsigned long)cmd->dma_cfg[0].paddr;
                    input_y = (unsigned long)cmd->dma_cfg[0].paddr;
                    if (cmd->input_fmt == JPEGENC_FMT_NV21
                        || cmd->input_fmt == JPEGENC_FMT_NV12) {
                        input_u = input_y + picsize_x * picsize_y;
                        input_v = input_u;
                    }
                    if (cmd->input_fmt == JPEGENC_FMT_YUV420) {
                        input_u = input_y + picsize_x * picsize_y;
                        input_v = input_u + picsize_x * picsize_y  / 4;
                    }
                }
                jenc_pr(LOG_INFO, "cmd->plane_num[%d]\n", cmd->plane_num);
                jenc_pr(LOG_INFO, "cmd->input_fmt[%d]\n", cmd->input_fmt);
                jenc_pr(LOG_INFO, "dma addr[0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx]\n",
                    (unsigned long)cmd->dma_cfg[0].vaddr,
                    (unsigned long)cmd->dma_cfg[0].paddr,
                    (unsigned long)cmd->dma_cfg[1].vaddr,
                    (unsigned long)cmd->dma_cfg[1].paddr,
                    (unsigned long)cmd->dma_cfg[2].vaddr,
                    (unsigned long)cmd->dma_cfg[2].paddr);
            }
        }

        if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
            && (get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_SC2)) {
            /*
             * mfdin in  fmt:  0:422 single, 1:444 single, 2:NV21, 3:NV12, 4:420 plane, 5:444 plane
             * mfdin out fmt:  0:420, 1:422, 2:444
             * picture   fmt:  0:RGB, 1:444, 2:422, 3:420
             * (out, pic)   :  (0,3), (1,2), (2,1)
             */
            if (cmd->input_fmt     == JPEGENC_FMT_YUV422_SINGLE)
                iformat = 0;

            else if (cmd->input_fmt == JPEGENC_FMT_YUV444_SINGLE)
                iformat = 1;

            else if (cmd->input_fmt == JPEGENC_FMT_NV21)
                iformat = 2;

            else if (cmd->input_fmt == JPEGENC_FMT_NV12)
                iformat = 3;

            else if (cmd->input_fmt == JPEGENC_FMT_YUV420)
                iformat = 4;

            else if (cmd->input_fmt == JPEGENC_FMT_YUV444_PLANE)
                iformat = 5;
            else if (cmd->input_fmt == JPEGENC_FMT_RGB888) {
                iformat = 1;
                r2y_en = 1;
            }

            if (cmd->output_fmt     == JPEGENC_FMT_YUV420)
                oformat = 0;
            else if (cmd->output_fmt == JPEGENC_FMT_YUV422_SINGLE)
                oformat = 1;
            else if (cmd->output_fmt == JPEGENC_FMT_YUV444_SINGLE)
                oformat = 2;

            block_mode = cmd->block_mode;
            mfdin_canvas0_stride = cmd->y_stride;
            mfdin_canvas1_stride = cmd->u_stride;
            mfdin_canvas2_stride = cmd->v_stride;
            mfdin_canvas_height = cmd->h_stride;

            if (simulation_enable) {
                if (g_block_mode)
                    block_mode = g_block_mode;
                if (g_canv0_stride)
                    mfdin_canvas0_stride = g_canv0_stride;
                if (g_canv1_stride)
                    mfdin_canvas1_stride = g_canv1_stride;
                if (g_canv2_stride)
                    mfdin_canvas2_stride = g_canv2_stride;
                if (g_canvas_height)
                    mfdin_canvas_height = g_canvas_height;
            }

            if (block_mode) {
                mfdin_canvas0_blkmode = 1;
                mfdin_canvas1_blkmode = 1;
                mfdin_canvas2_blkmode = 1;
            } else {
                mfdin_canvas0_blkmode = 0;
                mfdin_canvas1_blkmode = 0;
                mfdin_canvas2_blkmode = 0;
            }

            if ((iformat == 0 && oformat == 0) ||    /*case1013, 422 single -> 420*/
                (iformat == 0 && oformat == 1) ||    /*case1002, 422 single -> 422*/
                (iformat == 0 && oformat == 2) ||    /*case1004, 422 single -> 444*/
                (iformat == 1 && oformat == 1) ||    /*case1003, 444 single -> 444*/
                (iformat == 1 && oformat == 0) ||
                (iformat == 1 && oformat == 2)) {    /*case1005, 444 single -> 444*/
                mfdin_canvas0_addr = input;
                mfdin_canvas1_addr = input;
                mfdin_canvas2_addr = input;

                if (iformat == 0) {
                    mfdin_canvas0_stride = cmd->y_stride * 2;
                    mfdin_canvas1_stride = cmd->u_stride * 2;
                    mfdin_canvas2_stride = cmd->v_stride * 3;
                } else if (iformat == 1) {
                    mfdin_canvas0_stride = cmd->y_stride * 3;
                    mfdin_canvas1_stride = cmd->u_stride * 3;
                    mfdin_canvas2_stride = cmd->v_stride * 3;
                }
            } else if ((iformat == 2 && oformat == 0) ||
                (iformat == 3 && oformat == 0) ||    /*case1001, NV21 -> 420*/
                (iformat == 3 && oformat == 1) ||    /*case1000, NV21 -> 422*/
                (iformat == 3 && oformat == 2) ||    /*case1006, NV21 -> 444*/
                (iformat == 2 && oformat == 2) ||
                (iformat == 2 && oformat == 1)) {    /*case1006, NV12 -> 444, linear*/
                if (cmd->type == JPEGENC_DMA_BUFF) {
                    mfdin_canvas0_addr = input_y;
                    mfdin_canvas1_addr = input_u;
                    mfdin_canvas2_addr = input_v;
                } else {
                    mfdin_canvas0_addr = input;
                    mfdin_canvas1_addr = input + mfdin_canvas0_stride * mfdin_canvas_height;
                    mfdin_canvas2_addr = mfdin_canvas1_addr;
                }
            } else if ((iformat == 4 && oformat == 0) ||    /*case1010, 420 plane -> 420*/
                (iformat == 4 && oformat == 2) ||
                (iformat == 4 && oformat == 1)) {            /*case1008, case1011, case1012, 420 plane -> 444*/
                if (!simulation_enable) {
                    mfdin_canvas1_stride = mfdin_canvas0_stride / 2;
                    mfdin_canvas2_stride = mfdin_canvas0_stride / 2;
                }
                if (cmd->type == JPEGENC_DMA_BUFF) {
                    mfdin_canvas0_addr = input_y;
                    mfdin_canvas1_addr = input_u;
                    mfdin_canvas2_addr = input_v;
                } else {
                    mfdin_canvas0_addr = input;
                    mfdin_canvas1_addr = input + mfdin_canvas0_stride * mfdin_canvas_height;

                    mfdin_canvas2_addr = input + mfdin_canvas0_stride * mfdin_canvas_height +
                            mfdin_canvas0_stride * mfdin_canvas_height / 4;
                }
                jenc_pr(LOG_INFO, "%x:%x:%x, mfdin_canvas0_stride=%d, mfdin_canvas1_stride=%d, mfdin_canvas2_stride=%d, mfdin_canvas_height=%d\n",
                    mfdin_canvas0_addr, mfdin_canvas1_addr, mfdin_canvas2_addr,
                    mfdin_canvas0_stride,
                    mfdin_canvas1_stride,
                    mfdin_canvas2_stride,
                    mfdin_canvas_height);

                jenc_pr(LOG_INFO, "process yuv420p input, uoff=%d, voff=%d\n",
                        mfdin_canvas0_stride * mfdin_canvas_height,
                        mfdin_canvas0_stride * mfdin_canvas_height +
                        mfdin_canvas0_stride * mfdin_canvas_height / 4);
            } else if (iformat == 4 && oformat == 1) {    /*case1009, 420 plane -> 422*/
                if (!simulation_enable) {
                    mfdin_canvas1_stride = mfdin_canvas0_stride / 2;
                    mfdin_canvas2_stride = mfdin_canvas0_stride / 2;
                }
                if (cmd->type == JPEGENC_DMA_BUFF) {
                    mfdin_canvas0_addr = input_y;
                    mfdin_canvas1_addr = input_u;
                    mfdin_canvas2_addr = input_v;
                } else {
                    mfdin_canvas0_addr = input;
                    mfdin_canvas1_addr = input + mfdin_canvas0_stride * mfdin_canvas_height;
                    mfdin_canvas2_addr = input + mfdin_canvas0_stride * mfdin_canvas_height +
                            mfdin_canvas0_stride * mfdin_canvas_height / 4;
                }
                //mfdin_canvas_bias = mfdin_canvas1_stride << 16;
            } else if (iformat == 5 /*&& oformat == 0*/) {    /*case1007, 444 plane -> 420*/
                mfdin_canvas1_stride = mfdin_canvas0_stride;
                mfdin_canvas2_stride = mfdin_canvas0_stride;
                mfdin_canvas0_addr = input;
                mfdin_canvas1_addr = input              + mfdin_canvas0_stride * mfdin_canvas_height;
                mfdin_canvas2_addr = mfdin_canvas1_addr + mfdin_canvas1_stride * mfdin_canvas_height;
                //mfdin_big_endian = true;
            } else {
                jenc_pr(LOG_ERROR, "config input or output format err!\n");
                return -1;
            }

            if ((get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T3) || \
                (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T5M) || \
                (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T3X) || \
                (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_S7D)) {
                if ((cmd->input_fmt == JPEGENC_FMT_RGB565)
                    || (cmd->input_fmt >= JPEGENC_MAX_FRAME_FMT))
                    return -1;

                if (cmd->output_fmt     == JPEGENC_FMT_YUV420) {
                    oformat = 0;
                } else if (cmd->output_fmt == JPEGENC_FMT_YUV422_SINGLE) {
                    oformat = 1;
                } else if (cmd->output_fmt == JPEGENC_FMT_YUV444_SINGLE) {
                    oformat = 2;
                }

                if ((cmd->input_fmt <= JPEGENC_FMT_YUV444_PLANE) ||
                    (cmd->input_fmt >= JPEGENC_FMT_YUV422_12BIT))
                    r2y_en = 0;
                else
                    r2y_en = 1;

                if (cmd->input_fmt >= JPEGENC_FMT_YUV422_12BIT) {
                    iformat = 7;
                    ifmt_extra =
                        cmd->input_fmt - JPEGENC_FMT_YUV422_12BIT;

    #ifdef CONFIG_AMLOGIC_MEDIA_CANVAS

                    if (cmd->input_fmt == JPEGENC_FMT_YUV422_12BIT)
                        canvas_w = picsize_x * 24 / 8;
                    else if (cmd->input_fmt == JPEGENC_FMT_YUV444_10BIT)
                        canvas_w = picsize_x * 32 / 8;
                    else
                        canvas_w = (picsize_x * 20 + 7) / 8;
                    canvas_w = ((canvas_w + 31) >> 5) << 5;
                    canvas_config_proxy(enc_canvas_offset,
                        input,
                        canvas_w, picsize_y,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    input = enc_canvas_offset;
                    input = input & 0xff;
    #endif
                } else if (cmd->input_fmt == JPEGENC_FMT_YUV422_SINGLE) {
                    iformat = 0;

    #ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                    canvas_w = picsize_x * 2;
                    canvas_w = ((canvas_w + 31) >> 5) << 5;
                    canvas_config_proxy(enc_canvas_offset,
                        input,
                        canvas_w, picsize_y,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    input = enc_canvas_offset;
    #endif
                } else if ((cmd->input_fmt == JPEGENC_FMT_YUV444_SINGLE)
                    || (cmd->input_fmt == JPEGENC_FMT_RGB888)) {
                    iformat = 1;
                    if (cmd->input_fmt == JPEGENC_FMT_RGB888)
                        r2y_en = 1;

    #ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                    canvas_w = picsize_x * 3;
                    canvas_w = ((canvas_w + 31) >> 5) << 5;
                    canvas_config_proxy(enc_canvas_offset,
                        input,
                        canvas_w, picsize_y,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    input = enc_canvas_offset;
    #endif
                } else if ((cmd->input_fmt == JPEGENC_FMT_NV21)
                    || (cmd->input_fmt == JPEGENC_FMT_NV12)) {
                    iformat = (cmd->input_fmt == JPEGENC_FMT_NV21) ? 2 : 3;
    #ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                    canvas_w = ((cmd->encoder_width + 31) >> 5) << 5;
                    if (cmd->type == JPEGENC_DMA_BUFF) {
                        canvas_config_proxy(enc_canvas_offset,
                            input_y,
                            canvas_w, picsize_y,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                        canvas_config_proxy(enc_canvas_offset + 1,
                            input_u,
                            canvas_w, picsize_y / 2,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                    } else {
                        canvas_config_proxy(enc_canvas_offset,
                            input,
                            canvas_w, picsize_y,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                        canvas_config_proxy(enc_canvas_offset + 1,
                            input + canvas_w * picsize_y, canvas_w,
                            picsize_y / 2, CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                    }
                    input = ((enc_canvas_offset + 1) << 8) |
                        enc_canvas_offset;
    #endif
                } else if (cmd->input_fmt == JPEGENC_FMT_YUV420) {
                    iformat = 4;

    #ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                    canvas_w = ((cmd->encoder_width + 63) >> 6) << 6;
                    if (cmd->type == JPEGENC_DMA_BUFF) {
                        canvas_config_proxy(enc_canvas_offset,
                            input_y,
                            canvas_w, picsize_y,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                        canvas_config_proxy(enc_canvas_offset + 2,
                            input_u,
                            canvas_w / 2, picsize_y / 2,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                        canvas_config_proxy(enc_canvas_offset + 2,
                            input_v,
                            canvas_w / 2, picsize_y / 2,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                    } else {
                        canvas_config_proxy(enc_canvas_offset,
                            input,
                            canvas_w, picsize_y,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                        canvas_config_proxy(enc_canvas_offset + 2,
                            input + canvas_w * picsize_y,
                            canvas_w / 2, picsize_y / 2,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                        canvas_config_proxy(enc_canvas_offset + 2,
                            input + canvas_w * picsize_y * 5 / 4,
                            canvas_w / 2, picsize_y / 2,
                            CANVAS_ADDR_NOWRAP,
                            CANVAS_BLKMODE_LINEAR);
                    }
                    input = ((enc_canvas_offset + 2) << 16) |
                        ((enc_canvas_offset + 1) << 8) |
                        enc_canvas_offset;
    #endif
                } else if ((cmd->input_fmt == JPEGENC_FMT_YUV444_PLANE)
                    || (cmd->input_fmt == JPEGENC_FMT_RGB888_PLANE)) {
                    iformat = 5;
                    if (cmd->input_fmt == JPEGENC_FMT_RGB888_PLANE)
                        r2y_en = 1;

    #ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                    canvas_w = ((cmd->encoder_width + 31) >> 5) << 5;
                    canvas_config_proxy(enc_canvas_offset,
                        input,
                        canvas_w, picsize_y,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    canvas_config_proxy(enc_canvas_offset + 1,
                        input + canvas_w * picsize_y, canvas_w,
                        picsize_y, CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    canvas_config_proxy(enc_canvas_offset + 2,
                        input + canvas_w * picsize_y * 2,
                        canvas_w, picsize_y, CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    input = ((enc_canvas_offset + 2) << 16) |
                        ((enc_canvas_offset + 1) << 8) |
                        enc_canvas_offset;
    #endif
                } else if (cmd->input_fmt == JPEGENC_FMT_RGBA8888) {
                    iformat = 12;
                    r2y_en = 1;
                }
                ret = 0;
            }

        } else {
            if ((cmd->input_fmt == JPEGENC_FMT_RGB565)
                || (cmd->input_fmt >= JPEGENC_MAX_FRAME_FMT))
                return -1;

            if (cmd->output_fmt == JPEGENC_FMT_YUV422_SINGLE)
                oformat = 1;
            else
                oformat = 0;

            if ((cmd->input_fmt <= JPEGENC_FMT_YUV444_PLANE) ||
                (cmd->input_fmt >= JPEGENC_FMT_YUV422_12BIT))
                r2y_en = 0;
            else
                r2y_en = 1;

            if (cmd->input_fmt >= JPEGENC_FMT_YUV422_12BIT) {
                iformat = 7;
                ifmt_extra =
                    cmd->input_fmt - JPEGENC_FMT_YUV422_12BIT;

#ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                if (cmd->input_fmt == JPEGENC_FMT_YUV422_12BIT)
                    canvas_w = picsize_x * 24 / 8;
                else if (cmd->input_fmt == JPEGENC_FMT_YUV444_10BIT)
                    canvas_w = picsize_x * 32 / 8;
                else
                    canvas_w = (picsize_x * 20 + 7) / 8;
                canvas_w = ((canvas_w + 31) >> 5) << 5;
                canvas_config_proxy(enc_canvas_offset,
                    input,
                    canvas_w, picsize_y,
                    CANVAS_ADDR_NOWRAP,
                    CANVAS_BLKMODE_LINEAR);
                input = enc_canvas_offset;
                input = input & 0xff;
#endif
            } else if (cmd->input_fmt == JPEGENC_FMT_YUV422_SINGLE) {
                iformat = 0;

#ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                canvas_w = picsize_x * 2;
                canvas_w = ((canvas_w + 31) >> 5) << 5;
                canvas_config_proxy(enc_canvas_offset,
                    input,
                    canvas_w, picsize_y,
                    CANVAS_ADDR_NOWRAP,
                    CANVAS_BLKMODE_LINEAR);
                input = enc_canvas_offset;
#endif
            } else if ((cmd->input_fmt == JPEGENC_FMT_YUV444_SINGLE)
                || (cmd->input_fmt == JPEGENC_FMT_RGB888)) {
                iformat = 1;
                if (cmd->input_fmt == JPEGENC_FMT_RGB888)
                    r2y_en = 1;

#ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                canvas_w = picsize_x * 3;
                canvas_w = ((canvas_w + 31) >> 5) << 5;
                canvas_config_proxy(enc_canvas_offset,
                    input,
                    canvas_w, picsize_y,
                    CANVAS_ADDR_NOWRAP,
                    CANVAS_BLKMODE_LINEAR);
                input = enc_canvas_offset;
#endif
            } else if ((cmd->input_fmt == JPEGENC_FMT_NV21)
                || (cmd->input_fmt == JPEGENC_FMT_NV12)) {
                iformat = (cmd->input_fmt == JPEGENC_FMT_NV21) ? 2 : 3;

#ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                canvas_w = ((cmd->encoder_width + 31) >> 5) << 5;
                if (cmd->type == JPEGENC_DMA_BUFF) {
                    canvas_config_proxy(enc_canvas_offset,
                        input_y,
                        canvas_w, picsize_y,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    canvas_config_proxy(enc_canvas_offset + 1,
                        input_u,
                        canvas_w, picsize_y / 2,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                } else {
                    canvas_config_proxy(enc_canvas_offset,
                        input,
                        canvas_w, picsize_y,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    canvas_config_proxy(enc_canvas_offset + 1,
                        input + canvas_w * picsize_y, canvas_w,
                        picsize_y / 2, CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                }
                input = ((enc_canvas_offset + 1) << 8) |
                    enc_canvas_offset;
#endif
            } else if (cmd->input_fmt == JPEGENC_FMT_YUV420) {
                iformat = 4;

#ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                canvas_w = ((cmd->encoder_width + 63) >> 6) << 6;
                if (cmd->type == JPEGENC_DMA_BUFF) {
                    canvas_config_proxy(enc_canvas_offset,
                        input_y,
                        canvas_w, picsize_y,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    canvas_config_proxy(enc_canvas_offset + 1,
                        input_u,
                        canvas_w / 2, picsize_y / 2,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    canvas_config_proxy(enc_canvas_offset + 2,
                        input_v,
                        canvas_w / 2, picsize_y / 2,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                } else {
                    canvas_config_proxy(enc_canvas_offset,
                        input,
                        canvas_w, picsize_y,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    canvas_config_proxy(enc_canvas_offset + 1,
                        input + canvas_w * picsize_y,
                        canvas_w / 2, picsize_y / 2,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                    canvas_config_proxy(enc_canvas_offset + 2,
                        input + canvas_w * picsize_y * 5 / 4,
                        canvas_w / 2, picsize_y / 2,
                        CANVAS_ADDR_NOWRAP,
                        CANVAS_BLKMODE_LINEAR);
                }
                input = ((enc_canvas_offset + 2) << 16) |
                    ((enc_canvas_offset + 1) << 8) |
                    enc_canvas_offset;
#endif
            } else if ((cmd->input_fmt == JPEGENC_FMT_YUV444_PLANE)
                || (cmd->input_fmt == JPEGENC_FMT_RGB888_PLANE)) {
                iformat = 5;
                if (cmd->input_fmt == JPEGENC_FMT_RGB888_PLANE)
                    r2y_en = 1;

#ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
                canvas_w = ((cmd->encoder_width + 31) >> 5) << 5;
                canvas_config_proxy(enc_canvas_offset,
                    input,
                    canvas_w, picsize_y,
                    CANVAS_ADDR_NOWRAP,
                    CANVAS_BLKMODE_LINEAR);
                canvas_config_proxy(enc_canvas_offset + 1,
                    input + canvas_w * picsize_y, canvas_w,
                    picsize_y, CANVAS_ADDR_NOWRAP,
                    CANVAS_BLKMODE_LINEAR);
                canvas_config_proxy(enc_canvas_offset + 2,
                    input + canvas_w * picsize_y * 2,
                    canvas_w, picsize_y, CANVAS_ADDR_NOWRAP,
                    CANVAS_BLKMODE_LINEAR);
                input = ((enc_canvas_offset + 2) << 16) |
                    ((enc_canvas_offset + 1) << 8) |
                    enc_canvas_offset;
#endif
            } else if (cmd->input_fmt == JPEGENC_FMT_RGBA8888) {
                iformat = 12;
                r2y_en = 1;
            }
        }
        ret = 0;
    } else if (cmd->type == JPEGENC_CANVAS_BUFF) {
        r2y_en = 0;
        if (cmd->input_fmt == JPEGENC_FMT_YUV422_SINGLE) {
            iformat = 0;
            input = input & 0xff;
        } else if (cmd->input_fmt == JPEGENC_FMT_YUV444_SINGLE) {
            iformat = 1;
            input = input & 0xff;
        } else if ((cmd->input_fmt == JPEGENC_FMT_NV21)
            || (cmd->input_fmt == JPEGENC_FMT_NV12)) {
            iformat = (cmd->input_fmt == JPEGENC_FMT_NV21) ? 2 : 3;
            input = input & 0xffff;
        } else if (cmd->input_fmt == JPEGENC_FMT_YUV420) {
            iformat = 4;
            input = input & 0xffffff;
        } else if ((cmd->input_fmt == JPEGENC_FMT_YUV444_PLANE)
            || (cmd->input_fmt == JPEGENC_FMT_RGB888_PLANE)) {
            if (cmd->input_fmt == JPEGENC_FMT_RGB888_PLANE)
                r2y_en = 1;
            iformat = 5;
            input = input & 0xffffff;
        } else if ((cmd->input_fmt == JPEGENC_FMT_YUV422_12BIT)
            || (cmd->input_fmt == JPEGENC_FMT_YUV444_10BIT)
            || (cmd->input_fmt == JPEGENC_FMT_YUV422_10BIT)) {
            iformat = 7;
            ifmt_extra = cmd->input_fmt - JPEGENC_FMT_YUV422_12BIT;
            input = input & 0xff;
        } else
            ret = -1;
    }

    if (ret == 0)
        mfdin_basic_jpeg(input, iformat, oformat,
            picsize_x, picsize_y, r2y_en, ifmt_extra,
            mfdin_canvas0_stride,
            mfdin_canvas1_stride,
            mfdin_canvas2_stride,
            mfdin_canvas0_blkmode,
            mfdin_canvas1_blkmode,
            mfdin_canvas2_blkmode,
            mfdin_canvas0_addr,
            mfdin_canvas1_addr,
            mfdin_canvas2_addr,
            mfdin_canvas_bias,
            mfdin_big_endian);

    if (dump_input) {
        dump_raw_input(wq, mfdin_canvas0_addr, mfdin_canvas1_addr, mfdin_canvas2_addr);
    }

    return ret;
}

static void jpegenc_isr_tasklet(ulong data)
{
    struct jpegenc_manager_s *manager = (struct jpegenc_manager_s *)data;

    jenc_pr(LOG_INFO, "encoder is done %d\n", manager->encode_hw_status);

    if ((manager->encode_hw_status == JPEGENC_ENCODER_DONE)
        && (manager->process_irq)) {
        manager->wq.hw_status = manager->encode_hw_status;
        manager->wq.output_size = READ_HREG(HCODEC_VLC_TOTAL_BYTES);
        jenc_pr(LOG_INFO, "encoder size %d\n", manager->wq.output_size);
        atomic_inc(&manager->wq.ready);
        wake_up_interruptible(&manager->wq.complete);
    }
}

static irqreturn_t jpegenc_isr(s32 irq_number, void *para)
{
    struct jpegenc_manager_s *manager = (struct jpegenc_manager_s *)para;
    jenc_pr(LOG_ALL, "jpegenc intr is fired\n");

    if (manager->irq_requested == false)
        return IRQ_NONE;

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1) {
        if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_T7)
            || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2))
            WRITE_HREG(HCODEC_ASSIST_MBOX2_CLR_REG, 1);
        else
            WRITE_HREG(HCODEC_ASSIST_MBOX0_CLR_REG, 1);
    } else
        WRITE_HREG(HCODEC_ASSIST_MBOX2_CLR_REG, 1);

    manager->encode_hw_status  = READ_HREG(JPEGENC_ENCODER_STATUS);

    if (manager->encode_hw_status == JPEGENC_ENCODER_DONE) {
        jpegenc_time_count_end(&time_cnt);
        manager->process_irq = true;
        tasklet_schedule(&manager->tasklet);
    }

    return IRQ_HANDLED;
}

static void jpegenc_start(void)
{
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    WRITE_VREG(DOS_SW_RESET1, (1 << 12) | (1 << 11));
    WRITE_VREG(DOS_SW_RESET1, 0);
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    WRITE_HREG(HCODEC_MPSR, 0x0001);
}

static void _jpegenc_stop(void)
{
    ulong timeout = jiffies + HZ;

    WRITE_HREG(HCODEC_MPSR, 0);
    WRITE_HREG(HCODEC_CPSR, 0);

    while (READ_HREG(HCODEC_IMEM_DMA_CTRL) & 0x8000) {
        if (time_after(jiffies, timeout))
            break;
    }
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_GXBB)
        WRITE_VREG(DOS_SW_RESET1,
               (1 << 12) | (1 << 11) |
               (1 << 2) | (1 << 6) |
               (1 << 7) | (1 << 8) |
               (1 << 14) | (1 << 16) |
               (1 << 17));
    else
        WRITE_VREG(DOS_SW_RESET1,
               (1 << 12) | (1 << 11) |
               (1 << 2) | (1 << 6) |
               (1 << 7) | (1 << 8) |
               (1 << 16) | (1 << 17));

    WRITE_VREG(DOS_SW_RESET1, 0);

    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
}

static void dump_mem(u8 *addr) {
    int i;
    u8 *offset = addr;

    for (i=0;i<dumpmem_line;i++) {
        offset += i * 8;
        jenc_pr(LOG_INFO, "%#x\t%#x\t%#x\t%#x\t%#x\t%#x\t%#x\t%#x\n",
            *(offset+0), *(offset+1), *(offset+2), *(offset+3),
            *(offset+4), *(offset+5), *(offset+6), *(offset+7));
    }
}

static void __iomem *mc_addr;
static u32 mc_addr_map;
#define MC_SIZE (4096 * 4)
s32 jpegenc_loadmc(const char *p)
{
    ulong timeout;
    s32 ret = 0;
    //int i=0;

    /* use static mempry*/
    if (mc_addr == NULL) {
        mc_addr = kmalloc(MC_SIZE, GFP_KERNEL);

        if (!mc_addr) {
            jenc_pr(LOG_ERROR, "jpegenc loadmc iomap mc addr error.\n");
            return -ENOMEM;
        }
        memset(mc_addr, 0, MC_SIZE);
    }

    if (get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_C1)
        ret = get_data_from_name("gxl_jpeg_enc", (u8 *)mc_addr);
    else
        ret = get_firmware_data(VIDEO_ENC_JPEG, (u8 *)mc_addr);

    dump_mem(mc_addr);

    if (ret < 0)
        jenc_pr(LOG_ERROR, "jpegenc microcode fail ret=%d, name: %s.\n", ret, p);

    mc_addr_map = dma_map_single(&gJpegenc.this_pdev->dev, mc_addr, MC_SIZE, DMA_TO_DEVICE);

    WRITE_HREG(HCODEC_MPSR, 0);
    WRITE_HREG(HCODEC_CPSR, 0);

    /* Read CBUS register for timing */
    //timeout = READ_HREG(HCODEC_MPSR);
    //timeout = READ_HREG(HCODEC_MPSR);
    timeout = jiffies + HZ;
    WRITE_HREG(HCODEC_IMEM_DMA_ADR, mc_addr_map);
    WRITE_HREG(HCODEC_IMEM_DMA_COUNT, 0x1000);

    if ((get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T7) || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_S5))
        WRITE_HREG(HCODEC_IMEM_DMA_CTRL, (0x8000 | (0xf << 16))); // ucode test c is 0x8000 | (0xf << 16)
    else if ((get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T3) || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T5M) || \
        (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T3X) || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_S7D)) {
        jenc_pr(LOG_INFO, "t3 HCODEC_IMEM_DMA_CTRL (0x8000 | (0xf << 16))\n");
        WRITE_HREG(HCODEC_IMEM_DMA_CTRL, (0x8000 | (0xf << 16))); // Endian : 4'b1000);
    } else if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2) {
        jenc_pr(LOG_INFO, "sc2 HCODEC_IMEM_DMA_CTRL (0x8000 | (7 << 16))\n");
        WRITE_HREG(HCODEC_IMEM_DMA_CTRL, (0x8000 | (7 << 16))); // Endian : 4'b1000);
    } else
        WRITE_HREG(HCODEC_IMEM_DMA_CTRL, (0x8000 | (7 << 16))); // Endian : 4'b1000);

    while (READ_HREG(HCODEC_IMEM_DMA_CTRL) & 0x8000) {
        if (time_before(jiffies, timeout)) {
            schedule();
        } else {
            jenc_pr(LOG_ERROR, "hcodec load mc error\n");
            ret = -EBUSY;
            break;
        }
    }

    dma_unmap_single(&gJpegenc.this_pdev->dev, mc_addr_map, MC_SIZE, DMA_TO_DEVICE);
    return ret;
}

static s32 jpegenc_poweron_ex(u32 clock)
{
    u32 frq;

    if (clock == 1)
        frq = 200;
    else if (clock == 3)
        frq = 300;
    else
        frq = 400;

    jpeg_enc_clk_enable(&g_jpeg_enc_clks, frq);

    /* Powerup HCODEC memories */
    WRITE_VREG(DOS_MEM_PD_HCODEC, 0x0);

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_T7) {
        jenc_pr(LOG_INFO, "powering on hcodec\n");
        vdec_poweron(VDEC_HCODEC);
        jenc_pr(LOG_INFO, "hcodec power status after poweron:%d\n", vdec_on(VDEC_HCODEC));
    } else {
        pm_runtime_get_sync(&gJpegenc.this_pdev->dev);
    }

    /*
     * [21] hcodec clk_en for henc qdct
     * [20] hcodec clk_en for henc vlc
     * [19] hcodec clk_en for assist and cbus
     * [18] hcodec clk_en for ddr
     * [17] hcodec clk_en for vcpu
     * [16] hcodec clk_en for hdec assist
     * [15] hcodec clk_en for hdec dblk
     * [14] reserved
     * [13] hcodec clk_en for hdec mc
     * [12] hcodec clk_en for hdec pic_dc
     */
    WRITE_VREG_BITS(DOS_GCLK_EN0, 0x7fff, 12, 15);

    return 0;
}

static s32 jpegenc_poweroff_ex(void)
{
    WRITE_VREG_BITS(DOS_GCLK_EN0, 0, 12, 15);

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_T7) {
        jenc_pr(LOG_INFO, "powering off hcodec for t7\n");
        vdec_poweroff(VDEC_HCODEC);
        jenc_pr(LOG_INFO, "hcodec power status after poweroff:%d\n", vdec_on(VDEC_HCODEC));
    } else
        pm_runtime_put_sync(&gJpegenc.this_pdev->dev);

    /* power off HCODEC memories */
    WRITE_VREG(DOS_MEM_PD_HCODEC, 0xffffffffUL);

    jpeg_enc_clk_disable(&g_jpeg_enc_clks);

    return 0;
}

#if 0
bool jpegenc_on(void)
{
    bool hcodec_on;
    ulong flags;

    spin_lock_irqsave(&lock, flags);

    hcodec_on = vdec_on(VDEC_HCODEC);
    hcodec_on &= (gJpegenc.opened > 0);

    spin_unlock_irqrestore(&lock, flags);
    return hcodec_on;
}
#endif

static s32 jpegenc_poweron(u32 clock)
{
    //ulong flags;
    u32 data32;
    u32 frq;

    //spin_lock_irqsave(&lock, flags);

    if ((get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_C1)
        || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2)) {
        data32 = 0;
        amports_switch_gate("vdec", 1);

        if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2) {
            if (clock == 1)
                frq = 200;
            else if (clock == 3)
                frq = 300;
            else
                frq = 400;

            jpeg_enc_clk_enable(&g_jpeg_enc_clks, frq);

            pwr_ctrl_psci_smc(PDID_SC2_DOS_HCODEC, true);
        } else if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_M8) {
            WRITE_AOREG(AO_RTI_PWR_CNTL_REG0,
                (READ_AOREG(AO_RTI_PWR_CNTL_REG0) & (~0x18)));
            udelay(10);
            /* Powerup HCODEC */
            /* [1:0] HCODEC */
            WRITE_AOREG(AO_RTI_GEN_PWR_SLEEP0,
                (READ_AOREG(AO_RTI_GEN_PWR_SLEEP0) & (~0x3)));
            udelay(10);
        }

        WRITE_VREG(DOS_SW_RESET1, 0xffffffff);
        WRITE_VREG(DOS_SW_RESET1, 0);

        /* Enable Dos internal clock gating */
        jpegenc_clock_enable(clock);

        if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_M8) {
            /* Powerup HCODEC memories */
            WRITE_VREG(DOS_MEM_PD_HCODEC, 0x0);

            /* Remove HCODEC ISO */
            WRITE_AOREG(AO_RTI_GEN_PWR_ISO0,
                (READ_AOREG(AO_RTI_GEN_PWR_ISO0) & (~0x30)));
            udelay(10);
        }
        /* Disable auto-clock gate */
        WRITE_VREG(DOS_GEN_CTRL0, (READ_VREG(DOS_GEN_CTRL0) | 0x1));
        WRITE_VREG(DOS_GEN_CTRL0, (READ_VREG(DOS_GEN_CTRL0) & 0xFFFFFFFE));
    } else {
        jpegenc_poweron_ex(clock);
    }
    //spin_unlock_irqrestore(&lock, flags);

    mdelay(10);
    return 0;
}

static s32 jpegenc_poweroff(void)
{
    //ulong flags;
    //spin_lock_irqsave(&lock, flags);

    if ((get_cpu_major_id() < AM_MESON_CPU_MAJOR_ID_C1)
        || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2)) {
        if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2) {
            jpeg_enc_clk_disable(&g_jpeg_enc_clks);
            pwr_ctrl_psci_smc(PDID_SC2_DOS_HCODEC, false);
        }else if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_M8) {
            /* enable HCODEC isolation */
            WRITE_AOREG(AO_RTI_GEN_PWR_ISO0,
                READ_AOREG(AO_RTI_GEN_PWR_ISO0) | 0x30);
            /* power off HCODEC memories */
            WRITE_VREG(DOS_MEM_PD_HCODEC, 0xffffffffUL);
        }
        /* disable HCODEC clock */
        jpegenc_clock_disable();

        if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_M8) {
            /* HCODEC power off */
            WRITE_AOREG(AO_RTI_GEN_PWR_SLEEP0,
                READ_AOREG(AO_RTI_GEN_PWR_SLEEP0) | 0x3);
        }

        /* release DOS clk81 clock gating */
        amports_switch_gate("vdec", 0);
    } else {
        jpegenc_poweroff_ex();
    }
    //spin_unlock_irqrestore(&lock, flags);

    return 0;
}

void jpegenc_reset(void)
{
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_GXBB)
        WRITE_VREG(DOS_SW_RESET1,
               (1 << 2) | (1 << 6) |
               (1 << 7) | (1 << 8) |
               (1 << 14) | (1 << 16) |
               (1 << 17));
    else
        WRITE_VREG(DOS_SW_RESET1,
               (1 << 2) | (1 << 6) | (1 << 7) |
               (1 << 8) | (1 << 16) | (1 << 17));
    WRITE_VREG(DOS_SW_RESET1, 0);
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
    READ_VREG(DOS_SW_RESET1);
}

static s32 jpegenc_init(void)
{
    jpegenc_poweron(clock_level);

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_MG9TV)
        WRITE_HREG(HCODEC_ASSIST_MMC_CTRL1, 0x32);
    else
        WRITE_HREG(HCODEC_ASSIST_MMC_CTRL1, 0x2);

    jenc_pr(LOG_ALL, "start to load microcode\n");

    if (fw_tee_enabled() && !legacy_load && ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_T7 )
        || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2 ))) {
        char *buf = vmalloc(0x1000 * 16);
        int ret = -1;
        jenc_pr(LOG_INFO, "load ucode\n");
        if (get_firmware_data(VIDEO_ENC_JPEG, buf) < 0) {
            //amvdec_disable();
            jenc_pr(LOG_ERROR, "get firmware for jpeg enc fail!\n");
            vfree(buf);
            return -1;
        }
        WRITE_HREG(HCODEC_MPSR, 0);
        WRITE_HREG(HCODEC_CPSR, 0);
        ret = amvdec_loadmc_ex(VFORMAT_JPEG_ENC, NULL, buf);

        if (ret < 0) {
            //amvdec_disable();
            vfree(buf);
            jenc_pr(LOG_ERROR, "jpegenc: the %s fw loading failed, err: %x\n",
                tee_enabled() ? "TEE" : "local", ret);
            return -EBUSY;
        }
        vfree(buf);
    } else {
        if (jpegenc_loadmc(jpegenc_ucode[0]) < 0)
            return -EBUSY;
    }

    jenc_pr(LOG_ALL, "jpegenc load microcode success.\n");
    gJpegenc.process_irq = false;

    if (request_irq(gJpegenc.irq_num, jpegenc_isr, IRQF_SHARED,
        "jpegenc-irq", (void *)&gJpegenc) == 0)
        gJpegenc.irq_requested = true;
    else
        gJpegenc.irq_requested = false;

    WRITE_HREG(JPEGENC_ENCODER_STATUS, JPEGENC_ENCODER_IDLE);
    gJpegenc.inited = true;
    return 0;
}

static s32 convert_cmd(struct jpegenc_wq_s *wq, u32 *cmd_info)
{
    int i = 0;
    u32 data_offset;
    unsigned long paddr = 0;
    struct enc_dma_cfg *cfg = NULL;
    s32 ret = 0;
    if (!wq) {
        jenc_pr(LOG_ERROR, "jpegenc convert_cmd error\n");
        return -1;
    }

    memset(&wq->cmd, 0, sizeof(struct jpegenc_request_s));
    wq->cmd.type = cmd_info[0];
    wq->cmd.input_fmt = cmd_info[1];
    wq->cmd.output_fmt = cmd_info[2];
    wq->cmd.encoder_width = cmd_info[3];
    wq->cmd.encoder_height = cmd_info[4];
    wq->cmd.framesize = cmd_info[5];
    wq->cmd.src = cmd_info[6];
    wq->cmd.jpeg_quality = cmd_info[7];
    wq->cmd.QuantTable_id = cmd_info[8];
    wq->cmd.flush_flag = cmd_info[9];
    wq->cmd.block_mode = cmd_info[10];

    wq->cmd.y_stride = cmd_info[11];
    wq->cmd.u_stride = cmd_info[12];
    wq->cmd.v_stride = cmd_info[13];
    wq->cmd.h_stride = cmd_info[14];
    jenc_pr(LOG_DEBUG, "convert_cmd: ystride:%d, h_stride:%d\n",
            cmd_info[11], cmd_info[14]);

    if (is_oversize(wq->cmd.encoder_width,
        wq->cmd.encoder_height,
        wq->max_width * wq->max_height)) {
        jenc_pr(LOG_ERROR,
            "set encode size %dx%d is larger than supported (%dx%d).\n",
            wq->cmd.encoder_width,
            wq->cmd.encoder_height,
            wq->max_width,
            wq->max_height);
        return -1;
    }

    wq->cmd.jpeg_quality = jpeg_quality_scaling(wq->cmd.jpeg_quality);
    if (wq->cmd.QuantTable_id < 4) {
        jenc_pr(LOG_INFO,
            "JPEGENC_SEL_QUANT_TABLE : %d.\n",
            wq->cmd.QuantTable_id);
    } else {
        wq->cmd.QuantTable_id = 0;
        jenc_pr(LOG_ERROR,
            "JPEGENC_SEL_QUANT_TABLE invalid. target value: %d.\n",
            cmd_info[8]);
    }
    jenc_pr(LOG_INFO,
        "target quality : %d,  jpeg_quality value: %d.\n",
        cmd_info[7], wq->cmd.jpeg_quality);
    if (wq->cmd.type == JPEGENC_DMA_BUFF) {
        data_offset = 14;
        wq->cmd.plane_num = cmd_info[data_offset++];
        jenc_pr(LOG_INFO, "wq->cmd.plane_num %d\n",
            wq->cmd.plane_num);
        if (wq->cmd.input_fmt == JPEGENC_FMT_NV12 ||
            wq->cmd.input_fmt == JPEGENC_FMT_NV21 ||
            wq->cmd.input_fmt == JPEGENC_FMT_YUV420 ||
            wq->cmd.input_fmt == JPEGENC_FMT_RGB888) {
            if (wq->cmd.plane_num == 0 || wq->cmd.plane_num > 3) {
                jenc_pr(LOG_ERROR, "wq->cmd.plane_num is invalid %d.\n",
                    wq->cmd.plane_num);
                return -1;
            }
            for (i = 0; i < wq->cmd.plane_num; i++) {
                cfg = &wq->cmd.dma_cfg[i];
                cfg->dir = DMA_TO_DEVICE;
                cfg->fd = cmd_info[data_offset++];
                cfg->dev = &(gJpegenc.this_pdev->dev);

                ret = enc_dma_buf_get_phys(cfg, &paddr);
                if (ret < 0) {
                    jenc_pr(LOG_ERROR, "import fd %d failed\n",
                        cfg->fd);
                    cfg->paddr = NULL;
                    cfg->vaddr = NULL;
                    return -1;
                }
                cfg->paddr = (void *)paddr;
                jenc_pr(LOG_INFO, "paddr 0x%lx\n", (unsigned long)cfg->paddr);
            }
        } else {
            jenc_pr(LOG_ERROR, "error fmt = %d\n",
                wq->cmd.input_fmt);
        }
    }
    return 0;
}

static void jpegenc_start_cmd(struct jpegenc_wq_s *wq)
{
    gJpegenc.process_irq = false;
    gJpegenc.encode_hw_status = JPEGENC_ENCODER_IDLE;

    jpegenc_reset();

    set_jpeg_input_format(wq, &wq->cmd);

    init_jpeg_encoder(wq);

    jpegenc_init_output_buffer(wq);

    /* clear mailbox interrupt */
    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1) {
        if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_T7)
            || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_SC2))
            WRITE_HREG(HCODEC_ASSIST_MBOX2_CLR_REG, 1);
        else
            WRITE_HREG(HCODEC_ASSIST_MBOX0_CLR_REG, 1);
    } else
        WRITE_HREG(HCODEC_ASSIST_MBOX2_CLR_REG, 1);

    /* enable mailbox interrupt */
    if ((get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
        && (get_cpu_major_id() != AM_MESON_CPU_MAJOR_ID_SC2)) {
        if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_T7)
            WRITE_HREG(HCODEC_ASSIST_MBOX2_MASK, 0xffffffff);
        else
            WRITE_HREG(HCODEC_ASSIST_MBOX0_MASK, 0xffffffff);
    } else
        WRITE_HREG(HCODEC_ASSIST_MBOX2_MASK, 1);

    gJpegenc.encode_hw_status = JPEGENC_ENCODER_IDLE;
    WRITE_HREG(JPEGENC_ENCODER_STATUS, JPEGENC_ENCODER_IDLE);
    gJpegenc.process_irq = false;

    time_cnt = jpegenc_time_count_start();

    jpegenc_start();
    jenc_pr(LOG_INFO, "jpegenc_start\n");
}

static void jpegenc_stop(void)
{
    if ((gJpegenc.irq_num >= 0) &&
        (gJpegenc.irq_requested == true)) {
        gJpegenc.irq_requested = false;
        free_irq(gJpegenc.irq_num, &gJpegenc);
    }
    _jpegenc_stop();
    jpegenc_poweroff();
    jenc_pr(LOG_INFO, "jpegenc_stop\n");
}

static void dma_flush(u32 buf_start, u32 buf_size)
{
    dma_sync_single_for_device(&gJpegenc.this_pdev->dev,
        buf_start, buf_size, DMA_TO_DEVICE);
}

static void cache_flush(u32 buf_start, u32 buf_size)
{
    dma_sync_single_for_cpu(&gJpegenc.this_pdev->dev,
        buf_start, buf_size, DMA_FROM_DEVICE);
}

static s32 jpegenc_open(struct inode *inode, struct file *file)
{
    struct jpegenc_wq_s *wq;
    s32 r;
    jenc_pr(LOG_INFO, "jpegenc open, filp=%lu\n", (unsigned long)file);
#ifdef CONFIG_AM_ENCODER
    if (amvenc_avc_on() == true) {
        jenc_pr(LOG_ERROR, "hcodec in use for AVC Encode now.\n");
        return -EBUSY;
    }
#endif
    file->private_data = NULL;

    spin_lock(&gJpegenc.sem_lock);
    if (gJpegenc.opened > 0) {
        spin_unlock(&gJpegenc.sem_lock);
        jenc_pr(LOG_ERROR, "jpegenc open busy.\n");
        return -EBUSY;
    }
    wq = &gJpegenc.wq;
    wq->buf_start = gJpegenc.mem.buf_start;
    wq->buf_size = gJpegenc.mem.buf_size;
    gJpegenc.opened++;
    spin_unlock(&gJpegenc.sem_lock);

#ifdef CONFIG_CMA
    if (gJpegenc.use_reserve == false) {
        wq->buf_start = codec_mm_alloc_for_dma(DRIVER_NAME,
            gJpegenc.mem.buf_size >> PAGE_SHIFT, 0, 0);
        if (wq->buf_start) {
            wq->buf_size = gJpegenc.mem.buf_size;
        } else {
            jenc_pr(LOG_ERROR,
                "jpegenc - codec_mm allocation failed\n");
            spin_lock(&gJpegenc.sem_lock);
            gJpegenc.opened--;
            spin_unlock(&gJpegenc.sem_lock);
            return -ENOMEM;
        }
    }
#endif

    jenc_pr(LOG_DEBUG,
        "jpegenc - allocated from %s: start:0x%x, %d MB.\n",
        gJpegenc.use_reserve ? "reserved" :
            gJpegenc.use_cma ? "cma" : "codec_mm",
        wq->buf_start, gJpegenc.mem.buf_size / SZ_1M);

    spin_lock(&gJpegenc.sem_lock);
    init_waitqueue_head(&wq->complete);
    atomic_set(&wq->ready, 0);
    wq->AssiststreamStartVirtAddr = NULL;
    memset(gQuantTable, 0, sizeof(gQuantTable));
    wq->cmd.QuantTable_id = 0;
    wq->cmd.jpeg_quality = 90;
    wq->max_width = gJpegenc.mem.bufspec->max_width;
    wq->max_height = gJpegenc.mem.bufspec->max_height;
    wq->headbytes = 0;
    file->private_data = (void *)wq;
#ifdef EXTERN_QUANT_TABLE
    gExternalQuantTablePtr = NULL;
    external_quant_table_available = false;
#endif
    spin_unlock(&gJpegenc.sem_lock);
    r = 0;

    return r;
}

static s32 jpegenc_release(struct inode *inode, struct file *file)
{
    struct jpegenc_wq_s *wq = (struct jpegenc_wq_s *)file->private_data;

    if (wq != &gJpegenc.wq) {
        jenc_pr(LOG_ERROR, "jpegenc release error\n");
        return -1;
    }
    if (gJpegenc.inited) {
        jpegenc_stop();
        gJpegenc.inited = false;
    }
    enc_dma_buf_release(file);
    enc_free_buffers(file);
    memset(gQuantTable, 0, sizeof(gQuantTable));

    if (wq->AssiststreamStartVirtAddr)
        wq->AssiststreamStartVirtAddr = NULL;

#ifdef CONFIG_CMA
    if (wq->buf_start) {
        codec_mm_free_for_dma(DRIVER_NAME, wq->buf_start);
    }
#endif
    wq->buf_start = 0;
    wq->buf_size = 0;
#ifdef EXTERN_QUANT_TABLE
    kfree(gExternalQuantTablePtr);
    gExternalQuantTablePtr = NULL;
    external_quant_table_available = false;
#endif
    spin_lock(&gJpegenc.sem_lock);
    if (gJpegenc.opened > 0)
        gJpegenc.opened--;
    spin_unlock(&gJpegenc.sem_lock);
    jenc_pr(LOG_DEBUG, "jpegenc release\n");
    return 0;
}

static s32 jpegenc_reconfig_input(struct jpegenc_wq_s *wq, u32 new_addr, u32 new_size) {
    s32 ret = 0;
    struct encdrv_buffer_pool_t *vbp;

    if ((new_addr == 0) || (new_size == 0))
        return -1;

    ret = down_interruptible(&s_vpu_sem);
    if (ret == 0) {
        vbp = kzalloc(sizeof(*vbp), GFP_KERNEL);
        if (!vbp) {
            up(&s_vpu_sem);
            return -ENOMEM;
        }

        wq->InputBuffStart = new_addr;
        wq->InputBuffEnd = wq->InputBuffStart + new_size - 1;

        vbp->vb.phys_addr = new_addr;
        vbp->vb.size = new_size;

        spin_lock(&s_vpu_lock);
        list_add(&vbp->list, &s_vbp_head);
        spin_unlock(&s_vpu_lock);

        up(&s_vpu_sem);
    }
    return ret;
}

static void jpegenc_restore_input(struct jpegenc_wq_s *wq) {
    wq->InputBuffStart = wq->buf_start + gJpegenc.mem.bufspec->input.buf_start;
    wq->InputBuffEnd = wq->InputBuffStart + gJpegenc.mem.bufspec->input.buf_size - 1;
}
static long jpegenc_ioctl(struct file *file, u32 cmd, ulong arg)
{
    long r = 0;
    struct jpegenc_wq_s *wq = (struct jpegenc_wq_s *)file->private_data;
#define MAX_ADDR_INFO_SIZE 30
    u32 addr_info[MAX_ADDR_INFO_SIZE + 4];
    int shared_fd = -1;
    //struct jpegenc_frame_params *frm_params;

    struct encdrv_dma_buf_info_t dma_info;
    struct encdrv_buffer_t buf;
    struct encdrv_buffer_pool_t *pool, *n;
    struct encdrv_buffer_t vb;
    bool find = false;
    u32 cached = 0;

    switch (cmd) {
    case JPEGENC_IOC_QUERY_DMA_SUPPORT:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_QUERY_DMA_SUPPORT\n");
        jenc_pr(LOG_INFO, "use_dma_io=%u\n", use_dma_io);
        put_user(use_dma_io, (u32 *)arg);
        break;
    case JPEGENC_IOC_CONFIG_DMA_INPUT:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_CONFIG_DMA_INPUT\n");
        if (copy_from_user(&shared_fd, (void *)arg, sizeof(s32))) {
            jenc_pr(LOG_ERROR,
                "jpegenc JPEGENC_IOC_CONFIG_DMA_INPUT error.\n");
            return -1;
        }

        jenc_pr(LOG_INFO, "JPEGENC_IOC_CONFIG_DMA_INPUT, shared_fd:%d\n",
            shared_fd);
        memset (&dma_info, 0, sizeof(dma_info));
        dma_info.fd = shared_fd;
        if (enc_src_addr_config(&dma_info, file)) {
            jenc_pr(LOG_ERROR,
                    "src addr config error\n");
            r = -EFAULT;
            break;
        }
        jenc_pr(LOG_INFO, "paddr %lx\n",
                dma_info.phys_addr);
        r = jpegenc_reconfig_input(wq, dma_info.phys_addr, dma_info.size);
        break;
    case JPEGENC_IOC_RELEASE_DMA_INPUT:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_RELEASE_DMA_INPUT\n");
        enc_dma_buf_release(file);
        // restore to original input buffer config if dma buffer is revoked
        jpegenc_restore_input(wq);
        break;
    case JPEGENC_IOC_NEW_CMD:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_NEW_CMD\n");
        if (copy_from_user(addr_info, (void *)arg,
            MAX_ADDR_INFO_SIZE * sizeof(u32))) {
            jenc_pr(LOG_ERROR,
                "jpegenc get new cmd error.\n");
            return -1;
        }
        if (!gJpegenc.inited) {
            jenc_pr(LOG_DEBUG, "jpegenc uninitialized.\n");
            return -1;
        }
        if (!convert_cmd(wq, addr_info))
            jpegenc_start_cmd(wq);
        break;
    case JPEGENC_IOC_NEW_CMD2:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_NEW_CMD2\n");
        if (copy_from_user(&(wq->cmd), (void *)arg, (unsigned long) sizeof(struct jpegenc_request_s))) {
            jenc_pr(LOG_ERROR,
                "jpegenc get new cmd2 error.\n");
            return -1;
        }
        if (!gJpegenc.inited) {
            jenc_pr(LOG_DEBUG, "jpegenc uninitialized.\n");
            return -1;
        }

        dump_request(&(wq->cmd));

        if (is_oversize(wq->cmd.encoder_width,
            wq->cmd.encoder_height,
            wq->max_width * wq->max_height)) {
            jenc_pr(LOG_ERROR,
                "set encode size %dx%d is larger than supported (%dx%d).\n",
                wq->cmd.encoder_width,
                wq->cmd.encoder_height,
                wq->max_width,
                wq->max_height);
            return -1;
        }

        wq->cmd.jpeg_quality = jpeg_quality_scaling(wq->cmd.jpeg_quality);

        if (wq->cmd.QuantTable_id < 4) {
            jenc_pr(LOG_INFO, "JPEGENC_SEL_QUANT_TABLE: %d\n", wq->cmd.QuantTable_id);
        } else {
            wq->cmd.QuantTable_id = 0;
            jenc_pr(LOG_ERROR, "JPEGENC_SEL_QUANT_TABLE invalid, use 0 instead\n");
        }

        jenc_pr(LOG_INFO, "scaled jpeg_quality: %d\n", wq->cmd.jpeg_quality);

        //if (!convert_cmd(wq, addr_info))
        jpegenc_start_cmd(wq);

        break;
    case JPEGENC_IOC_GET_STAGE:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_GET_STAGE\n");
        put_user(wq->hw_status, (u32 *)arg);
        break;
    case JPEGENC_IOC_GET_OUTPUT_SIZE:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_GET_OUTPUT_SIZE\n");
        buf.phys_addr = wq->BitstreamStart;
        buf.size = wq->output_size;

        spin_lock(&s_vpu_lock);
        list_for_each_entry_safe(pool, n,
            &s_vbp_head, list) {
            //if (pool->filp == filp) {
                vb = pool->vb;
                if ((vb.phys_addr <= buf.phys_addr)
                    && ((vb.phys_addr + vb.size)
                        > buf.phys_addr)
                    && ((vb.phys_addr + vb.size)
                        >= buf.phys_addr + buf.size)
                    && find == false){
                    cached = vb.cached;
                    find = true;
                    break;
                }
            //}
        }
        spin_unlock(&s_vpu_lock);
        //if (find && cached)
        if (find)
            cache_flush(
                (u32)buf.phys_addr,
                (u32)buf.size);
        addr_info[0] = wq->headbytes;
        addr_info[1] = wq->output_size;
        r = copy_to_user((u32 *)arg, addr_info , 2 * sizeof(u32));
        break;
    case JPEGENC_IOC_CONFIG_INIT:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_CONFIG_INIT\n");
        if (gJpegenc.inited) {
            jenc_pr(LOG_DEBUG, "jpegenc initialized.\n");
            return -1;
        }
        jpegenc_init();
        r = jpegenc_buffspec_init(wq);
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_CONFIG_INIT end\n");
        break;
    case JPEGENC_IOC_GET_BUFFINFO:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_GET_BUFFINFO\n");
        addr_info[0] = gJpegenc.mem.buf_size;
        addr_info[1] = gJpegenc.mem.bufspec->input.buf_start;
        addr_info[2] = gJpegenc.mem.bufspec->input.buf_size;
        addr_info[3] = gJpegenc.mem.bufspec->assist.buf_start;
        addr_info[4] = gJpegenc.mem.bufspec->assist.buf_size;
        addr_info[5] = gJpegenc.mem.bufspec->bitstream.buf_start;
        addr_info[6] = gJpegenc.mem.bufspec->bitstream.buf_size;
        r = copy_to_user((u32 *)arg, addr_info , 7 * sizeof(u32));
        break;
    case JPEGENC_IOC_GET_DEVINFO:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_GET_DEVINFO\n");
        if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_GXL) {
            /* GXL send same id of GXTVBB to upper*/
            r = copy_to_user((s8 *)arg, JPEGENC_DEVINFO_GXTVBB,
                strlen(JPEGENC_DEVINFO_GXTVBB));
        } else if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_GXTVBB) {
            r = copy_to_user((s8 *)arg, JPEGENC_DEVINFO_GXTVBB,
                strlen(JPEGENC_DEVINFO_GXTVBB));
        } else if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_GXBB) {
            r = copy_to_user((s8 *)arg, JPEGENC_DEVINFO_GXBB,
                strlen(JPEGENC_DEVINFO_GXBB));
        } else if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_MG9TV) {
            r = copy_to_user((s8 *)arg, JPEGENC_DEVINFO_G9,
                strlen(JPEGENC_DEVINFO_G9));
        } else {
            r = copy_to_user((s8 *)arg, JPEGENC_DEVINFO_M8,
                strlen(JPEGENC_DEVINFO_M8));
        }
        break;
    case JPEGENC_IOC_SET_EXT_QUANT_TABLE:
        jenc_pr(LOG_DEBUG, "ioctl JPEGENC_IOC_SET_EXT_QUANT_TABLE\n");
#ifdef EXTERN_QUANT_TABLE
        if (arg == 0) {
            kfree(gExternalQuantTablePtr);
            gExternalQuantTablePtr = NULL;
            external_quant_table_available = false;
        } else {
            void __user *argp = (void __user *)arg;
            gExternalQuantTablePtr =
                kmalloc(sizeof(u16) * DCTSIZE2 * 2,
                GFP_KERNEL);
            if (gExternalQuantTablePtr) {
                if (copy_from_user
                    (gExternalQuantTablePtr, argp,
                    sizeof(u16) * DCTSIZE2 * 2)) {
                    r = -1;
                    break;
                }
                external_quant_table_available = true;
                r = 0;
            } else {
                jenc_pr(LOG_ERROR,
                    "gExternalQuantTablePtr malloc fail\n");
                r = -1;
            }
        }
#else
        r = 0;
#endif
        break;
    default:
        jenc_pr(LOG_DEBUG, "ioctl BAD cmd\n");
        r = -1;
        break;
    }
    return r;
}

#ifdef CONFIG_COMPAT
static long jpegenc_compat_ioctl(struct file *filp,
    unsigned int cmd, unsigned long args)
{
    unsigned long ret;

    args = (unsigned long)compat_ptr(args);
    ret = jpegenc_ioctl(filp, cmd, args);
    return ret;
}
#endif

static s32 jpegenc_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct jpegenc_wq_s *wq = (struct jpegenc_wq_s *)filp->private_data;
    ulong off = vma->vm_pgoff << PAGE_SHIFT;
    ulong vma_size = vma->vm_end - vma->vm_start;

    if (vma_size == 0) {
        jenc_pr(LOG_ERROR, "vma_size is 0\n");
        return -EAGAIN;
    }
    off += wq->buf_start;
    if ((off > (wq->buf_start + wq->buf_size)) || ((off + vma_size) > (wq->buf_start + wq->buf_size))) {
        jenc_pr(LOG_ERROR, "vma_size is 0x%lx, off is 0x%lx\n", vma_size, off);
        return -EAGAIN;
    }
    jenc_pr(LOG_INFO, "vma_size is %ld, off is %ld\n", vma_size, off);
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_IO;
    /* vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot); */
    if (remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT,
        vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
        jenc_pr(LOG_ERROR, "set_cached: failed remap_pfn_range\n");
        return -EAGAIN;
    }
    return 0;
}

static u32 jpegenc_poll(struct file *file, poll_table *wait_table)
{
    struct jpegenc_wq_s *wq = (struct jpegenc_wq_s *)file->private_data;
    poll_wait(file, &wq->complete, wait_table);

    if (atomic_read(&wq->ready)) {
        atomic_dec(&wq->ready);
        return POLLIN | POLLRDNORM;
    }
    return 0;
}

static const struct file_operations jpegenc_fops = {
    .owner = THIS_MODULE,
    .open = jpegenc_open,
    .mmap = jpegenc_mmap,
    .release = jpegenc_release,
    .unlocked_ioctl = jpegenc_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = jpegenc_compat_ioctl,
#endif
    .poll = jpegenc_poll,
};

static s32 jpegenc_wq_init(void)
{
    jenc_pr(LOG_DEBUG, "jpegenc_wq_init.\n");
    spin_lock_init(&gJpegenc.sem_lock);
    spin_lock(&gJpegenc.sem_lock);
    gJpegenc.irq_requested = false;
    gJpegenc.process_irq = false;
    gJpegenc.inited = false;
    gJpegenc.opened = 0;
    gJpegenc.encode_hw_status = JPEGENC_ENCODER_IDLE;

    tasklet_init(&gJpegenc.tasklet,
             jpegenc_isr_tasklet,
             (ulong)&gJpegenc);
    spin_unlock(&gJpegenc.sem_lock);
    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_GXBB)
        clock_level = 5;
    else if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_M8M2)
        clock_level = 3;
    else
        clock_level = 1;

    if (is_support_vdec_canvas())
        enc_canvas_offset = ENC_CANVAS_OFFSET;
    else
        enc_canvas_offset = JPEGENC_CANVAS_INDEX;
    return 0;
}

static s32 jpegenc_wq_uninit(void)
{
    s32 r = -1;
    jenc_pr(LOG_DEBUG, "uninit encode wq.\n");
    if ((gJpegenc.encode_hw_status == JPEGENC_ENCODER_IDLE) || (gJpegenc.encode_hw_status == JPEGENC_ENCODER_DONE)) {
        if ((gJpegenc.irq_num >= 0) &&
            (gJpegenc.irq_requested == true)) {
            free_irq(gJpegenc.irq_num, &gJpegenc);
            gJpegenc.irq_requested = false;
        }
        r = 0;
    }
    return  r;
}

static ssize_t encode_status_show(struct class *cla,
    struct class_attribute *attr, char *buf)
{
    s32 irq_num;
    u32 hw_status, width, height;
    bool process_irq;
    bool inited;
    bool use_reserve;
    u32 cma_size, max_w, max_h;
    u32 buffer_start, buffer_size;
    u8 lev, opened;
    struct Jpegenc_Buff_s res;

    spin_lock(&gJpegenc.sem_lock);

    irq_num = gJpegenc.irq_num;
    hw_status = gJpegenc.encode_hw_status;
    process_irq = gJpegenc.process_irq;
    inited = gJpegenc.inited;
    opened = gJpegenc.opened;
    use_reserve = gJpegenc.use_reserve;
    res.buf_start = gJpegenc.mem.reserve_mem.buf_start;
    res.buf_size = gJpegenc.mem.reserve_mem.buf_size;
    buffer_start = gJpegenc.mem.buf_start;
    buffer_size = gJpegenc.mem.buf_size;
    lev = gJpegenc.mem.cur_buf_lev;
    max_w = gJpegenc.mem.bufspec->max_width;
    max_h = gJpegenc.mem.bufspec->max_height;
    width = gJpegenc.wq.cmd.encoder_width;
    height = gJpegenc.wq.cmd.encoder_height;
#ifdef CONFIG_CMA
    cma_size = gJpegenc.mem.cma_pool_size / SZ_1M;
#endif
    spin_unlock(&gJpegenc.sem_lock);

    jenc_pr(LOG_DEBUG,
        "jpegenc width: %d, encode height: %d.\n",
        width, height);
    jenc_pr(LOG_DEBUG,
        "jpegenc hw_status: %d, process_irq: %s.\n",
        hw_status, process_irq ? "true" : "false");
    jenc_pr(LOG_DEBUG,
        "jpegenc irq num: %d,  inited: %s, opened: %d\n",
        irq_num, inited ? "true" : "false", opened);
    if (use_reserve) {
        jenc_pr(LOG_DEBUG,
            "jpegenc reserve memory, buffer start: 0x%x, size: %d MB.\n",
            res.buf_start, res.buf_size / SZ_1M);
    } else {
#ifdef CONFIG_CMA
        jenc_pr(LOG_DEBUG, "jpegenc cma pool size: %d.\n", cma_size);
#endif
    }
    jenc_pr(LOG_DEBUG, "jpegenc buffer start: 0x%x, size: 0x%x\n",
        buffer_start, buffer_size);
    jenc_pr(LOG_DEBUG, "buffer level: %s\n", glevel_str[lev]);
    return snprintf(buf, 40, "max size: %dx%d\n", max_w, max_h);
}

static int enc_dma_buf_map(struct enc_dma_cfg *cfg)
{
    long ret = -1;
    int fd = -1;
    struct dma_buf *dbuf = NULL;
    struct dma_buf_attachment *d_att = NULL;
    struct sg_table *sg = NULL;
    void *vaddr = NULL;
    struct device *dev = NULL;
    enum dma_data_direction dir;

    if (cfg == NULL || (cfg->fd < 0) || cfg->dev == NULL) {
        jenc_pr(LOG_ERROR, "error input param\n");
        return -EINVAL;
    }

    jenc_pr(LOG_INFO, "enc_dma_buf_map, fd %d\n", cfg->fd);

    fd = cfg->fd;
    dev = cfg->dev;
    dir = cfg->dir;
    jenc_pr(LOG_INFO, "enc_dma_buffer_map fd %d\n", fd);

    dbuf = dma_buf_get(fd);

    if (IS_ERR_OR_NULL(dbuf)) {
        jenc_pr(LOG_ERROR, "failed to get dma buffer,fd %d\n",fd);
        return -EINVAL;
    }

    d_att = dma_buf_attach(dbuf, dev);

    if (IS_ERR(d_att)) {
        jenc_pr(LOG_ERROR, "failed to set dma attach\n");
        goto attach_err;
    }

    sg = dma_buf_map_attachment(d_att, dir);

    if (IS_ERR(sg)) {
        jenc_pr(LOG_ERROR, "failed to get dma sg\n");
        goto map_attach_err;
    }

    cfg->dbuf = dbuf;
    cfg->attach = d_att;
    cfg->vaddr = vaddr;
    cfg->sg = sg;
    cfg->size = dbuf->size;
    jenc_pr(LOG_INFO, "dmabuf size is %zu\n", cfg->size);

    return 0;

map_attach_err:
    dma_buf_detach(dbuf, d_att);

attach_err:
    dma_buf_put(dbuf);

    return ret;
}

static int enc_dma_buf_get_phys(struct enc_dma_cfg *cfg, unsigned long *addr)
{
    struct sg_table *sg_table;
    struct page *page;
    int ret;
    jenc_pr(LOG_INFO, "jpegenc_dma_buf_get_phys in\n");

    ret = enc_dma_buf_map(cfg);

    if (ret < 0) {
        jenc_pr(LOG_ERROR, "jpegenc_dma_buf_get_phys failed\n");
        return ret;
    }

    if (cfg->sg) {
        sg_table = cfg->sg;
        page = sg_page(sg_table->sgl);
        *addr = PFN_PHYS(page_to_pfn(page));
        ret = 0;
    }

    jenc_pr(LOG_INFO, "jpegenc_dma_buf_get_phys 0x%lx\n", *addr);
    return ret;
}

static void enc_dma_buf_unmap(struct enc_dma_cfg *cfg)
{
    int fd = -1;
    struct dma_buf *dbuf = NULL;
    struct dma_buf_attachment *d_att = NULL;
    struct sg_table *sg = NULL;
    //void *vaddr = NULL;
    struct device *dev = NULL;
    enum dma_data_direction dir;

    if (cfg == NULL || (cfg->fd < 0) || cfg->dev == NULL
       || cfg->dbuf == NULL /*|| cfg->vaddr == NULL*/
       || cfg->attach == NULL || cfg->sg == NULL) {
        jenc_pr(LOG_ERROR, "Error input param\n");
        return;
    }

    fd = cfg->fd;
    dev = cfg->dev;
    dir = cfg->dir;
    dbuf = cfg->dbuf;
    d_att = cfg->attach;
    sg = cfg->sg;

    dma_buf_unmap_attachment(d_att, sg, dir);

    dma_buf_detach(dbuf, d_att);

    dma_buf_put(dbuf);
    jenc_pr(LOG_DEBUG, "enc_dma_buffer_unmap fd %d\n",fd);
}

static s32 enc_src_addr_config(struct encdrv_dma_buf_info_t *pinfo,
        struct file *filp)
{
    struct encdrv_dma_buf_pool_t *vbp;
    unsigned long phy_addr;
    struct enc_dma_cfg *cfg;
    s32 ret = 0;

    vbp = kzalloc(sizeof(*vbp), GFP_KERNEL);
    if (!vbp) {
        ret = -ENOMEM;
        return ret;
    }
    memset(vbp, 0, sizeof(struct encdrv_dma_buf_pool_t));
    cfg = &vbp->dma_cfg;
    cfg->dir = DMA_TO_DEVICE;
    cfg->fd = pinfo->fd;
    cfg->dev = &(gJpegenc.this_pdev->dev);
    phy_addr = 0;
    ret = enc_dma_buf_get_phys(cfg, &phy_addr);
    if (ret < 0) {
        jenc_pr(LOG_ERROR, "import fd %d failed\n", cfg->fd);
        kfree(vbp);
        ret = -1;
        return ret;
    }
    pinfo->phys_addr = (ulong) phy_addr;
    pinfo->size = cfg->size;
    vbp->filp = filp;
    spin_lock(&s_dma_buf_lock);
    list_add(&vbp->list, &s_dma_bufp_head);
    spin_unlock(&s_dma_buf_lock);
    jenc_pr(LOG_INFO, "enc_src_addr_config phy_addr 0x%lx\n",
        pinfo->phys_addr);
    return ret;
}

static s32 enc_dma_buf_release(struct file *filp)
{
    struct encdrv_dma_buf_pool_t *pool, *n;
    struct enc_dma_cfg vb;

    jenc_pr(LOG_DEBUG, "enc_release_dma_buffers\n");
    list_for_each_entry_safe(pool, n, &s_dma_bufp_head, list) {
        if (pool->filp == filp) {
                vb = pool->dma_cfg;
                if (vb.attach) {
                enc_dma_buf_unmap(&vb);
                spin_lock(&s_dma_buf_lock);
                list_del(&pool->list);
                spin_unlock(&s_dma_buf_lock);
                kfree(pool);
            }
        }
    }
    return 0;
}

static s32 enc_free_buffers(struct file *filp)
{
    struct encdrv_buffer_pool_t *pool, *n;
    struct encdrv_buffer_t vb;

    jenc_pr(LOG_DEBUG, "enc_free_buffers\n");
    list_for_each_entry_safe(pool, n, &s_vbp_head, list) {
        //if (pool->filp == filp) {
            vb = pool->vb;
            if (vb.phys_addr) {
                spin_lock(&s_vpu_lock);
                list_del(&pool->list);
                spin_unlock(&s_vpu_lock);
                kfree(pool);
            }
        //}
    }
    return 0;
}

static ssize_t power_ctrl_show(struct class *cla, struct class_attribute *attr, char *buf) {
    jenc_pr(LOG_INFO, "power status: %lu\n", pwr_ctrl_status_psci_smc(PDID_T7_DOS_HCODEC));
    jenc_pr(LOG_INFO, "jpeg clk: %ld\n", clk_get_rate(g_jpeg_enc_clks.jpeg_enc_clk));
    return 1;//snprintf(buf, PAGE_SIZE, "power control show done\n");
}

static ssize_t power_ctrl_store(struct class *class,struct class_attribute *attr,
        const char *buf, size_t count) {
    if (strncmp(buf, "poweron", 7) == 0) {
        jenc_pr(LOG_INFO, "now powering on:\n");
        //pwr_ctrl_psci_smc(PM_HCODEC, true);
        //jpegenc_poweron_ex(6);
        jpegenc_init();
    } else if (strncmp(buf, "poweroff", 8) == 0) {
        jenc_pr(LOG_INFO, "now powering off:\n");
        //pwr_ctrl_psci_smc(PM_HCODEC, false);
        jpegenc_poweroff_ex();
    } else if (strncmp(buf, "mbox0", 5) == 0) {
        jenc_pr(LOG_INFO, "trigger mbox0:\n");

        WRITE_HREG(HCODEC_ASSIST_MBOX0_MASK, 1);   //enable irq
        WRITE_VREG(HCODEC_ASSIST_MBOX0_IRQ_REG, 0x1);  // set irq
    } else if (strncmp(buf, "mbox1", 5) == 0) {
        jenc_pr(LOG_INFO, "trigger mbox1:\n");

        WRITE_HREG(HCODEC_ASSIST_MBOX1_MASK, 1);   //enable irq
        WRITE_VREG(HCODEC_ASSIST_MBOX1_IRQ_REG, 0x1);  // set irq
    } else if (strncmp(buf, "mbox2", 5) == 0) {
        jenc_pr(LOG_INFO, "trigger mbox2:\n");

        WRITE_HREG(HCODEC_ASSIST_MBOX2_MASK, 1);   //enable irq
        WRITE_VREG(HCODEC_ASSIST_MBOX2_IRQ_REG, 0x1);  // set irq
    }

    return 1;//snprintf(buf, PAGE_SIZE, "policy read,just for test\n");
}

static CLASS_ATTR_RO(encode_status);
static CLASS_ATTR_RW(power_ctrl);
//static CLASS_ATTR(clock_ctrl, 0664, clock_ctrl_show, clock_ctrl_store);

static struct attribute *jpegenc_class_attrs[] = {
    &class_attr_encode_status.attr,
    &class_attr_power_ctrl.attr,
    //&class_attr_clock_ctrl.attr,
    NULL
};

ATTRIBUTE_GROUPS(jpegenc_class);

static struct class jpegenc_class = {
    .name = CLASS_NAME,
    .class_groups = jpegenc_class_groups,
};

s32 init_jpegenc_device(void)
{
    s32 r = 0;
    r = register_chrdev(0, DEVICE_NAME, &jpegenc_fops);
    if (r <= 0) {
        jenc_pr(LOG_ERROR, "register jpegenc device error\n");
        return r;
    }
    jpegenc_device_major = r;

    r = class_register(&jpegenc_class);
    if (r < 0) {
        jenc_pr(LOG_ERROR, "error create jpegenc class.\n");
        return r;
    }

    jpegenc_dev = device_create(&jpegenc_class, NULL,
        MKDEV(jpegenc_device_major, 0), NULL,
        DEVICE_NAME);

    if (IS_ERR(jpegenc_dev)) {
        jenc_pr(LOG_ERROR, "create jpegenc device error.\n");
        class_unregister(&jpegenc_class);
        return -1;
    }
    return r;
}

s32 uninit_jpegenc_device(void)
{
    if (jpegenc_dev)
        device_destroy(&jpegenc_class, MKDEV(jpegenc_device_major, 0));

    class_destroy(&jpegenc_class);

    unregister_chrdev(jpegenc_device_major, DEVICE_NAME);
    return 0;
}

static s32 jpegenc_mem_device_init(struct reserved_mem *rmem,
        struct device *dev)
{
    s32 r = 0;
    struct resource res;
    if (!rmem) {
        jenc_pr(LOG_ERROR,
            "Can't obtain I/O memory, will allocate jpegenc buffer!\n");
        r = -EFAULT;
        return r;
    }
    res.start = (phys_addr_t) rmem->base;
    res.end = res.start + (phys_addr_t) rmem->size - 1;
    gJpegenc.mem.reserve_mem.buf_start = res.start;
    gJpegenc.mem.reserve_mem.buf_size = res.end - res.start + 1;
    jenc_pr(LOG_DEBUG, "found reserved memory device(start:0x%x, size:0x%x)\n",
        gJpegenc.mem.reserve_mem.buf_start, gJpegenc.mem.reserve_mem.buf_size);
    if (gJpegenc.mem.reserve_mem.buf_size >=
        jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_VGA].min_buffsize)
        gJpegenc.use_reserve = true;
    else {
        jenc_pr(LOG_ERROR,
            "jpegenc reserve_mem too small, size is %d.\n",
            gJpegenc.mem.reserve_mem.buf_size);
        gJpegenc.mem.reserve_mem.buf_start = 0;
        gJpegenc.mem.reserve_mem.buf_size = 0;
        return -EFAULT;
    }
    return r;
}

static s32 jpegenc_probe(struct platform_device *pdev)
{
    s32 res_irq;
    s32 idx;

    jenc_pr(LOG_DEBUG, "jpegenc probe start.\n");

    gJpegenc.this_pdev = pdev;
    gJpegenc.use_reserve = false;
    gJpegenc.use_cma = false;

    jpeg_in_full_hcodec = 0;
    mfdin_ambus_canv_conv = 0;

    if ((get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T3) || \
        (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T5M) || \
        (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T3X) || \
        (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_S7D)) {
        jenc_pr(LOG_INFO, "jpegenc_probe: jpeg_in_full_hcodec\n");
        jpeg_in_full_hcodec = 1;
        mfdin_ambus_canv_conv = 1;
    }

    memset(&gJpegenc.mem, 0, sizeof(struct jpegenc_meminfo_s));

    idx = of_reserved_mem_device_init(&pdev->dev);
    if (idx != 0) {
        jenc_pr(LOG_DEBUG,
            "jpegenc memory resource undefined. err=%d\n", idx);
    }

    if (gJpegenc.use_reserve == false) {
#ifndef CONFIG_CMA
        jenc_pr(LOG_ERROR,
            "jpegenc memory is invalid, probe fail!\n");
        return -EFAULT;
#else
        struct device_node *mem_node;
        struct reserved_mem *rmem = NULL;

        mem_node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
        if (mem_node)
            rmem = of_reserved_mem_lookup(mem_node);
        of_node_put(mem_node);

        if (rmem) {
            jenc_pr(LOG_DEBUG,
                "jpegenc - reserved cma node found: %s.\n", rmem->name);
            gJpegenc.mem.cma_pool_size = rmem->size;
            gJpegenc.use_cma = true;
            jenc_pr(LOG_DEBUG,
                "jpegenc - codec mm pool size: %d MB.\n", codec_mm_get_free_size() / SZ_1M);

        } else {
            jenc_pr(LOG_DEBUG,
                "jpegenc - reserved cma node not found, using codec mm pool size.\n");
            gJpegenc.mem.cma_pool_size = codec_mm_get_free_size();
        }

        jenc_pr(LOG_DEBUG,
            "jpegenc - cma memory pool size: %d MB\n",
            (u32)gJpegenc.mem.cma_pool_size / SZ_1M);
        gJpegenc.mem.buf_size = gJpegenc.mem.cma_pool_size;
#endif
    } else {

        jenc_pr(LOG_DEBUG, "using reserved memory(start:0x%x, size:%d MB)\n",
            gJpegenc.mem.reserve_mem.buf_start,
            (u32)gJpegenc.mem.reserve_mem.buf_size / SZ_1M);
        gJpegenc.mem.buf_start = gJpegenc.mem.reserve_mem.buf_start;
        gJpegenc.mem.buf_size = gJpegenc.mem.reserve_mem.buf_size;
    }

    // when use_cma is false, choose JPEGENC_BUFFER_LEVEL_8M as default
    if (gJpegenc.use_cma == false) {
        jenc_pr(LOG_DEBUG, "set mem spec to JPEGENC_BUFFER_LEVEL_8M\n");
        gJpegenc.mem.buf_size = jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_8M].min_buffsize;
    }

    if (gJpegenc.mem.buf_size >=
        jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_HD].min_buffsize) {
        gJpegenc.mem.cur_buf_lev = JPEGENC_BUFFER_LEVEL_HD;
        gJpegenc.mem.bufspec = (struct Jpegenc_BuffInfo_s *)
            &jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_HD];
    } else if (gJpegenc.mem.buf_size >=
        jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_13M].min_buffsize) {
        gJpegenc.mem.cur_buf_lev = JPEGENC_BUFFER_LEVEL_13M;
        gJpegenc.mem.bufspec = (struct Jpegenc_BuffInfo_s *)
            &jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_13M];
    } else if (gJpegenc.mem.buf_size >=
        jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_8M].min_buffsize) {
        gJpegenc.mem.cur_buf_lev = JPEGENC_BUFFER_LEVEL_8M;
        gJpegenc.mem.bufspec = (struct Jpegenc_BuffInfo_s *)
            &jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_8M];
    } else if (gJpegenc.mem.buf_size >=
        jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_5M].min_buffsize) {
        gJpegenc.mem.cur_buf_lev = JPEGENC_BUFFER_LEVEL_5M;
        gJpegenc.mem.bufspec = (struct Jpegenc_BuffInfo_s *)
            &jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_5M];
    } else if (gJpegenc.mem.buf_size >=
        jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_3M].min_buffsize) {
        gJpegenc.mem.cur_buf_lev = JPEGENC_BUFFER_LEVEL_3M;
        gJpegenc.mem.bufspec = (struct Jpegenc_BuffInfo_s *)
            &jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_3M];
    } else if (gJpegenc.mem.buf_size >=
        jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_2M].min_buffsize) {
        gJpegenc.mem.cur_buf_lev = JPEGENC_BUFFER_LEVEL_2M;
        gJpegenc.mem.bufspec = (struct Jpegenc_BuffInfo_s *)
            &jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_2M];
    } else if (gJpegenc.mem.buf_size >=
        jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_VGA].min_buffsize) {
        gJpegenc.mem.cur_buf_lev = JPEGENC_BUFFER_LEVEL_VGA;
        gJpegenc.mem.bufspec = (struct Jpegenc_BuffInfo_s *)
            &jpegenc_buffspec[JPEGENC_BUFFER_LEVEL_VGA];
    } else {
        jenc_pr(LOG_ERROR,
            "jpegenc probe memory too small, size is %d.\n",
            gJpegenc.mem.buf_size);
        gJpegenc.mem.buf_start = 0;
        gJpegenc.mem.buf_size = 0;
        return -EFAULT;
    }

    if ((get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_T7)  || (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_S5)) {
        switch (manual_irq_num) {
            case 0:
                res_irq = platform_get_irq_byname(pdev, "dos_mbox_slow_irq0");
                jenc_pr(LOG_INFO, "[%s:%d] get irq dos_mbox_slow_irq0, res_irq=%d\n", __FUNCTION__, __LINE__, res_irq);
                break;
            case 1:
                res_irq = platform_get_irq_byname(pdev, "dos_mbox_slow_irq1");
                jenc_pr(LOG_INFO, "[%s:%d] get irq dos_mbox_slow_irq1, res_irq=%d\n", __FUNCTION__, __LINE__, res_irq);
                break;
            case 2:
                res_irq = platform_get_irq_byname(pdev, "dos_mbox_slow_irq2");
                jenc_pr(LOG_INFO, "[%s:%d] get irq dos_mbox_slow_irq2, res_irq=%d\n", __FUNCTION__, __LINE__, res_irq);
                break;
            default:

                res_irq = platform_get_irq_byname(pdev, "dos_mbox_slow_irq0");
                jenc_pr(LOG_INFO, "[%s:%d] get irq dos_mbox_slow_irq0, res_irq=%d\n", __FUNCTION__, __LINE__, res_irq);
                break;
        }
    } else {
        res_irq = platform_get_irq(pdev, 0);
    }

    if (res_irq < 0) {
        jenc_pr(LOG_ERROR, "[%s] get irq error!", __func__);
        return -EINVAL;
    } else
        jenc_pr(LOG_DEBUG, "[%s] get irq success: %d!, manual_irq_num=%d\n", __func__, res_irq, manual_irq_num);

    gJpegenc.irq_num = res_irq;

    jenc_pr(LOG_DEBUG,
        "jpegenc memory config success, buff size is 0x%x, level: %s\n",
        gJpegenc.mem.buf_size,
        glevel_str[gJpegenc.mem.cur_buf_lev]);

    jpegenc_wq_init();
    init_jpegenc_device();

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1) {
        if (jpeg_enc_clk_get(&pdev->dev, &g_jpeg_enc_clks) != 0) {
            jenc_pr(LOG_ERROR, "jpeg_enc_clk_get failed\n");
            return -1;
        }
    }
    pm_runtime_enable(&gJpegenc.this_pdev->dev);

    jenc_pr(LOG_DEBUG, "jpegenc probe end.\n");
    return 0;
}

static s32 jpegenc_remove(struct platform_device *pdev)
{
    if (jpegenc_wq_uninit())
        jenc_pr(LOG_ERROR, "jpegenc_wq_uninit error.\n");

    of_reserved_mem_device_release(&pdev->dev);

    if (get_cpu_major_id() >= AM_MESON_CPU_MAJOR_ID_C1)
        jpeg_enc_clk_put(&pdev->dev, &g_jpeg_enc_clks);

    uninit_jpegenc_device();
    jenc_pr(LOG_DEBUG, "jpegenc remove.\n");
    return 0;
}

static const struct of_device_id amlogic_jpegenc_dt_match[] = {
    {
        .compatible = "amlogic, jpegenc",
    },
    {},
};

static struct platform_driver jpegenc_driver = {
    .probe = jpegenc_probe,
    .remove = jpegenc_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = amlogic_jpegenc_dt_match,
    }
};

#if 0
static struct codec_profile_t jpegenc_profile = {
    .name = "jpegenc",
    .profile = ""
};
#endif

static s32 __init jpegenc_driver_init_module(void)
{
    if (get_cpu_major_id() == AM_MESON_CPU_MAJOR_ID_TXHD2) {
        jenc_pr(LOG_DEBUG, "The chip is not support jpegenc!!\n");
        return -1;
    }

    jenc_pr(LOG_DEBUG, "jpegenc module init\n");

    if (platform_driver_register(&jpegenc_driver)) {
        jenc_pr(LOG_ERROR, "failed to register jpegenc driver\n");
        return -ENODEV;
    }

#if 0
    vcodec_profile_register(&jpegenc_profile);
#endif
    enc_register_set_debug_level_func(DEBUG_AMVENC_JPEG, set_log_level);
    return 0;
}

static void __exit jpegenc_driver_remove_module(void)
{
    jenc_pr(LOG_DEBUG, "jpegenc module remove.\n");
    platform_driver_unregister(&jpegenc_driver);
}

static const struct reserved_mem_ops rmem_jpegenc_ops = {
    .device_init = jpegenc_mem_device_init,
};

static s32 __init jpegenc_mem_setup(struct reserved_mem *rmem)
{
    rmem->ops = &rmem_jpegenc_ops;
    jenc_pr(LOG_DEBUG, "jpegenc reserved mem setup.\n");
    return 0;
}

module_param(simulation_enable, uint, 0664);
MODULE_PARM_DESC(simulation_enable, "\n simulation_enable\n");

module_param(g_block_mode, uint, 0664);
MODULE_PARM_DESC(g_block_mode, "\n g_block_mode\n");

module_param(g_canv0_stride, uint, 0664);
MODULE_PARM_DESC(g_canv0_stride, "\n g_canv0_stride\n");

module_param(g_canv1_stride, uint, 0664);
MODULE_PARM_DESC(g_canv1_stride, "\n g_canv1_stride\n");

module_param(g_canv2_stride, uint, 0664);
MODULE_PARM_DESC(g_canv2_stride, "\n g_canv2_stride\n");

module_param(g_canvas_height, uint, 0664);
MODULE_PARM_DESC(g_canvas_height, "\n g_canvas_height\n");

module_param(clock_level, uint, 0664);
MODULE_PARM_DESC(clock_level, "\n clock_level\n");

module_param(jpegenc_print_level, uint, 0664);
MODULE_PARM_DESC(jpegenc_print_level, "\n jpegenc_print_level\n");

module_param(reg_offset, int, 0664);
MODULE_PARM_DESC(reg_offset, "\n reg_offset\n");

module_param(use_dma_io, uint, 0664);
MODULE_PARM_DESC(use_dma_io, "\n use dma io or not\n");

module_param(use_quality, uint, 0664);
MODULE_PARM_DESC(use_quality, "\n use_quality\n");

module_param(legacy_load, uint, 0664);
MODULE_PARM_DESC(legacy_load, "\n legacy_load\n");

module_param(dumpmem_line, uint, 0664);
MODULE_PARM_DESC(dumpmem_line, "\n dumpmem_line\n");

module_param(pointer, uint, 0664);
MODULE_PARM_DESC(pointer, "\n pointer\n");

/*######### DEBUG-BRINGUP#########*/
module_param(manual_clock, uint, 0664);
MODULE_PARM_DESC(manual_clock, "\n manual_clock\n");

module_param(manual_irq_num, uint, 0664);
MODULE_PARM_DESC(manual_irq_num, "\n manual_irq_num\n");

module_param(manual_interrupt, uint, 0664);
MODULE_PARM_DESC(manual_interrupt, "\n manual_interrupt\n");
/*################################*/

module_init(jpegenc_driver_init_module);
module_exit(jpegenc_driver_remove_module);
RESERVEDMEM_OF_DECLARE(jpegenc, "amlogic, jpegenc-memory", jpegenc_mem_setup);

module_param(dump_input, uint, 0664);
MODULE_PARM_DESC(dump_input, "\n dump_input\n");

MODULE_DESCRIPTION("AMLOGIC JPEG Encoder Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("simon.zheng <simon.zheng@amlogic.com>");
