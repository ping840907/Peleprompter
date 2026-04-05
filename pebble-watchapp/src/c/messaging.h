/**
 * messaging.h - AppMessage 通訊模組
 *
 * 處理手錶與 Android 手機之間的雙向通訊。
 * 包含文字區塊請求、設定同步等功能。
 */
#pragma once

#include <pebble.h>
#include "constants.h"
#include "ring_buffer.h"

// ============================================================
// 回呼函式型態定義
// ============================================================

/** 收到文字區塊時的回呼 */
typedef void (*TextChunkReceivedCallback)(int32_t offset, const char *text, int32_t len);

/** 收到設定同步時的回呼 */
typedef void (*SettingsSyncCallback)(int32_t scroll_speed, int32_t text_size);

/** 連線狀態改變時的回呼 */
typedef void (*ConnectionChangeCallback)(bool connected);

// ============================================================
// API
// ============================================================

/**
 * 初始化 AppMessage 通訊
 * @param text_cb     收到文字區塊的回呼
 * @param settings_cb 收到設定同步的回呼
 * @param conn_cb     連線狀態改變的回呼
 */
void messaging_init(TextChunkReceivedCallback text_cb,
                    SettingsSyncCallback settings_cb,
                    ConnectionChangeCallback conn_cb);

/** 反初始化 AppMessage */
void messaging_deinit(void);

/**
 * 向手機請求文字區塊
 * @param offset 要請求的字元偏移量
 */
void messaging_request_text(int32_t offset);

/**
 * 向手機同步目前的設定
 * @param scroll_speed 捲動速度 1-6
 * @param text_size    文字大小 0-2
 */
void messaging_send_settings(int32_t scroll_speed, int32_t text_size);

/** 取得目前的藍牙連線狀態 */
bool messaging_is_connected(void);
