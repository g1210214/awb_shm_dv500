<?php
/**
 * AWB Ring Buffer Reader/Writer (FFI版)
 *
 * CLI:
 *   php awb_ffi.php read
 *   php awb_ffi.php history 10
 *   php awb_ffi.php write R B
 *   php awb_ffi.php status
 *
 * Web:
 *   http://IP/awb_ffi.php?cmd=status
 *   http://IP/awb_ffi.php?cmd=write&r=300&b=400
 */

// ---- 配置（与C程序一致）----
define('AWB_RING_BUFFER_SIZE', 64);
// --- 单帧内部偏移 ---
define('FRAME_SIZE', 24);
define('FRAME_R_GAIN',      0);   // u16
define('FRAME_GR_GAIN',     2);   // u16
define('FRAME_GB_GAIN',     4);   // u16
define('FRAME_B_GAIN',      6);   // u16
define('FRAME_GLOBAL_R',    8);   // u16
define('FRAME_GLOBAL_G',   10);   // u16
define('FRAME_GLOBAL_B',   12);   // u16
//                              14 = padding
define('FRAME_TIMESTAMP',  16);   // u32
define('FRAME_ID',         20);   // u32

// --- 控制区域偏移 ---
define('OFFSET_WRITE_IDX',     FRAME_SIZE * AWB_RING_BUFFER_SIZE); // 1536 u32
define('OFFSET_FRAME_COUNT',  1540); // u32
define('OFFSET_LATEST_ID',    1544); // u32
define('OFFSET_CUR_RGAIN',    1548); // u16
define('OFFSET_CUR_GRGAIN',   1550); // u16
define('OFFSET_CUR_GBAGAIN',  1552); // u16
define('OFFSET_CUR_BGAIN',    1554); // u16
define('OFFSET_INITIALIZED',  1556); // u32 (td_bool 是一个 enum，在 C 语言中 enum 默认底层类型是 int，也就是 4 字节)
define('OFFSET_AUTO_MODE',    1560); // td_bool
define('OFFSET_CUR_COLORTEMP', 1564); // u16

// ---- FFI 加载 libc ----
$ffi = FFI::cdef("
    int shmget(int key, unsigned long size, int flag);
    void* shmat(int id, void* addr, int flag);
    int ftok(const char* path, int id);
", "libc.so.6");

$buf = null;

// 共享内存连接
function shm_open() {
    global $ffi, $buf;
    $key = $ffi->ftok("/tmp", 65); // 与 C 程序一致，'A' 的 ASCII = 65
    $id  = $ffi->shmget($key, 0, 0666);
    if ($id < 0) $id = $ffi->shmget(0x1234, 0, 0666); // 备用键值
    if ($id < 0) { echo "Error: Cannot open shared memory. awb_self running?\n"; return false; }
    $buf = $ffi->shmat($id, null, 0);
    return $buf != null;
}

// 内存读写辅助函数
function r16($off) {
    global $ffi, $buf;
    $p = $ffi->cast("uint8_t*", $buf);
    return $p[$off] | ($p[$off+1] << 8);
}

function r32($off) {
    global $ffi, $buf;
    $p = $ffi->cast("uint8_t*", $buf);
    return $p[$off] | ($p[$off+1]<<8) | ($p[$off+2]<<16) | ($p[$off+3]<<24);
}

function w16($off, $val) {
    global $ffi, $buf;
    $p = $ffi->cast("uint8_t*", $buf);
    $p[$off]   = $val & 0xFF;
    $p[$off+1] = ($val >> 8) & 0xFF;
}

function w32($off, $val) {
    global $ffi, $buf;
    $p = $ffi->cast("uint8_t*", $buf);
    $p[$off]   = $val & 0xFF;
    $p[$off+1] = ($val >> 8) & 0xFF;
    $p[$off+2] = ($val >> 16) & 0xFF;
    $p[$off+3] = ($val >> 24) & 0xFF;
}

// 单帧数据读取
function read_frame($idx) {
    $o = $idx * FRAME_SIZE;
    return [
        'u16Rgain'     => r16($o + FRAME_R_GAIN),
        'u16ColorTemp'    => r16($o + FRAME_GR_GAIN),
        'u16Gbgain'    => r16($o + FRAME_GB_GAIN),
        'u16Bgain'     => r16($o + FRAME_B_GAIN),
        'u16GlobalR'   => r16($o + FRAME_GLOBAL_R),
        'u16GlobalG'   => r16($o + FRAME_GLOBAL_G),
        'u16GlobalB'   => r16($o + FRAME_GLOBAL_B),
        'u32Timestamp' => r32($o + FRAME_TIMESTAMP),
        'u32FrameId'   => r32($o + FRAME_ID),
    ];
}

function check_init() {
    if (!shm_open()) exit;
    if (r16(OFFSET_INITIALIZED) == 0) { echo "Warning: Not initialized.\n"; exit; }
}

// ---- 命令实现 ----
function cmd_read() {
    check_init();

    $wi = r32(OFFSET_WRITE_IDX); 
    $fc = r32(OFFSET_FRAME_COUNT); 
    $li = r32(OFFSET_LATEST_ID);
    $f = read_frame(($wi - 1) % AWB_RING_BUFFER_SIZE);
    echo "Timestamp: " . date("Y-m-d H:i:s", $f['u32Timestamp']) . "\n";
    echo "\n========================================\n  AWB Current Data\n========================================\n\n";
    echo "Write Index: $wi | Frame Count: $fc | Latest ID: $li\n\n";
    echo "Gains: R=" . r16(OFFSET_CUR_RGAIN) . " B=" . r16(OFFSET_CUR_BGAIN) . " C_T=" . r16(OFFSET_CUR_COLORTEMP) . "\n\n";
    echo "Globa: ID={$f['u32FrameId']} R={$f['u16GlobalR']} G={$f['u16GlobalG']} B={$f['u16GlobalB']}\n";
}

function cmd_history($count = 10) {
    check_init();
    $wi = r32(OFFSET_WRITE_IDX); 
    $fc = r32(OFFSET_FRAME_COUNT);
    $count = min($count, $fc, AWB_RING_BUFFER_SIZE);
    echo "\n=== Last $count frames ===\n";
    echo "FrameID | Timestamp      | GlobalR | GlobalG | GlobalB | Rgain  | Bgain  | ColorTemp\n";
    echo "--------+----------------+---------+---------+---------+--------+--------+----------\n";
    for ($i = 0; $i < $count; $i++) {
        $f = read_frame(($wi - $count + $i) % AWB_RING_BUFFER_SIZE);
        printf("%7d | %s | %7d | %7d | %7d | 0x%04x | 0x%04x | 0x%04x\n",
            $f['u32FrameId'], date("Y-m-d H:i:s", $f['u32Timestamp']),
            $f['u16GlobalR'], $f['u16GlobalG'], $f['u16GlobalB'],
            $f['u16Rgain'], $f['u16Bgain'], $f['u16ColorTemp']);
    }
}

function cmd_write($r, $b) {
    check_init();
    $r = max(0, min(4095, (int)$r));
    $b = max(0, min(4095, (int)$b));
    w16(OFFSET_CUR_RGAIN, $r);  
    // w16(OFFSET_CUR_GRGAIN, 256);
    // w16(OFFSET_CUR_GBAGAIN, 256); 
    w16(OFFSET_CUR_BGAIN, $b);
    w32(OFFSET_AUTO_MODE, 0);   // ← 写增益时自动切回手动
    echo "\nWritten: R Gain=$r (0x" . sprintf("%04x",$r) . ") B Gain=$b (0x" . sprintf("%04x",$b) . ")\n";
    echo "Gr=256 Gb=256 (fixed 1.0)\n";
}

function cmd_status() {
    if (!shm_open()) exit;
    $init = r16(OFFSET_INITIALIZED);
    echo "\n========================================\n  AWB Ring Buffer Status\n========================================\n\n";
    echo "Status: " . ($init ? "Initialized" : "Not initialized") . "\n";
    if ($init) {
        echo "Write Index: " . r32(OFFSET_WRITE_IDX) . "\n";
        echo "Frame Count: " . r32(OFFSET_FRAME_COUNT) . "\n";
        echo "Latest ID:   " . r32(OFFSET_LATEST_ID) . "\n";
        echo "Capacity:    " . AWB_RING_BUFFER_SIZE . " frames\n";
    }
}

function cmd_auto() {
    check_init();
    w32(OFFSET_AUTO_MODE, 1);  // 设标志位 = 1
    echo "Switched to Auto WB mode.\n";
}

// ---- 主程序 ----
$is_web = php_sapi_name() !== 'cli';

if ($is_web) {
    $cmd = $_GET['cmd'] ?? 'help';
} else {
    $cmd = $argv[1] ?? 'help';
}

ob_start();

switch ($cmd) {
    case 'read':    
        cmd_read(); 
        break;
    case 'auto':
        cmd_auto();
        break;
    case 'history': 
        cmd_history($is_web ? ($_GET['count'] ?? 10) : ($argv[2] ?? 10)); 
        break;
    case 'write':
        $r = $is_web ? ($_GET['r'] ?? 0) : ($argv[2] ?? 0);
        $b = $is_web ? ($_GET['b'] ?? 0) : ($argv[3] ?? 0);
        cmd_write($r, $b);
        break;
    case 'status':  
        cmd_status(); 
        break;
    default:
        echo "Usage: php awb_ffi.php [status|read|history N|write R B]\n";
        echo "Web:   http://IP/awb_ffi.php?cmd=status\n";
        break;
}

$out = ob_get_clean();

// Web 模式：用深色主题 HTML 包裹输出，带导航链接和增益写入表单 + CLI 模式：直接输出纯文本
if ($is_web) {
    header('Content-Type: text/html; charset=utf-8');
    echo "<!DOCTYPE html><html><head><meta charset='utf-8'><title>AWB</title>";
    echo "<style>body{background:#1a1a2e;color:#eee;font-family:monospace;padding:20px}";
    echo "pre{background:#16213e;padding:15px;border-radius:5px;white-space:pre-wrap}";
    echo "a{color:#0ff;margin-right:10px}input{background:#222;color:#eee;border:1px solid #444;padding:3px}</style>";
    echo "</head><body>";
    echo "<div><a href='?cmd=status'>Status</a><a href='?cmd=read'>Read</a><a href='?cmd=history&count=10'>History</a><a href='?cmd=auto'>auto</a></div>";
    echo "<form style='margin:10px 0'>r_gain:<input name='r' value='256' size=4>b_gain:<input name='b' value='256' size=4>"
       . "<input type='hidden' name='cmd' value='write'><button>Write</button></form>";
    echo "<pre>" . htmlspecialchars($out) . "</pre>";
    echo "</body></html>";
} else {
    echo $out;
}
