/**
 * constants.h - Peleprompter 共用常數定義
 * 
 * 定義所有通訊協定鍵值、UI 參數、緩衝區大小等
 */
#pragma once

#include <pebble.h>

// ============================================================
// 離線測試功能 (Mock Mode)
// ============================================================
// 設為 1 可在未連線時自動載入測試文字以供預覽
#define ENABLE_MOCK_MODE 1

// ============================================================
// 通訊協定鍵值 (與 Android 端一致)
// ============================================================
#define KEY_COMMAND        0   // (Int) 指令類型
#define KEY_TEXT_OFFSET    1   // (Int) 字元偏移量
#define KEY_TEXT_CHUNK     2   // (String) 文字內容
#define KEY_SCROLL_SPEED   3   // (Int) 捲動速度 1-6
#define KEY_TEXT_SIZE      4   // (Int) 文字大小 0=小, 1=中, 2=大

// ============================================================
// 指令類型
// ============================================================
#define CMD_REQUEST_TEXT    0  // 手錶向手機請求文字區塊
#define CMD_SEND_TEXT      1  // 手機回傳文字區塊
#define CMD_SYNC_SETTINGS  2  // 同步設定 (速度/字型大小)

// ============================================================
// 文字大小列舉
// ============================================================
typedef enum {
    TEXT_SIZE_SMALL  = 0,
    TEXT_SIZE_MEDIUM = 1,
    TEXT_SIZE_LARGE  = 2
} TextSizeLevel;

// ============================================================
// 環形緩衝區設定
// ============================================================
// 注意：實際記憶體使用量 ≈ RING_BUFFER_CAPACITY × 2，因為
// ring buffer 節點持有文字，且 refresh_flat_text() 會產生一份
// 連續的扁平化副本供繪圖引擎使用。因此下方容量已預留此開銷。
//
// Aplite (Pebble Original) 記憶體極度有限（堆積約 24KB）
#if defined(PBL_PLATFORM_APLITE)
  #define RING_BUFFER_CAPACITY     1536   // 節點記憶體上限 (實際佔用 ≈ 3KB 含副本)
  #define CHUNK_SIZE               200    // 每次請求的文字區塊大小
  #define PREFETCH_SCREEN_COUNT    2      // 預載 2 個螢幕高度
#elif defined(PBL_PLATFORM_CHALK) || defined(PBL_PLATFORM_GABBRO)
  // 圓形螢幕可用面積較小，但記憶體較充裕
  #define RING_BUFFER_CAPACITY     4096   // 節點記憶體上限 (實際佔用 ≈ 8KB 含副本)
  #define CHUNK_SIZE               512    // 每次請求的文字區塊大小
  #define PREFETCH_SCREEN_COUNT    3      // 預載 3 個螢幕高度
#else
  // Basalt, Diorite, Emery
  #define RING_BUFFER_CAPACITY     5120   // 節點記憶體上限 (實際佔用 ≈ 10KB 含副本)
  #define CHUNK_SIZE               512    // 每次請求的文字區塊大小
  #define PREFETCH_SCREEN_COUNT    3      // 預載 3 個螢幕高度
#endif

// AppMessage 收發緩衝區大小
#define INBOX_SIZE   2048
#define OUTBOX_SIZE  256

// ============================================================
// 捲動速度相關
// ============================================================
#define SCROLL_SPEED_MIN       1
#define SCROLL_SPEED_MAX       6
#define SCROLL_SPEED_DEFAULT   3

// 自動捲動計時器間隔 (毫秒) - 依速度等級對應
// 等級 1 最慢，等級 6 最快
// 定義於 main.c，此處僅宣告
extern const uint16_t SCROLL_INTERVALS_MS[7];

// 每次捲動的像素數
#define SCROLL_STEP_PX         1

// 手動捲動後暫停自動捲動的時間 (毫秒)
#define MANUAL_SCROLL_PAUSE_MS 2000

// 手動捲動每次跳動的像素數
#define MANUAL_SCROLL_SHORT_PX 60
#define MANUAL_SCROLL_LONG_PX  120

// ============================================================
// 圓形螢幕邊距 (Chalk / Gabbro)
// ============================================================
#if defined(PBL_ROUND)
  #define ROUND_HORIZONTAL_INSET 18
  #define ROUND_VERTICAL_INSET   10
#else
  #define ROUND_HORIZONTAL_INSET 0
  #define ROUND_VERTICAL_INSET   0
#endif

// 一般螢幕邊距
#define TEXT_MARGIN_H  4
#define TEXT_MARGIN_V  2

// ============================================================
// 持久化儲存鍵
// ============================================================
#define PERSIST_KEY_SCROLL_SPEED  100
#define PERSIST_KEY_TEXT_SIZE     101
