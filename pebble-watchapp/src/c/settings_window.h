/**
 * settings_window.h - 設定視窗
 *
 * 允許使用者調整捲動速度 (1-6)、文字大小 (小/中/大)、頂部時間欄顯示，
 * 以及手動跳頁功能。
 */
#pragma once

#include <pebble.h>
#include "constants.h"

// ============================================================
// 設定變更回呼
// ============================================================
typedef void (*SettingsChangedCallback)(int32_t scroll_speed,
                                        TextSizeLevel text_size,
                                        bool show_status_bar);

/**
 * 跳頁回呼
 * @param page_num  目標頁碼 (0-indexed)
 */
typedef void (*JumpToPageCallback)(int32_t page_num);

// ============================================================
// API
// ============================================================

/**
 * 推入設定視窗到視窗堆疊
 * @param speed           目前的捲動速度
 * @param size            目前的文字大小
 * @param show_status_bar 目前的頂部時間欄狀態
 * @param current_page    目前頁碼 (1-indexed, 0 表示尚未載入)
 * @param total_pages     文件總頁數 (0 表示尚未載入)
 * @param changed_cb      設定變更時的回呼
 * @param jump_cb         跳頁確認時的回呼
 */
void settings_window_push(int32_t speed, TextSizeLevel size,
                          bool show_status_bar,
                          int32_t current_page,
                          int32_t total_pages,
                          SettingsChangedCallback changed_cb,
                          JumpToPageCallback jump_cb);
