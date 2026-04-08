/**
 * messaging.h - AppMessage 通訊模組
 *
 * 處理手錶與 Android 手機之間的雙向通訊。
 * 圖片模式：手錶發送頁面請求，手機回傳圖片區塊。
 */
#pragma once

#include <pebble.h>
#include "constants.h"

// ============================================================
// 回呼函式型態定義
// ============================================================

/**
 * 收到 CMD_INIT_IMAGES 時的回呼
 * @param total_pages 文件總頁數
 * @param page_width  頁面寬度 (pixels)
 * @param page_height 頁面高度 (pixels)
 */
typedef void (*ImagesInitCallback)(int32_t total_pages,
                                    int32_t page_width,
                                    int32_t page_height);

/**
 * 收到圖片資料區塊時的回呼
 * @param page_num     目標頁碼
 * @param chunk_idx    此區塊索引
 * @param total_chunks 總區塊數
 * @param data         原始位元組資料
 * @param data_len     資料長度
 */
typedef void (*ImageChunkCallback)(int32_t page_num,
                                    int32_t chunk_idx,
                                    int32_t total_chunks,
                                    const uint8_t *data,
                                    int32_t data_len);

/** 收到設定同步時的回呼 */
typedef void (*SettingsSyncCallback)(int32_t scroll_speed, int32_t text_size);

/** 連線狀態改變時的回呼 */
typedef void (*ConnectionChangeCallback)(bool connected);

// ============================================================
// API
// ============================================================

/**
 * 初始化 AppMessage 通訊
 * @param init_cb     收到圖片初始化訊息的回呼
 * @param chunk_cb    收到圖片區塊的回呼
 * @param settings_cb 收到設定同步的回呼
 * @param conn_cb     連線狀態改變的回呼
 */
void messaging_init(ImagesInitCallback init_cb,
                    ImageChunkCallback chunk_cb,
                    SettingsSyncCallback settings_cb,
                    ConnectionChangeCallback conn_cb);

/** 反初始化 AppMessage */
void messaging_deinit(void);

/**
 * 向手機發送初始化請求（包含螢幕尺寸與文字大小偏好）
 * 手機收到後將回傳 CMD_INIT_IMAGES
 * @param watch_width  手錶螢幕寬度
 * @param watch_height 手錶螢幕高度
 * @param text_size    目前文字大小設定 (TextSizeLevel)
 */
void messaging_request_init(int32_t watch_width,
                             int32_t watch_height,
                             int32_t text_size);

/**
 * 向手機請求指定頁面的圖片資料
 * @param page_num 目標頁碼 (0-indexed)
 */
void messaging_request_page(int32_t page_num);

/**
 * 向手機同步目前的設定
 * @param scroll_speed 捲動速度 1-6
 * @param text_size    文字大小 0-2
 */
void messaging_send_settings(int32_t scroll_speed, int32_t text_size);

/** 取得目前的藍牙連線狀態 */
bool messaging_is_connected(void);
