#ifndef __AWB_SHM_H__
#define __AWB_SHM_H__

#include <sys/types.h>
#include "ot_common.h"

/* 环形缓冲区配置 */
#define AWB_RING_BUFFER_SIZE    64  /* 缓存最近64帧数据 */
#define VIPIPE 0

/* ---- 共享内存结构体（与PHP端一致，不变）---- */
typedef struct {
    td_u16 u16Rgain;            /* R通道增益  2 bytes*/
    td_u16 u16Grgain;           /* Gr通道增益 2 bytes 这个和下面的u16Gbgain因为不怎么需要，这边直接复用一个为ColorTemp*/
    td_u16 u16Gbgain;           /* Gb通道增益 2 bytes*/
    td_u16 u16Bgain;            /* B通道增益 2 bytes*/
    td_u16 u16GlobalR;          /* 统计数据 Global R 2 bytes*/
    td_u16 u16GlobalG;          /* 统计数据 Global G 2 bytes*/
    td_u16 u16GlobalB;          /* 统计数据 Global B 2 bytes*/
    td_u32 u32Timestamp;        /* 时间戳 (秒) 4 bytes  ← 这里要对齐！*/
    td_u32 u32FrameId;          /* 帧ID 4 bytes*/
} AWB_FRAME_DATA_S;

/* 环形缓冲区结构体 - 无锁设计（单写者-多读者） */
typedef struct {
    AWB_FRAME_DATA_S stFrames[AWB_RING_BUFFER_SIZE];  /* 数据缓冲 */
    volatile td_u32 u32WriteIdx;   /* 写入位置 (volatile保证可见性) */
    volatile td_u32 u32FrameCount; /* 总帧数 */
    volatile td_u32 u32LatestId;   /* 最新帧ID */
    /* 当前生效的增益值（PHP写，C读→ISP） */
    volatile td_u16 u16CurrentRgain;
    volatile td_u16 u16CurrentGrgain;
    volatile td_u16 u16CurrentGbgain;
    volatile td_u16 u16CurrentBgain;
    volatile td_bool bInitialized;
    volatile td_bool bAutoMode;   /* 自动模式 */
    volatile td_u16 u16CurrentColorTemp;
} AWB_RING_BUFFER_S;


#endif /* __AWB_SHM_H__ */
