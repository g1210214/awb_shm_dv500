# AWB SHM DV500 — Hi3519DV500 白平衡共享内存控制工具

基于 **Hi3519DV500 SDK (SS SDK)** 的 AWB（自动白平衡）手动控制程序。通过 **System V 共享内存** 实现 C 程序与 PHP 网页之间的数据交换，支持网页端实时读取 ISP 白平衡统计数据、手动设置增益值、切换自动/手动模式。

## 架构概览

```
┌──────────────┐   共享内存 (System V IPC)   ┌──────────────┐
│   awb_shm    │◄──────────────────────────►│  awb_ffi.php │
│  (C 程序)    │    AWB_RING_BUFFER_S        │  (PHP FFI)   │
│              │                             │              │
│ ISP ←→ 增益  │  ① C 写入 ISP 统计数据       │ ① 读统计数据 │
│ 统计数据     │  ② PHP 写入增益 → C 读 → ISP │ ② 写增益值   │
└──────────────┘                             └──────┬───────┘
      │                                             │
      ▼                                             ▼
  Hi3519DV500 ISP                              Web 浏览器
  (ViPipe 0)                              http://IP/awb_ffi.php
```

## 核心功能

| 功能 | 说明 |
|------|------|
| **ISP 统计数据采集** | 每秒读取 ISP AWB 统计数据 (`global_r/g/b`)，写入环形缓冲区 |
| **增益值下发** | PHP 设置 R/B 增益值 → C 程序读取 → 写入 ISP |
| **自动/手动模式切换** | 支持在网页端切换 AWB 自动模式和手动增益模式 |
| **历史数据查看** | 环形缓冲区缓存最近 64 帧数据，支持历史查询 |
| **无锁设计** | 单写者（C 程序）- 多读者（PHP），使用 `volatile` 保证可见性 |

## 编译

在 SDK 环境下执行 `make` 即可：

```bash
make
```

Makefile 使用 SDK 的 `../Makefile.param` 交叉编译参数，生成 `awb_shm` 可执行文件。

## 使用方法

### 1. 启动 C 程序

```bash
./awb_shm
```

程序启动后会：
1. 创建（或附加）共享内存，key 通过 `ftok("/tmp", 'A')` 生成
2. 初始化环形缓冲区（若为创建者），从 ISP 读取当前增益作为初始值
3. 启动 AWB 控制线程，每秒循环执行：读 ISP 统计 → 处理模式切换 → 写环形缓冲区
4. 等待 PHP 端写入增益值

按 `Ctrl+C` 退出，程序会自动释放共享内存。

### 2. PHP 端（需 PHP 7.4+ 开启 FFI 扩展）

#### Web 模式

将 `awb_ffi.php` 放到 Web 服务器根目录，浏览器访问：

| URL | 功能 |
|-----|------|
| `http://IP/awb_ffi.php?cmd=status` | 查看环形缓冲区状态 |
| `http://IP/awb_ffi.php?cmd=read` | 读取最新一帧统计数据及当前增益 |
| `http://IP/awb_ffi.php?cmd=history&count=10` | 查看最近 10 帧历史数据 |
| `http://IP/awb_ffi.php?cmd=write&r=256&b=256` | 写入手动增益（自动切为手动模式） |
| `http://IP/awb_ffi.php?cmd=auto` | 切回自动白平衡模式 |

#### CLI 模式

```bash
php awb_ffi.php status              # 查看状态
php awb_ffi.php read                # 读取当前数据
php awb_ffi.php history 10          # 查看最近 10 帧
php awb_ffi.php write 300 400       # 写入 R=300 B=400 增益
php awb_ffi.php auto                # 切回自动模式
```

## 共享内存结构

```c
// 单帧数据 (24 bytes)
typedef struct {
    td_u16 u16Rgain;       // R通道增益
    td_u16 u16Grgain;      // Gr通道增益 (复用为色温 ColorTemp)
    td_u16 u16Gbgain;      // Gb通道增益
    td_u16 u16Bgain;       // B通道增益
    td_u16 u16GlobalR;     // 统计 Global R
    td_u16 u16GlobalG;     // 统计 Global G
    td_u16 u16GlobalB;     // 统计 Global B
    td_u32 u32Timestamp;   // 时间戳 (Unix 秒)
    td_u32 u32FrameId;     // 帧ID
} AWB_FRAME_DATA_S;

// 环形缓冲区控制结构
typedef struct {
    AWB_FRAME_DATA_S stFrames[64];   // 数据缓冲 (1536 bytes)
    volatile td_u32 u32WriteIdx;     // 写入位置
    volatile td_u32 u32FrameCount;   // 总帧数
    volatile td_u32 u32LatestId;     // 最新帧ID
    volatile td_u16 u16CurrentRgain; // 当前R增益 (PHP写, C读)
    volatile td_u16 u16CurrentGrgain;
    volatile td_u16 u16CurrentGbgain;
    volatile td_u16 u16CurrentBgain;
    volatile td_bool bInitialized;   // 初始化标志
    volatile td_bool bAutoMode;      // 自动模式标志
    volatile td_u16 u16CurrentColorTemp;
} AWB_RING_BUFFER_S;
```

## 数据流详解

```
                    ┌─────────────────────────────────┐
                    │         C 程序主循环 (1秒)        │
                    │                                   │
  共享内存增益值 ──► ① 拍快照: shm → 局部变量 stMwbAttr │
                    │                                   │
  ISP AWB 统计  ──► ② 读统计数据 → stWBStat            │
                    │                                   │
                    │ ③ awb_set_mode():                 │
                    │    手动模式 → 写增益到ISP          │
                    │    自动模式 → 读ISP实时增益更新    │
                    │                                   │
                    │ ④ fill_ringbuffer():              │
  环形缓冲区   ◄──   │    统计数据+增益 → stFrames[]    │
                    └─────────────────────────────────┘
```

### 自动 / 手动模式逻辑

- **手动模式** (`bAutoMode=0`)：PHP 设置的 `u16CurrentRgain/Bgain` 被写入 ISP，ISP 使用手动增益
- **自动模式** (`bAutoMode=1`)：ISP 自行计算白平衡，C 程序查询实时增益并更新到共享内存
- PHP 执行 `write` 命令时自动切换为手动模式；执行 `auto` 命令时切回自动模式

## 增益值说明

| 增益值 | 十六进制 | 含义 |
|--------|----------|------|
| `0x100` (256) | 0x0100 | 1.0x（基准增益） |
| `0x200` (512) | 0x0200 | 2.0x |
| `0x080` (128) | 0x0080 | 0.5x |

有效范围：`0 ~ 4095`（0x000 ~ 0xFFF，即 0x100 = 1.0x 时约 15.99x）

> **注意**：Gr/Gb 增益固定为 `0x100`（1.0x），PHP 端仅控制 R 和 B 增益。

## 文件说明

| 文件 | 说明 |
|------|------|
| `awb_shm.c` | C 主程序：共享内存管理、ISP 数据采集、AWB 控制 |
| `awb_shm.h` | 共享内存结构体定义（C 和 PHP 两端需保持一致） |
| `awb_ffi.php` | PHP FFI 端：共享内存读写、Web 界面、CLI 工具 |
| `Makefile` | 编译脚本（依赖 SDK 的 `Makefile.param`） |

## 注意事项

1. **PHP FFI 要求**：PHP 需开启 `FFI` 扩展（`php.ini` 中 `ffi.enable=true`）
2. **启动顺序**：需先启动 C 程序（创建共享内存），再运行 PHP 端
3. **共享内存 Key**：通过 `ftok("/tmp", 'A')` 生成，备用 Key 为 `0x1234`
4. **内存清理**：只有共享内存的创建者（C 程序）退出时才会删除共享内存
5. **ViPipe**：默认使用 `ViPipe 0`，如需修改请更改 `awb_shm.h` 中的 `VIPIPE` 宏
6. **字段复用**：`u16Grgain` 字段被复用为色温值（ColorTemp），因为 Gr/Gb 增益固定不调整
