/**
 * settings_window.h - 設定視窗
 *
 * 允許使用者調整捲動速度 (1-6) 和文字大小 (小/中/大)。
 */
#pragma once

#include <pebble.h>
#include "constants.h"

// ============================================================
// 設定變更回呼
// ============================================================
typedef void (*SettingsChangedCallback)(int32_t scroll_speed, TextSizeLevel text_size);

// ============================================================
// API
// ============================================================

/**
 * 推入設定視窗到視窗堆疊
 * @param speed    目前的捲動速度
 * @param size     目前的文字大小
 * @param callback 設定變更時的回呼
 */
void settings_window_push(int32_t speed, TextSizeLevel size,
                          SettingsChangedCallback callback);
