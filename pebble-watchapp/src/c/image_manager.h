/**
 * image_manager.h - 手錶端圖片頁面快取管理員
 *
 * 管理從 Android 接收的 1-bit 圖片頁面。
 * 每頁尺寸對應手錶螢幕解析度；資料以 GBitmapFormat1Bit 格式儲存。
 * 採用小型環形快取，依捲動位置淘汰遠端頁面以節省記憶體。
 */
#pragma once

#include <pebble.h>
#include "constants.h"

// ============================================================
// 頁面插槽結構
// ============================================================
typedef struct {
    int32_t  page_num;        // 此插槽持有的頁碼；-1 表示空置
    GBitmap *bitmap;          // 已分配的 GBitmap（含像素資料）
    bool     is_ready;        // 所有區塊均已收到，可以繪製
    bool     is_loading;      // 正在接收區塊中
    int32_t  chunks_received; // 已收到的區塊數
    int32_t  total_chunks;    // 預期的總區塊數
} PageSlot;

// ============================================================
// 圖片管理員結構
// ============================================================
typedef struct {
    PageSlot slots[MAX_PAGES_CACHED];
    int32_t  total_pages;  // 文件總頁數 (由 CMD_INIT_IMAGES 設定)
    int32_t  page_width;   // 每頁寬度 (pixels)
    int32_t  page_height;  // 每頁高度 (pixels)
    int32_t  row_stride;   // 每行位元組數 = ceil(page_width / 8)
    int32_t  page_data_size; // 每頁原始資料大小 = row_stride × page_height
} ImageManager;

// ============================================================
// API
// ============================================================

/** 初始化管理員（所有插槽清空） */
void image_manager_init(ImageManager *mgr);

/** 釋放所有分配的 GBitmap 記憶體 */
void image_manager_deinit(ImageManager *mgr);

/**
 * 設定文件維度（收到 CMD_INIT_IMAGES 後呼叫）
 * 同時重置所有快取頁面。
 */
void image_manager_set_dimensions(ImageManager *mgr,
                                   int32_t total_pages,
                                   int32_t page_width,
                                   int32_t page_height);

/**
 * 取得指定頁碼的插槽（若已快取）
 * @return 插槽指標，若未快取則回傳 NULL
 */
PageSlot *image_manager_get_slot(ImageManager *mgr, int32_t page_num);

/**
 * 為指定頁碼分配插槽（淘汰最遠的舊頁以騰出空間）
 *
 * 淘汰時會略過位於 [protect_lo, protect_hi] 範圍內的頁面，
 * 避免把正在畫面上顯示的頁面踢出快取造成閃爍。
 * 若無空插槽且所有已佔用插槽都在保護範圍內，回傳 NULL。
 *
 * @param protect_lo 受保護頁碼下界（含），傳負值代表不保護
 * @param protect_hi 受保護頁碼上界（含）
 * @return 已清空、可接收資料的插槽指標；失敗回傳 NULL
 */
PageSlot *image_manager_alloc_slot(ImageManager *mgr, int32_t page_num,
                                   int32_t protect_lo, int32_t protect_hi);

/**
 * 接收一個圖片資料區塊並寫入對應插槽
 * 當所有區塊均收到時，自動標記 is_ready = true
 *
 * @param page_num    目標頁碼
 * @param chunk_idx   此區塊的索引
 * @param total_chunks 總區塊數
 * @param data        區塊原始位元組
 * @param data_len    區塊位元組數
 * @return true 若此呼叫後該頁已完整（is_ready）
 */
bool image_manager_receive_chunk(ImageManager *mgr,
                                  int32_t page_num,
                                  int32_t chunk_idx,
                                  int32_t total_chunks,
                                  const uint8_t *data,
                                  int32_t data_len);

/** 強制淘汰指定頁面（釋放 GBitmap 並清空插槽） */
void image_manager_evict(ImageManager *mgr, int32_t page_num);

/** 查詢指定頁面是否已完整且可繪製 */
bool image_manager_is_ready(ImageManager *mgr, int32_t page_num);

/** 查詢指定頁面是否正在載入中 */
bool image_manager_is_loading(ImageManager *mgr, int32_t page_num);

/**
 * 取得指定頁面的 GBitmap（僅在 is_ready 後有效）
 * 呼叫者不可釋放此 bitmap。
 */
GBitmap *image_manager_get_bitmap(ImageManager *mgr, int32_t page_num);
