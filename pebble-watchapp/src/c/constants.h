/**
 * constants.h - Peleprompter 共用常數定義
 *
 * 定義所有通訊協定鍵值、UI 參數、緩衝區大小等
 */
#pragma once

#include <pebble.h>

// ============================================================
// 通訊協定鍵值 (與 Android 端一致)
// ============================================================
#define KEY_COMMAND        0   // (Int) 指令類型
#define KEY_SCROLL_SPEED   3   // (Int) 捲動速度 1-6
#define KEY_TEXT_SIZE      4   // (Int) 文字大小 0=小, 1=中, 2=大
#define KEY_PAGE_NUM       5   // (Int) 頁碼 (0-indexed)
#define KEY_CHUNK_INDEX    6   // (Int) 此頁圖片資料的區塊索引
#define KEY_TOTAL_CHUNKS   7   // (Int) 此頁共幾個區塊
#define KEY_IMAGE_DATA     8   // (Bytes) 圖片原始位元組
#define KEY_TOTAL_PAGES    9   // (Int) 文件總頁數
#define KEY_WATCH_WIDTH    10  // (Int) 手錶螢幕寬度 (pixels)
#define KEY_WATCH_HEIGHT   11  // (Int) 手錶螢幕高度 (pixels)

// ============================================================
// 指令類型
// ============================================================
#define CMD_REQUEST_TEXT     0  // 手錶→手機：初始化請求 (含螢幕尺寸與文字大小)
#define CMD_SYNC_SETTINGS    2  // 雙向：同步設定 (速度/字型大小)
#define CMD_INIT_IMAGES      3  // 手機→手錶：初始化圖片模式 (總頁數+尺寸)
#define CMD_REQUEST_PAGE     4  // 手錶→手機：請求第 N 頁圖片
#define CMD_SEND_IMAGE_CHUNK 5  // 手機→手錶：傳送圖片資料區塊

// ============================================================
// 文字大小列舉
// ============================================================
typedef enum {
    TEXT_SIZE_TINY   = 0,   // 9px  (index.js) / 10px (Android) — 細筆畫
    TEXT_SIZE_SMALL  = 1,   // 11px (index.js) / 13px (Android) — 細筆畫
    TEXT_SIZE_MEDIUM = 2,   // 14px (index.js) / 16px (Android) — 原 Small
    TEXT_SIZE_LARGE  = 3,   // 18px (index.js) / 22px (Android) — 原 Medium
    TEXT_SIZE_XLARGE = 4    // 22px (index.js) / 28px (Android) — 原 Large
} TextSizeLevel;

// ============================================================
// 圖片模式設定
// ============================================================
// 每個圖片區塊最大位元組數 (需留出 AppMessage header 空間)
#define IMAGE_CHUNK_DATA_SIZE  1900

// 每個平台最多快取幾頁圖片
// Aplite 堆積約 24KB，每頁 1-bit 圖片約 3KB，最多快取 2 頁
// 彩色平台堆積較大，可快取 3 頁
#if defined(PBL_PLATFORM_APLITE)
  #define MAX_PAGES_CACHED 2
#else
  #define MAX_PAGES_CACHED 3
#endif

// 當距下一頁邊界剩餘像素低於此門檻時，預取下一頁
// 等同於 2 個螢幕高度的讀取緩衝
#define IMAGE_PREFETCH_THRESHOLD_PX  336   // 約 2 × 168px

// AppMessage 收發緩衝區大小
// 收件匣需足夠容納一個完整的圖片區塊加上 header
#define INBOX_SIZE   2048
#define OUTBOX_SIZE  256

// ============================================================
// 捲動速度相關
// ============================================================
#define SCROLL_SPEED_MIN       1
#define SCROLL_SPEED_MAX       6
#define SCROLL_SPEED_DEFAULT   3

// 自動捲動計時器間隔 (毫秒) - 依速度等級對應
extern const uint16_t SCROLL_INTERVALS_MS[7];

// 每次捲動的像素數
#define SCROLL_STEP_PX         1

// 手動捲動後暫停自動捲動的時間 (毫秒)
#define MANUAL_SCROLL_PAUSE_MS 2000

// 手動捲動每次跳動的像素數
#define MANUAL_SCROLL_SHORT_PX 60
#define MANUAL_SCROLL_LONG_PX  120

// ============================================================
// 持久化儲存鍵
// ============================================================
#define PERSIST_KEY_SCROLL_SPEED    100
#define PERSIST_KEY_TEXT_SIZE       101
#define PERSIST_KEY_SHOW_STATUS_BAR 102
