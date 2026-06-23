/*
 * AWB Manual Control Program for Hi3519DV500 (SS SDK)
 * 
 * 功能：
 * 1. 创建共享内存 + 环形缓冲区（与PHP awb_ffi.php 共享）
 * 2. 获取ISP AWB统计数据 (global_r/g/b) → 写入环形缓冲区
 * 3. 读PHP设置的增益值 (u16CurrentRgain等) → 写入ISP
 * 4. 使用环形缓冲区(Ring Buffer)存储历史数据，无锁设计
 *
 * 用法: ./awb_shm [ViPipe]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>

#include "ot_common.h"
#include "ot_common_isp.h"
#include "hi_mpi_isp.h"
#include "awb_shm.h"

// 共享内存全局变量 
static td_s32 g_shmId = -1;
static td_bool g_is_shm_owner = TD_FALSE;
static td_bool g_running = TD_TRUE;
static AWB_RING_BUFFER_S *g_awb_shm = NULL;

// 辅助函数(信号处理函数)
static td_void awb_signal_handler(td_s32 s32Signo)
{
    printf("\nReceived signal %d, exiting...\n", s32Signo);
    g_running = TD_FALSE;
}

/**************************************初始化共享内存和环形缓冲区 + 释放共享内存 ***************************/
static td_s32 shm_setup(td_void)
{
    key_t key;
    /* 创建共享内存 key */
    key = ftok("/tmp", 'A');
    if (key == -1)
    {
        printf("ftok failed, using default key\n");
        key = 0x1234;
    }
    
    /* 尝试创建共享内存 */
    g_shmId = shmget(key, sizeof(AWB_RING_BUFFER_S), IPC_CREAT | IPC_EXCL | 0666);
    if (g_shmId == -1)
    {
        if (errno == EEXIST)
        {
            // 共享内存已经存在了 → 说明别人先创建的
            printf("Shared memory already exists, attaching...\n");
            g_shmId = shmget(key, sizeof(AWB_RING_BUFFER_S), 0666);
            g_is_shm_owner = TD_FALSE;   
        }
        else
        {
            // 共享内存创建失败
            printf("shmget failed: %s\n", strerror(errno));
            return TD_FAILURE;
        }
    }
    else
    {
        g_is_shm_owner = TD_TRUE;
        printf("Created new shared memory, key=0x%x, size=%d bytes\n", 
               key, (td_s32)sizeof(AWB_RING_BUFFER_S));
    }
    
    /* 映射共享内存 */
    g_awb_shm = (AWB_RING_BUFFER_S *)shmat(g_shmId, NULL, 0);
    if (g_awb_shm == (void *)-1)
    {
        printf("shmat failed: %s\n", strerror(errno));
        shmctl(g_shmId, IPC_RMID, NULL);
        return TD_FAILURE;
    }
    
    // 如果是创建者，初始化环形缓冲区 
    if (g_is_shm_owner)
    {
        td_s32 ret;
        ot_isp_wb_info wb_info;

        memset(g_awb_shm, 0, sizeof(AWB_RING_BUFFER_S));
        g_awb_shm->u32WriteIdx = 0;
        g_awb_shm->u32FrameCount = 0;
        g_awb_shm->u32LatestId = 0;
        g_awb_shm->bAutoMode = TD_TRUE;

        /* 查询PQ里面的 WB Info*/
        ret = ss_mpi_isp_query_wb_info(VIPIPE, &wb_info);
        if (ret == TD_SUCCESS)
        {
            g_awb_shm->u16CurrentRgain  = wb_info.r_gain;
            g_awb_shm->u16CurrentGrgain = wb_info.gr_gain;
            g_awb_shm->u16CurrentGbgain = wb_info.gb_gain;
            g_awb_shm->u16CurrentBgain  = wb_info.b_gain;
            g_awb_shm->u16CurrentColorTemp = wb_info.color_temp;
            printf("Init gains from ISP: R=%u Gr=%u Gb=%u B=%u\n",
                wb_info.r_gain,wb_info.gr_gain,wb_info.gb_gain,wb_info.b_gain);
        }
        else
        {
            /* 读取 ISP 失败，回退到固定默认值 1.0x */
            printf("Failed to read ISP gains (%#x), using default 0x100\n", ret);
            g_awb_shm->u16CurrentRgain  = 0x100;
            g_awb_shm->u16CurrentGrgain = 0x100;
            g_awb_shm->u16CurrentGbgain = 0x100;
            g_awb_shm->u16CurrentBgain  = 0x100;
        }

        g_awb_shm->bInitialized = TD_TRUE;
        g_awb_shm->bAutoMode = TD_TRUE;
        printf("Auto mode: %d\n", g_awb_shm->bAutoMode);
        printf("Ring buffer initialized, capacity=%d frames\n", AWB_RING_BUFFER_SIZE);
    }

    return TD_SUCCESS;
}


static td_void shm_teardown(td_void)
{
    if (g_awb_shm != NULL && g_awb_shm != (void *)-1)
    {
        shmdt(g_awb_shm);
        g_awb_shm = NULL;
    }
    
    /* 只有创建者才删除共享内存 */
    if (g_is_shm_owner && g_shmId != -1)
    {
        shmctl(g_shmId, IPC_RMID, NULL);
        printf("Shared memory deleted.\n");
    }
    
    g_shmId = -1;
}
/************************************************************************************************************ */

/* 写入环形缓冲区（无锁，单写者） */
static td_void fill_ringbuffer(const ot_isp_wb_stats *pstWBStat,const ot_isp_mwb_attr *pstMwbAttr)
{
    td_u32 idx;
    time_t now;
    
    
    if (g_awb_shm == NULL)
    {
        return;
    }
    
    /* 计算写入位置 */
    idx = g_awb_shm->u32WriteIdx % AWB_RING_BUFFER_SIZE;
    
    /* 获取时间戳 */
    time(&now);
    
    /* 写入数据到缓冲区 */
    g_awb_shm->stFrames[idx].u16GlobalR = pstWBStat->global_r / 100;
    g_awb_shm->stFrames[idx].u16GlobalG = pstWBStat->global_g / 100;
    g_awb_shm->stFrames[idx].u16GlobalB = pstWBStat->global_b / 100;
    g_awb_shm->stFrames[idx].u16Rgain = pstMwbAttr->r_gain;
    // g_awb_shm->stFrames[idx].u16Grgain = pstMwbAttr->gr_gain;
    // g_awb_shm->stFrames[idx].u16Gbgain = pstMwbAttr->gb_gain;
    g_awb_shm->stFrames[idx].u16Bgain = pstMwbAttr->b_gain;
    g_awb_shm->stFrames[idx].u32Timestamp = (td_u32)now;
    g_awb_shm->stFrames[idx].u32FrameId = g_awb_shm->u32LatestId + 1;
    g_awb_shm->stFrames[idx].u16Grgain = g_awb_shm->u16CurrentColorTemp; // 因为有两个不需要，我这边直接复用
    
    /* 更新索引（volatile写入，保证顺序） */
    g_awb_shm->u32LatestId++;
    g_awb_shm->u32WriteIdx++;
    if (g_awb_shm->u32FrameCount < AWB_RING_BUFFER_SIZE)
    {
        g_awb_shm->u32FrameCount++;
    }
}

/*
 * awb_set_mode — 根据 bAutoMode 标志位，设置 ISP 白平衡为自动或手动模式
 * 参数:
 *   ViPipe     — 视频管道号
 *   pstMwbAttr — 输入/输出：手动增益值（手动模式时用输入值，自动模式时被更新为ISP实时值）
 * 逻辑:
 *   手动模式 (bAutoMode=0): 把 PHP 设置的增益写入 ISP，切手动
 *   自动模式 (bAutoMode=1): 切自动，并查询 ISP 实时增益 → 更新到 pstMwbAttr 和共享内存
 * 返回: TD_SUCCESS 或错误码
 */
static td_s32 awb_set_mode(ot_vi_pipe ViPipe, ot_isp_mwb_attr *pstMwbAttr)
{
    td_s32 ret;
    ot_isp_wb_attr stWBAttr;
    
    // WB Attr
    ret = ss_mpi_isp_get_wb_attr(ViPipe, &stWBAttr);
    if (TD_SUCCESS != ret)
    {
        printf("ss_mpi_isp_get_wb_attr failed with %#x\n", ret);
        return ret;
    }
    
    if(g_awb_shm->bAutoMode == TD_FALSE){

        stWBAttr.op_type = OT_OP_MODE_MANUAL;
        stWBAttr.manual_attr.r_gain = pstMwbAttr->r_gain;
        stWBAttr.manual_attr.gr_gain = pstMwbAttr->gr_gain;
        stWBAttr.manual_attr.gb_gain = pstMwbAttr->gb_gain;
        stWBAttr.manual_attr.b_gain = pstMwbAttr->b_gain;
        printf("Auto mode: %d\n", g_awb_shm->bAutoMode);
    }else{
        stWBAttr.op_type = OT_OP_MODE_AUTO;
        ot_isp_wb_info wb_info;
        ret = ss_mpi_isp_query_wb_info(VIPIPE, &wb_info);
        pstMwbAttr->r_gain = wb_info.r_gain;
        pstMwbAttr->gr_gain= wb_info.gr_gain;
        pstMwbAttr->gb_gain = wb_info.gb_gain;
        pstMwbAttr->b_gain  = wb_info.b_gain;
        // 更新共享内存中的当前增益值
        g_awb_shm->u16CurrentRgain = wb_info.r_gain;
        g_awb_shm->u16CurrentGrgain = wb_info.gr_gain;
        g_awb_shm->u16CurrentGbgain = wb_info.gb_gain;
        g_awb_shm->u16CurrentBgain = wb_info.b_gain;
        g_awb_shm->u16CurrentColorTemp = wb_info.color_temp;
        printf("Auto mode: %d\n", g_awb_shm->bAutoMode);
    }
    
    ret = ss_mpi_isp_set_wb_attr(ViPipe, &stWBAttr);
    if (TD_SUCCESS != ret)
    {
        printf("ss_mpi_isp_set_wb_attr failed with %#x\n", ret);
        return ret;
    }
    
    return TD_SUCCESS;
}

/* AWB控制线程（主循环） */
static td_void *awb_thread(td_void *arg)
{
    td_u32 u32FrameCnt = 0;
    ot_isp_wb_stats stWBStat;
    ot_isp_mwb_attr stMwbAttr;
 
    printf("AWB thread started, ViPipe=%d\n", VIPIPE);
    
    while (g_running)
    {
        /* ① 拍快照：共享内存 → 局部变量（一次性拷贝，保证一致） */
        memset(&stMwbAttr, 0, sizeof(stMwbAttr));
        stMwbAttr.r_gain  = g_awb_shm->u16CurrentRgain;
        stMwbAttr.gr_gain = g_awb_shm->u16CurrentGrgain;
        stMwbAttr.gb_gain = g_awb_shm->u16CurrentGbgain;
        stMwbAttr.b_gain  = g_awb_shm->u16CurrentBgain;

        /* ②读ISP统计数据 → 存入局部变量 stWBStat -- 查询 PQ 3A Analyzer里面的AWB 模块*/
        td_s32 ret = ss_mpi_isp_get_wb_stats(VIPIPE, &stWBStat);
        if (TD_SUCCESS != ret)
        {
            printf("ss_mpi_isp_get_wb_stats failed with %#x\n", ret);
            usleep(1000000);
            continue;
        }

        // ② 处理：对局部变量做各种操作（自动模式查ISP更新、手动模式直接用）
        awb_set_mode(VIPIPE, &stMwbAttr);
        
        // ④ 写回：局部变量 → 环形缓冲区（供history查看）
        fill_ringbuffer(&stWBStat, &stMwbAttr);

        printf("[Frame %u] ISP: R=%u G=%u B=%u | Gain: R=%u Gr=%u Gb=%u B=%u\n",
               u32FrameCnt,
               stWBStat.global_r / 100, stWBStat.global_g / 100, stWBStat.global_b / 100,
               stMwbAttr.r_gain, stMwbAttr.gr_gain, stMwbAttr.gb_gain, stMwbAttr.b_gain);
        
        u32FrameCnt++;
        usleep(1000000);  /* 1秒更新一次 */
    }
    
    printf("AWB thread exited. Total frames: %u\n", u32FrameCnt);
    return NULL;
}

/* 主函数 */
td_s32 main(td_s32 argc, td_char *argv[])
{
    td_s32 ret;
    pthread_t awbThread;
    
    printf("\n========================================\n");
    printf("  AWB Manual Control (PHP -> ISP)\n");
    printf("  Hi3519DV500 SDK\n");
    printf("ViPipe: %d\n", VIPIPE);
    printf("========================================\n\n");

    /* 设置信号处理 */
    signal(SIGINT, awb_signal_handler);
    signal(SIGTERM, awb_signal_handler);
    
    /* 初始化环形缓冲区 */
    shm_setup();
    
    printf("Waiting for PHP to write gains...\n");
    printf("Open: http://IP/awb_ffi.php?cmd=read\n\n");
    
    /* 创建AWB控制线程 */
    ret = pthread_create(&awbThread, NULL, awb_thread, NULL);
    if (ret != 0)
    {
        printf("Failed to create AWB thread, error: %d\n", ret);
        shm_teardown();
        return TD_FAILURE;
    }
    
    /* 等待线程结束 */
    pthread_join(awbThread, NULL);
    
    /* 释放共享内存 */
    shm_teardown();
    
    printf("\nProgram exited normally.\n");
    return TD_SUCCESS;
}
