/**
 * main.c - Peleprompter 主程式
 *
 * 整合環形緩衝區、自動捲動、手動捲動、AppMessage 通訊、
 * 以及斷線優雅降級等所有核心功能。
 */

#include <pebble.h>
#include "constants.h"
#include "ring_buffer.h"
#include "messaging.h"
#include "settings_window.h"

// 捲動速度對應的計時器間隔 (毫秒)，定義於此處避免在每個 .c 中重複
const uint16_t SCROLL_INTERVALS_MS[7] = {
    0,    // 佔位 (索引 0 未使用)
    120,  // 等級 1: 慢
    90,   // 等級 2
    65,   // 等級 3: 預設
    45,   // 等級 4
    30,   // 等級 5
    18    // 等級 6: 快
};

// ============================================================
// 全域狀態
// ============================================================

// UI 元件
static Window    *s_main_window = NULL;
static Layer     *s_canvas_layer = NULL;     // 自訂繪製圖層
static TextLayer *s_status_layer = NULL;     // 狀態列 (載入中/斷線等)

// 環形緩衝區
static RingBuffer s_ring_buffer;

// 扁平化的顯示文字 (由環形緩衝區產生)
static char *s_flat_text = NULL;
static int32_t s_flat_text_len = 0;

// 捲動狀態
static int32_t s_scroll_offset_px = 0;       // 目前的垂直捲動偏移 (像素)
static int32_t s_total_content_height = 0;   // 目前文字的總高度 (像素)
static bool    s_auto_scroll_active = false;  // 自動捲動是否啟動
static AppTimer *s_scroll_timer = NULL;       // 自動捲動計時器

// 手動捲動動畫狀態
static AppTimer *s_manual_anim_timer = NULL;
static int32_t s_target_scroll_offset_px = -1;  // 目標捲動位置 (-1 表示未在動畫中)

// 手動捲動暫停
static AppTimer *s_manual_pause_timer = NULL;
static bool s_manual_pause_active = false;

// 設定
static int32_t       s_scroll_speed = SCROLL_SPEED_DEFAULT;
static TextSizeLevel s_text_size = TEXT_SIZE_MEDIUM;

// 連線狀態
static bool s_bt_connected = true;
static bool s_show_disconnect_warning = false;

// 是否正在等待文字區塊
static bool s_waiting_for_chunk = false;

// 緩衝區用盡暫停 (buffer underrun)
static bool s_buffer_underrun = false;

// 字型快取
static GFont s_current_font = NULL;

// ============================================================
// 輔助函式：字型管理
// ============================================================

/** 根據文字大小設定取得對應的系統字型 */
static GFont get_font_for_size(TextSizeLevel size) {
    switch (size) {
        case TEXT_SIZE_SMALL:
            return fonts_get_system_font(FONT_KEY_GOTHIC_18);
        case TEXT_SIZE_LARGE:
            return fonts_get_system_font(FONT_KEY_GOTHIC_28);
        case TEXT_SIZE_MEDIUM:
        default:
            return fonts_get_system_font(FONT_KEY_GOTHIC_24);
    }
}

/** 計算目前視窗可用的文字顯示區域 */
static GRect get_text_bounds(void) {
    GRect bounds = layer_get_bounds(s_canvas_layer);

    // 套用邊距
    int h_margin = TEXT_MARGIN_H + ROUND_HORIZONTAL_INSET;
    int v_margin = TEXT_MARGIN_V + ROUND_VERTICAL_INSET;

    bounds.origin.x += h_margin;
    bounds.origin.y += v_margin;
    bounds.size.w -= 2 * h_margin;
    bounds.size.h -= 2 * v_margin;

    return bounds;
}

// ============================================================
// 文字渲染與高度計算
// ============================================================

/** 計算給定文字在指定寬度下的總繪製高度 */
static int32_t calculate_text_height(const char *text, GFont font, int width) {
    if (!text || width <= 0) return 0;

    GSize size = graphics_text_layout_get_content_size(
        text,
        font,
        GRect(0, 0, width, 32000),  // 給一個極大的高度讓它自由排版
        GTextOverflowModeWordWrap,
        GTextAlignmentLeft
    );
    return size.h;
}

/** 更新扁平化文字並重新計算高度 */
static void refresh_flat_text(void) {
    if (s_flat_text) {
        free(s_flat_text);
        s_flat_text = NULL;
        s_flat_text_len = 0;
    }

    s_flat_text = ring_buffer_flatten(&s_ring_buffer);
    if (s_flat_text) {
        s_flat_text_len = strlen(s_flat_text);
        GRect text_bounds = get_text_bounds();
        s_total_content_height = calculate_text_height(
            s_flat_text, s_current_font, text_bounds.size.w
        );
    } else {
        s_total_content_height = 0;
    }
}

// ============================================================
// 預取邏輯
// ============================================================

/** 檢查是否需要向手機預取更多文字 */
static void check_prefetch(void) {
    if (s_waiting_for_chunk) return;  // 已有請求在途
    if (!s_canvas_layer) return;     // 視窗尚未載入

    GRect bounds = layer_get_bounds(s_canvas_layer);
    int screen_height = bounds.size.h;

    // 向下預取：當接近緩衝區底部時
    int remaining_below = s_total_content_height - s_scroll_offset_px - screen_height;
    if (remaining_below < screen_height * PREFETCH_SCREEN_COUNT
        && !s_ring_buffer.reached_end) {
        int32_t next_offset = ring_buffer_get_tail_end_offset(&s_ring_buffer);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "預取向下: offset=%ld", (long)next_offset);
        messaging_request_text(next_offset);
        s_waiting_for_chunk = true;
        return;  // 一次只發一個請求，避免衝突
    }

    // 向上預取：當接近緩衝區頂部時
    if (s_scroll_offset_px < screen_height * 1
        && !s_ring_buffer.reached_start) {
        int32_t head_offset = ring_buffer_get_head_offset(&s_ring_buffer);
        if (head_offset > 0) {
            int32_t prev_offset = head_offset - CHUNK_SIZE;
            if (prev_offset < 0) prev_offset = 0;
            APP_LOG(APP_LOG_LEVEL_DEBUG, "預取向上: offset=%ld", (long)prev_offset);
            messaging_request_text(prev_offset);
            s_waiting_for_chunk = true;
        }
    }
}

// ============================================================
// 繪製回呼
// ============================================================

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    GRect text_bounds = get_text_bounds();

    // 清除背景
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    if (s_flat_text && s_flat_text_len > 0) {
        // 繪製文字 (使用負的 y 偏移來實現捲動)
        graphics_context_set_text_color(ctx, GColorWhite);
        GRect draw_rect = GRect(
            text_bounds.origin.x,
            text_bounds.origin.y - s_scroll_offset_px,
            text_bounds.size.w,
            s_total_content_height + bounds.size.h  // 額外空間確保能完整顯示
        );
        graphics_draw_text(ctx, s_flat_text, s_current_font, draw_rect,
                           GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    }

    // 繪製斷線警告 (僅在緩衝區用盡時顯示)
    if (s_show_disconnect_warning) {
        GRect warn_rect = GRect(0, bounds.size.h - 30, bounds.size.w, 30);

        #ifdef PBL_COLOR
        // 彩色平台：深灰底 + 白字
        graphics_context_set_fill_color(ctx, GColorDarkGray);
        graphics_fill_rect(ctx, warn_rect, 0, GCornerNone);
        graphics_context_set_text_color(ctx, GColorWhite);
        #else
        // Aplite 黑白平台：白底 + 黑字 (反轉顯示)
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_rect(ctx, warn_rect, 0, GCornerNone);
        graphics_context_set_text_color(ctx, GColorBlack);
        #endif

        GFont small_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
        graphics_draw_text(ctx, "Disconnected",
                           small_font, warn_rect,
                           GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentCenter, NULL);
    }
}

// ============================================================
// 自動捲動
// ============================================================

static void auto_scroll_tick(void *data);

static void schedule_auto_scroll(void) {
    if (s_scroll_timer) {
        app_timer_cancel(s_scroll_timer);
        s_scroll_timer = NULL;
    }
    if (s_auto_scroll_active && !s_manual_pause_active) {
        uint16_t interval = SCROLL_INTERVALS_MS[s_scroll_speed];
        s_scroll_timer = app_timer_register(interval, auto_scroll_tick, NULL);
    }
}

static void auto_scroll_tick(void *data) {
    s_scroll_timer = NULL;

    if (!s_auto_scroll_active || s_manual_pause_active) return;

    // 計算最大可捲動偏移量 (依據文字區塊的實際高度)
    GRect text_bounds = get_text_bounds();
    int32_t max_offset = s_total_content_height - text_bounds.size.h;
    if (max_offset < 0) max_offset = 0;

    if (s_scroll_offset_px < max_offset) {
        s_scroll_offset_px += SCROLL_STEP_PX;
        if (s_scroll_offset_px > max_offset) {
            s_scroll_offset_px = max_offset;
        }

        // 當自動捲動同時加上手動目標也在變化時，讓目標同步往前移，避免向下手動動畫被自動捲動拉扯
        if (s_target_scroll_offset_px >= 0) {
            s_target_scroll_offset_px += SCROLL_STEP_PX;
            if (s_target_scroll_offset_px > max_offset) {
                s_target_scroll_offset_px = max_offset;
            }
        }

        layer_mark_dirty(s_canvas_layer);
        check_prefetch();
        schedule_auto_scroll();  // 繼續排程下一次捲動
    } else {
        // 已到底部
        if (!s_ring_buffer.reached_end) {
            // 還有更多文字，但緩衝區用盡 → 顯示載入狀態
            s_buffer_underrun = true;
            text_layer_set_text(s_status_layer, "Loading...");
            layer_set_hidden(text_layer_get_layer(s_status_layer), false);
            check_prefetch();

            if (!s_bt_connected) {
#if ENABLE_MOCK_MODE
                // Mock Mode 啟用時，不顯示斷線警告
#else
                // 離線且緩衝區已到底 → 顯示斷線警告
                s_show_disconnect_warning = true;
                layer_mark_dirty(s_canvas_layer);
#endif
            }
            // 不排程下一次捲動，等收到新區塊後由 on_text_chunk_received 恢復
        }
        // 若已到達原文結尾，也不再排程
    }
}

static void start_auto_scroll(void) {
    s_auto_scroll_active = true;
    s_manual_pause_active = false;
    
    // 開啟自動捲動後，立刻取消所有等待恢復的暫停計時器
    if (s_manual_pause_timer) {
        app_timer_cancel(s_manual_pause_timer);
        s_manual_pause_timer = NULL;
    }
    
    schedule_auto_scroll();
}

static void stop_auto_scroll(void) {
    s_auto_scroll_active = false;
    if (s_scroll_timer) {
        app_timer_cancel(s_scroll_timer);
        s_scroll_timer = NULL;
    }
}

// ============================================================
// 手動捲動暫停恢復
// ============================================================

static void manual_pause_expired(void *data) {
    s_manual_pause_timer = NULL;
    s_manual_pause_active = false;

    // 如果自動捲動原本是啟動的，恢復
    if (s_auto_scroll_active) {
        schedule_auto_scroll();
    }
}

/** 手動捲動時暫停自動捲動 2 秒 */
static void trigger_manual_pause(void) {
    if (s_auto_scroll_active) {
        s_manual_pause_active = true;
        // 取消目前的自動捲動計時器
        if (s_scroll_timer) {
            app_timer_cancel(s_scroll_timer);
            s_scroll_timer = NULL;
        }
        // 設定暫停計時器
        if (s_manual_pause_timer) {
            app_timer_cancel(s_manual_pause_timer);
        }
        s_manual_pause_timer = app_timer_register(
            MANUAL_SCROLL_PAUSE_MS, manual_pause_expired, NULL
        );
    }
}

#define MANUAL_ANIMATION_STEP_MS 33

static void manual_anim_tick(void *data) {
    s_manual_anim_timer = NULL;

    if (s_target_scroll_offset_px < 0) return;

    if (s_scroll_offset_px != s_target_scroll_offset_px) {
        // 使用 easing 讓動畫平滑：每次移動剩餘距離的一半
        int32_t diff = s_target_scroll_offset_px - s_scroll_offset_px;
        int32_t step = diff / 2;
        if (step == 0) {
            step = (diff > 0) ? 1 : -1;
        }

        s_scroll_offset_px += step;
        layer_mark_dirty(s_canvas_layer);

        // 繼續觸發下一幀動畫
        s_manual_anim_timer = app_timer_register(MANUAL_ANIMATION_STEP_MS, manual_anim_tick, NULL);
    } else {
        // 到達目標位置
        s_target_scroll_offset_px = -1;
        check_prefetch();
    }
}

/** 執行手動捲動 */
static void do_manual_scroll(int32_t delta_px) {
    GRect text_bounds = get_text_bounds();
    int32_t max_offset = s_total_content_height - text_bounds.size.h;
    if (max_offset < 0) max_offset = 0;

    // 初始化目標位置 (如果當前沒有動畫在進行)
    if (s_target_scroll_offset_px < 0) {
        s_target_scroll_offset_px = s_scroll_offset_px;
    }

    s_target_scroll_offset_px += delta_px;
    if (s_target_scroll_offset_px < 0) s_target_scroll_offset_px = 0;
    if (s_target_scroll_offset_px > max_offset) s_target_scroll_offset_px = max_offset;

    // 只有在向上捲動時暫停自動捲動，向下與自動捲動同向則不打斷閱讀流暢度
    if (delta_px < 0) {
        trigger_manual_pause();
    }

    // 啟動動畫計時器
    if (!s_manual_anim_timer) {
        s_manual_anim_timer = app_timer_register(MANUAL_ANIMATION_STEP_MS, manual_anim_tick, NULL);
    }
}

// ============================================================
// 按鈕處理
// ============================================================

static void up_short_click(ClickRecognizerRef recognizer, void *context) {
    do_manual_scroll(-MANUAL_SCROLL_SHORT_PX);
}

static void up_long_click(ClickRecognizerRef recognizer, void *context) {
    do_manual_scroll(-MANUAL_SCROLL_LONG_PX);
}

static void down_short_click(ClickRecognizerRef recognizer, void *context) {
    do_manual_scroll(MANUAL_SCROLL_SHORT_PX);
}

static void down_long_click(ClickRecognizerRef recognizer, void *context) {
    do_manual_scroll(MANUAL_SCROLL_LONG_PX);
}

static void select_short_click(ClickRecognizerRef recognizer, void *context) {
    // 播放/暫停自動捲動
    // 如果目前是啟動狀態且沒有被手動捲動卡住暫停，才算是真的「播放中」，此時按鈕為暫停；
    // 反之 (包含已手動暫停卡住時) 一律直接切換為無條件啟動。
    if (s_auto_scroll_active && !s_manual_pause_active) {
        stop_auto_scroll();
    } else {
        start_auto_scroll();
    }
}

/** 設定變更回呼 (從設定視窗回傳) */
static void on_settings_changed(int32_t speed, TextSizeLevel size) {
    bool font_changed = (size != s_text_size);
    s_scroll_speed = speed;
    s_text_size = size;

    // 更新字型
    if (font_changed) {
        s_current_font = get_font_for_size(s_text_size);
        refresh_flat_text();  // 重新計算高度
        s_scroll_offset_px = 0;  // 字型改變後重置捲動位置
        
        if (s_manual_anim_timer) {
            app_timer_cancel(s_manual_anim_timer);
            s_manual_anim_timer = NULL;
        }
        s_target_scroll_offset_px = -1;
    }

    // 如果自動捲動中，以新速度重新排程
    if (s_auto_scroll_active && !s_manual_pause_active) {
        schedule_auto_scroll();
    }

    // 持久化設定
    persist_write_int(PERSIST_KEY_SCROLL_SPEED, s_scroll_speed);
    persist_write_int(PERSIST_KEY_TEXT_SIZE, (int32_t)s_text_size);

    // 同步設定到手機
    messaging_send_settings(s_scroll_speed, (int32_t)s_text_size);
}

static void select_long_click(ClickRecognizerRef recognizer, void *context) {
    // 開啟設定視窗
    settings_window_push(s_scroll_speed, s_text_size, on_settings_changed);
}

static void click_config_provider(void *context) {
    window_single_click_subscribe(BUTTON_ID_UP, up_short_click);
    window_long_click_subscribe(BUTTON_ID_UP, 300, up_long_click, NULL);

    window_single_click_subscribe(BUTTON_ID_DOWN, down_short_click);
    window_long_click_subscribe(BUTTON_ID_DOWN, 300, down_long_click, NULL);

    window_single_click_subscribe(BUTTON_ID_SELECT, select_short_click);
    window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_click, NULL);
}

// ============================================================
// 通訊回呼
// ============================================================

/** 收到文字區塊 */
static void on_text_chunk_received(int32_t offset, const char *text, int32_t len) {
    s_waiting_for_chunk = false;

    if (len == 0) {
        // 空區塊表示已到達原文結尾
        s_ring_buffer.reached_end = true;
        APP_LOG(APP_LOG_LEVEL_INFO, "已到達文字結尾");
        return;
    }

    if (offset == 0) {
        s_ring_buffer.reached_start = true;
    }

    // 判斷此區塊應追加到尾部還是插入到頭部
    int32_t tail_end = ring_buffer_get_tail_end_offset(&s_ring_buffer);
    int32_t head_start = ring_buffer_get_head_offset(&s_ring_buffer);

    // 記住捲動前的內容高度（用於向上插入時調整偏移）
    int32_t old_height = s_total_content_height;

    if (s_ring_buffer.count == 0) {
        // 緩衝區為空，直接追加
        ring_buffer_append(&s_ring_buffer, offset, text, len);
    } else if (offset >= tail_end) {
        // 追加到尾部 (向下載入)
        ring_buffer_append(&s_ring_buffer, offset, text, len);
    } else if (head_start >= 0 && offset < head_start) {
        // 插入到頭部 (向上載入)
        // 檢查新區塊是否與既有頭部重疊
        int32_t new_end = offset + len;
        if (new_end > head_start) {
            // 重疊了，切除重疊部分
            int32_t overlapping_len = new_end - head_start;
            int32_t safe_len = len - overlapping_len;
            if (safe_len > 0) {
                ring_buffer_prepend(&s_ring_buffer, offset, text, safe_len);
            } else {
                APP_LOG(APP_LOG_LEVEL_DEBUG, "區塊完全重疊，忽略: offset=%ld", (long)offset);
                return;
            }
        } else {
            ring_buffer_prepend(&s_ring_buffer, offset, text, len);
        }
    } else {
        // 重複或重疊的區塊，忽略
        APP_LOG(APP_LOG_LEVEL_DEBUG, "忽略重複區塊 offset=%ld", (long)offset);
        return;
    }

    // 重建扁平化文字
    refresh_flat_text();

    // 如果是向上插入，調整捲動偏移以避免跳動
    if (head_start >= 0 && offset < head_start) {
        int32_t height_delta = s_total_content_height - old_height;
        s_scroll_offset_px += height_delta;
        
        if (s_target_scroll_offset_px >= 0) {
            s_target_scroll_offset_px += height_delta;
        }
    }

    // 隱藏斷線警告 (收到新文字了)
    s_show_disconnect_warning = false;

    // 清除緩衝區用盡狀態並隱藏狀態列
    if (s_buffer_underrun) {
        s_buffer_underrun = false;
    }
    layer_set_hidden(text_layer_get_layer(s_status_layer), true);

    layer_mark_dirty(s_canvas_layer);
    check_prefetch();

    // 如果自動捲動因 buffer underrun 而停滯，重新啟動排程
    if (s_auto_scroll_active && !s_manual_pause_active && !s_scroll_timer) {
        schedule_auto_scroll();
    }
}

/** 收到設定同步 (從手機推送) */
static void on_settings_synced(int32_t scroll_speed, int32_t text_size) {
    if (scroll_speed >= SCROLL_SPEED_MIN && scroll_speed <= SCROLL_SPEED_MAX) {
        s_scroll_speed = scroll_speed;
        persist_write_int(PERSIST_KEY_SCROLL_SPEED, s_scroll_speed);
    }

    if (text_size >= TEXT_SIZE_SMALL && text_size <= TEXT_SIZE_LARGE) {
        bool font_changed = ((TextSizeLevel)text_size != s_text_size);
        s_text_size = (TextSizeLevel)text_size;
        persist_write_int(PERSIST_KEY_TEXT_SIZE, (int32_t)s_text_size);

        if (font_changed) {
            s_current_font = get_font_for_size(s_text_size);
            refresh_flat_text();
            layer_mark_dirty(s_canvas_layer);
        }
    }

    if (s_auto_scroll_active && !s_manual_pause_active) {
        schedule_auto_scroll();
    }

    APP_LOG(APP_LOG_LEVEL_INFO, "設定已同步: speed=%ld, size=%ld",
            (long)s_scroll_speed, (long)s_text_size);
}

/** 藍牙連線狀態改變 */
static void on_connection_changed(bool connected) {
    s_bt_connected = connected;

    if (connected) {
        // 重新連線，隱藏警告
        s_show_disconnect_warning = false;
        layer_mark_dirty(s_canvas_layer);

        // 如果緩衝區不完整，繼續預取
        check_prefetch();
    }
#if ENABLE_MOCK_MODE
    else {
        // Mock 模式下，斷線也能載入假資料，因此嘗試預取
        check_prefetch();
    }
#endif
    // 斷線時不立刻警告，允許繼續閱讀緩衝區中的文字
    // 只有在捲動到緩衝區盡頭時才顯示警告 (在 auto_scroll_tick 中處理)
}

// ============================================================
// 主視窗生命週期
// ============================================================

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    // 建立自訂繪製圖層 (全螢幕)
    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(window_layer, s_canvas_layer);

    // 建立狀態列 (初始隱藏)
    s_status_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 15,
                                              bounds.size.w, 30));
    text_layer_set_background_color(s_status_layer, GColorClear);
    text_layer_set_text_color(s_status_layer, GColorWhite);
    text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
    text_layer_set_font(s_status_layer,
                        fonts_get_system_font(FONT_KEY_GOTHIC_18));
    layer_set_hidden(text_layer_get_layer(s_status_layer), true);
    layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

    // 載入字型
    s_current_font = get_font_for_size(s_text_size);

    // 顯示等待文字
    text_layer_set_text(s_status_layer, "Waiting for text...");
    layer_set_hidden(text_layer_get_layer(s_status_layer), false);

    // 向手機請求第一個區塊 (偏移量 0)
    messaging_request_text(0);
    s_waiting_for_chunk = true;
}

static void main_window_unload(Window *window) {
    layer_destroy(s_canvas_layer);
    text_layer_destroy(s_status_layer);
}

// ============================================================
// App 生命週期
// ============================================================

static void init(void) {
    // 讀取持久化設定
    if (persist_exists(PERSIST_KEY_SCROLL_SPEED)) {
        s_scroll_speed = persist_read_int(PERSIST_KEY_SCROLL_SPEED);
        if (s_scroll_speed < SCROLL_SPEED_MIN || s_scroll_speed > SCROLL_SPEED_MAX) {
            s_scroll_speed = SCROLL_SPEED_DEFAULT;
        }
    }
    if (persist_exists(PERSIST_KEY_TEXT_SIZE)) {
        int32_t size_val = persist_read_int(PERSIST_KEY_TEXT_SIZE);
        if (size_val >= TEXT_SIZE_SMALL && size_val <= TEXT_SIZE_LARGE) {
            s_text_size = (TextSizeLevel)size_val;
        }
    }

    // 初始化環形緩衝區
    ring_buffer_init(&s_ring_buffer);

    // 初始化通訊
    messaging_init(on_text_chunk_received, on_settings_synced, on_connection_changed);

    // 建立主視窗
    s_main_window = window_create();
    window_set_background_color(s_main_window, GColorBlack);
    window_set_click_config_provider(s_main_window, click_config_provider);
    window_set_window_handlers(s_main_window, (WindowHandlers){
        .load = main_window_load,
        .unload = main_window_unload,
    });
    window_stack_push(s_main_window, true);
}

static void deinit(void) {
    // 停止自動捲動
    stop_auto_scroll();
    if (s_manual_pause_timer) {
        app_timer_cancel(s_manual_pause_timer);
        s_manual_pause_timer = NULL;
    }
    if (s_manual_anim_timer) {
        app_timer_cancel(s_manual_anim_timer);
        s_manual_anim_timer = NULL;
    }

    // 釋放扁平化文字
    if (s_flat_text) {
        free(s_flat_text);
        s_flat_text = NULL;
    }

    // 釋放環形緩衝區
    ring_buffer_destroy(&s_ring_buffer);

    // 反初始化通訊
    messaging_deinit();

    // 銷毀視窗
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
