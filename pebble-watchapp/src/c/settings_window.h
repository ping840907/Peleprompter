/**
 * settings_window.h - 設定視窗
 *
 * 允許使用者調整捲動速度 (1-6)、文字大小 (小/中/大) 與頂部時間欄顯示。
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

// ============================================================
// API
// ============================================================

/**
 * 推入設定視窗到視窗堆疊
 * @param speed           目前的捲動速度
 * @param size            目前的文字大小
 * @param show_status_bar 目前的頂部時間欄狀態
 * @param callback        設定變更時的回呼
 */
void settings_window_push(int32_t speed, TextSizeLevel size,
                          bool show_status_bar,
                          SettingsChangedCallback callback);
