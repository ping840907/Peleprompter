/**
 * settings_window.h - 設定視窗
 *
 * 允許使用者調整捲動速度 (1-6)、文字大小 (小/中/大)、頂部時間欄顯示，
 * 以及手動跳頁（按頁碼 / 按百分比）功能。
 */
#pragma once

#include <pebble.h>
#include "constants.h"

// ============================================================
// 回呼型別
// ============================================================
typedef void (*SettingsChangedCallback)(int32_t scroll_speed,
                                        TextSizeLevel text_size,
                                        bool show_status_bar);

/** 跳頁回呼：page_num 為 0-indexed */
typedef void (*JumpToPageCallback)(int32_t page_num);

/** 跳轉百分比回呼：percent 範圍 0-100 */
typedef void (*JumpToPercentCallback)(int32_t percent);

// ============================================================
// API
// ============================================================

/**
 * 推入設定視窗到視窗堆疊
 * @param speed           目前的捲動速度
 * @param size            目前的文字大小
 * @param show_status_bar 目前的頂部時間欄狀態
 * @param current_page    目前頁碼 (1-indexed, 0=尚未載入)
 * @param total_pages     文件總頁數 (0=尚未載入)
 * @param current_percent 目前進度百分比 (0-100)
 * @param changed_cb      設定變更時的回呼
 * @param jump_page_cb    按頁碼跳轉確認時的回呼
 * @param jump_pct_cb     按百分比跳轉確認時的回呼
 */
void settings_window_push(int32_t speed, TextSizeLevel size,
                          bool show_status_bar,
                          int32_t current_page,
                          int32_t total_pages,
                          int32_t current_percent,
                          SettingsChangedCallback changed_cb,
                          JumpToPageCallback jump_page_cb,
                          JumpToPercentCallback jump_pct_cb);
