/**
 * main.c - Peleprompter 主程式（圖片渲染模式）
 *
 * 手機端（Android）將文字預先渲染成 1-bit 圖片（每頁 = 手錶螢幕大小），
 * 手錶端接收圖片頁面並顯示，並保持原有的近無縫捲動、自動捲動、
 * 手動捲動、頭尾 50px 留白等使用體驗。
 *
 * 架構重點：
 * - 圖片管理：ImageManager 維護最多 MAX_PAGES_CACHED 頁的 GBitmap
 * - 捲動：s_scroll_offset_px 為全域虛擬捲動位置
 *   · virtual_height = total_pages × page_height
 *   · 頁碼 = scroll_offset / page_height
 *   · 頁內 y  = scroll_offset % page_height
 * - 繪圖：同一幀可能跨兩頁（頁邊界過渡），各取對應頁的子區域拼合
 * - 50px 留白：已由手機端直接烘焙進第 0 頁（頂部）與最後頁（底部）
 */

#include <pebble.h>
#include "constants.h"
#include "messaging.h"
#include "image_manager.h"
#include "settings_window.h"

// 捲動速度對應的計時器間隔 (毫秒)
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
static Window         *s_main_window        = NULL;
static Layer          *s_canvas_layer       = NULL;
static TextLayer      *s_status_layer       = NULL;
static StatusBarLayer *s_pebble_status_bar  = NULL;

// 圖片管理員
static ImageManager s_img_mgr;

// 捲動狀態
static int32_t s_scroll_offset_px     = 0;
static bool    s_auto_scroll_active   = false;
static AppTimer *s_scroll_timer       = NULL;

// 手動捲動動畫
static AppTimer *s_manual_anim_timer  = NULL;
static int32_t  s_target_scroll_px    = -1;  // -1 = 無動畫

// 手動捲動暫停
static AppTimer *s_manual_pause_timer = NULL;
static bool     s_manual_pause_active = false;

// 設定
static int32_t       s_scroll_speed    = SCROLL_SPEED_DEFAULT;
static TextSizeLevel s_text_size       = TEXT_SIZE_MEDIUM;
static bool          s_show_status_bar = false;

// 連線狀態
static bool s_bt_connected           = true;
static bool s_show_disconnect_warning = false;

// 頁面請求鎖（同一時間只允許一個 CMD_REQUEST_PAGE 在途）
static bool    s_waiting_for_page = false;

// 初始化完成旗標
static bool    s_images_initialized = false;

// ============================================================
// 輔助：計算最大捲動偏移
// ============================================================
static int32_t get_max_scroll(void) {
    if (!s_images_initialized || s_img_mgr.total_pages == 0) return 0;

    GRect bounds = layer_get_bounds(s_canvas_layer);
    int screen_h = bounds.size.h;
    int32_t total_virtual_h = s_img_mgr.total_pages * s_img_mgr.page_height;
    int32_t max = total_virtual_h - screen_h;
    return max > 0 ? max : 0;
}

// ============================================================
// 預取邏輯
// ============================================================
static void check_prefetch(void) {
    if (s_waiting_for_page) return;
    if (!s_images_initialized || s_img_mgr.total_pages == 0) return;
    if (!s_canvas_layer) return;

    GRect bounds = layer_get_bounds(s_canvas_layer);
    int screen_h = bounds.size.h;
    int32_t ph = s_img_mgr.page_height;

    int32_t top_page      = s_scroll_offset_px / ph;
    int32_t bottom_page   = (s_scroll_offset_px + screen_h - 1) / ph;
    int32_t px_to_next_pg = ph - (s_scroll_offset_px % ph);  // 距下一頁邊界

    // 向下預取：距下一頁邊界不足門檻時預取 top_page+1, top_page+2
    int32_t prefetch_pages = (ph > screen_h) ? 1 : 2;
    for (int32_t pg = bottom_page + 1; pg <= top_page + prefetch_pages; pg++) {
        if (pg >= s_img_mgr.total_pages) break;
        if (!image_manager_is_ready(&s_img_mgr, pg)
            && !image_manager_is_loading(&s_img_mgr, pg)) {
            // 在距頁面邊界還有足夠空間時才預取，避免過早請求
            if (px_to_next_pg < IMAGE_PREFETCH_THRESHOLD_PX || pg == bottom_page + 1) {
                image_manager_alloc_slot(&s_img_mgr, pg);
                messaging_request_page(pg);
                s_waiting_for_page = true;
                return;
            }
        }
    }

    // 向上預取：如果目前頁面的上一頁尚未載入（為向上捲動準備）
    if (top_page > 0) {
        int32_t prev_pg = top_page - 1;
        if (!image_manager_is_ready(&s_img_mgr, prev_pg)
            && !image_manager_is_loading(&s_img_mgr, prev_pg)) {
            image_manager_alloc_slot(&s_img_mgr, prev_pg);
            messaging_request_page(prev_pg);
            s_waiting_for_page = true;
        }
    }
}

// ============================================================
// 繪製回呼
// ============================================================

/**
 * 繪製指定頁面的子區域到畫布上
 * @param ctx      繪圖上下文
 * @param page_num 頁碼
 * @param src_y    頁面內起始 y (0-based)
 * @param height   要繪製的行數
 * @param dest_y   目標螢幕 y
 */
static void draw_page_region(GContext *ctx, int32_t page_num,
                              int32_t src_y, int32_t height, int32_t dest_y) {
    GBitmap *bmp = image_manager_get_bitmap(&s_img_mgr, page_num);
    if (!bmp || height <= 0) return;

    // 用 bounds 選取頁內子區域後繪製到目標位置
    gbitmap_set_bounds(bmp, GRect(0, src_y,
                                   s_img_mgr.page_width, height));
    graphics_draw_bitmap_in_rect(ctx, bmp,
        GRect(0, dest_y, s_img_mgr.page_width, height));
    // 還原完整 bounds 供下次使用
    gbitmap_set_bounds(bmp, GRect(0, 0,
                                   s_img_mgr.page_width, s_img_mgr.page_height));
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int screen_h = bounds.size.h;
    int screen_w = bounds.size.w;

    // 清除背景（黑色）
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    if (!s_images_initialized || s_img_mgr.total_pages == 0) {
        // 尚未初始化，狀態文字由 s_status_layer 顯示
        goto draw_warning;
    }

    {
        int32_t ph       = s_img_mgr.page_height;
        int32_t top_page = s_scroll_offset_px / ph;
        int32_t y_in_top = s_scroll_offset_px % ph;  // 頁內已捲動的像素

        // ── 繪製上半部（來自 top_page）────────────────────────
        int32_t rows_from_top = ph - y_in_top;
        if (rows_from_top > screen_h) rows_from_top = screen_h;

        if (image_manager_is_ready(&s_img_mgr, top_page)) {
            draw_page_region(ctx, top_page, y_in_top, rows_from_top, 0);
        }

        // ── 繪製下半部（來自 top_page+1，若畫面跨越頁邊界）────
        int32_t remaining = screen_h - rows_from_top;
        if (remaining > 0) {
            int32_t next_page = top_page + 1;
            if (next_page < s_img_mgr.total_pages) {
                if (image_manager_is_ready(&s_img_mgr, next_page)) {
                    draw_page_region(ctx, next_page, 0, remaining, rows_from_top);
                }
                // 若下一頁尚未就緒，露出黑色背景即可（已清過）
            }
        }
    }

draw_warning:
    // 斷線警告（緩衝區用盡且藍牙離線時才顯示）
    if (s_show_disconnect_warning) {
        GRect warn_rect = GRect(0, screen_h - 30, screen_w, 30);
#ifdef PBL_COLOR
        graphics_context_set_fill_color(ctx, GColorDarkGray);
        graphics_fill_rect(ctx, warn_rect, 0, GCornerNone);
        graphics_context_set_text_color(ctx, GColorWhite);
#else
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

    int32_t max_offset = get_max_scroll();

    if (s_scroll_offset_px < max_offset) {
        s_scroll_offset_px += SCROLL_STEP_PX;
        if (s_scroll_offset_px > max_offset) s_scroll_offset_px = max_offset;

        // 同步手動動畫目標，避免向下手動動畫與自動捲動互相拉扯
        if (s_target_scroll_px >= 0) {
            s_target_scroll_px += SCROLL_STEP_PX;
            if (s_target_scroll_px > max_offset) s_target_scroll_px = max_offset;
        }

        layer_mark_dirty(s_canvas_layer);
        check_prefetch();
        schedule_auto_scroll();
    } else {
        // 已到最底部
        int32_t last_page = s_img_mgr.total_pages - 1;
        if (!image_manager_is_ready(&s_img_mgr, last_page)
            || !s_images_initialized) {
            // 最後一頁還沒就緒 → 顯示 Loading...
            text_layer_set_text(s_status_layer, "Loading...");
            layer_set_hidden(text_layer_get_layer(s_status_layer), false);
            check_prefetch();

            if (!s_bt_connected) {
                s_show_disconnect_warning = true;
                layer_mark_dirty(s_canvas_layer);
            }
            // 等頁面到達後由 on_image_chunk_received 恢復
        }
        // 已到真正結尾，不繼續排程
    }
}

static void start_auto_scroll(void) {
    s_auto_scroll_active  = true;
    s_manual_pause_active = false;

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
// 手動捲動
// ============================================================

static void manual_pause_expired(void *data) {
    s_manual_pause_timer  = NULL;
    s_manual_pause_active = false;

    if (s_auto_scroll_active) {
        schedule_auto_scroll();
    }
}

static void trigger_manual_pause(void) {
    if (s_auto_scroll_active) {
        s_manual_pause_active = true;
        if (s_scroll_timer) {
            app_timer_cancel(s_scroll_timer);
            s_scroll_timer = NULL;
        }
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

    if (s_target_scroll_px < 0) return;

    if (s_scroll_offset_px != s_target_scroll_px) {
        int32_t diff = s_target_scroll_px - s_scroll_offset_px;
        int32_t step = diff / 2;
        if (step == 0) step = (diff > 0) ? 1 : -1;

        s_scroll_offset_px += step;
        layer_mark_dirty(s_canvas_layer);

        s_manual_anim_timer = app_timer_register(
            MANUAL_ANIMATION_STEP_MS, manual_anim_tick, NULL
        );
    } else {
        s_target_scroll_px = -1;
        check_prefetch();
    }
}

static void do_manual_scroll(int32_t delta_px) {
    int32_t max_offset = get_max_scroll();

    if (s_target_scroll_px < 0) {
        s_target_scroll_px = s_scroll_offset_px;
    }

    s_target_scroll_px += delta_px;
    if (s_target_scroll_px < 0) s_target_scroll_px = 0;
    if (s_target_scroll_px > max_offset) s_target_scroll_px = max_offset;

    if (delta_px < 0) {
        trigger_manual_pause();
    }

    if (!s_manual_anim_timer) {
        s_manual_anim_timer = app_timer_register(
            MANUAL_ANIMATION_STEP_MS, manual_anim_tick, NULL
        );
    }
}

// ============================================================
// 按鈕處理
// ============================================================

static void up_short_click(ClickRecognizerRef recognizer, void *context) {
    do_manual_scroll(-MANUAL_SCROLL_SHORT_PX);
}

static void down_short_click(ClickRecognizerRef recognizer, void *context) {
    do_manual_scroll(MANUAL_SCROLL_SHORT_PX);
}

static void select_short_click(ClickRecognizerRef recognizer, void *context) {
    if (s_auto_scroll_active && !s_manual_pause_active) {
        stop_auto_scroll();
    } else {
        start_auto_scroll();
    }
}

static void on_settings_changed(int32_t speed, TextSizeLevel size, bool show_status_bar);

static void select_long_click(ClickRecognizerRef recognizer, void *context) {
    settings_window_push(s_scroll_speed, s_text_size, s_show_status_bar, on_settings_changed);
}

static void click_config_provider(void *context) {
    window_single_repeating_click_subscribe(BUTTON_ID_UP,   100, up_short_click);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_short_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, select_short_click);
    window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_click, NULL);
}

// ============================================================
// 通訊回呼
// ============================================================

/** 收到 CMD_INIT_IMAGES：手機已計算好總頁數，開始請求第 0 頁 */
static void on_images_initialized(int32_t total_pages,
                                   int32_t page_width,
                                   int32_t page_height) {
    s_images_initialized = true;
    s_scroll_offset_px   = 0;
    s_target_scroll_px   = -1;

    image_manager_set_dimensions(&s_img_mgr, total_pages, page_width, page_height);

    // 隱藏 "Waiting for text..." 狀態
    layer_set_hidden(text_layer_get_layer(s_status_layer), true);
    s_show_disconnect_warning = false;

    // 立即請求第 0 頁
    image_manager_alloc_slot(&s_img_mgr, 0);
    messaging_request_page(0);
    s_waiting_for_page = true;

    APP_LOG(APP_LOG_LEVEL_INFO,
            "圖片模式初始化：%ld 頁, %ldx%ld",
            (long)total_pages, (long)page_width, (long)page_height);
}

/** 收到圖片區塊 */
static void on_image_chunk_received(int32_t page_num,
                                     int32_t chunk_idx,
                                     int32_t total_chunks,
                                     const uint8_t *data,
                                     int32_t data_len) {
    bool page_complete = image_manager_receive_chunk(
        &s_img_mgr, page_num, chunk_idx, total_chunks, data, data_len
    );

    if (page_complete) {
        s_waiting_for_page = false;

        // 隱藏斷線警告 / Loading 狀態
        s_show_disconnect_warning = false;
        layer_set_hidden(text_layer_get_layer(s_status_layer), true);

        layer_mark_dirty(s_canvas_layer);
        check_prefetch();

        // 若自動捲動因等待而停滯，恢復排程
        if (s_auto_scroll_active && !s_manual_pause_active && !s_scroll_timer) {
            schedule_auto_scroll();
        }
    } else {
        // 仍有後續區塊在途，不解鎖 s_waiting_for_page
        // 在接收完整頁之前，持續重繪（讓 Loading 文字保持可見）
    }
}

/** 收到設定同步 */
static void on_settings_synced(int32_t scroll_speed, int32_t text_size) {
    bool needs_reinit = false;

    if (scroll_speed >= SCROLL_SPEED_MIN && scroll_speed <= SCROLL_SPEED_MAX) {
        s_scroll_speed = scroll_speed;
        persist_write_int(PERSIST_KEY_SCROLL_SPEED, s_scroll_speed);
    }

    if (text_size >= TEXT_SIZE_SMALL && text_size <= TEXT_SIZE_LARGE) {
        if ((TextSizeLevel)text_size != s_text_size) {
            s_text_size  = (TextSizeLevel)text_size;
            persist_write_int(PERSIST_KEY_TEXT_SIZE, (int32_t)s_text_size);
            needs_reinit = true;  // 字型變更需要重新渲染所有頁面
        }
    }

    if (needs_reinit && s_canvas_layer) {
        // 重新初始化：清空快取，向手機請求以新字型大小重新渲染
        s_images_initialized = false;
        image_manager_deinit(&s_img_mgr);

        GRect bounds = layer_get_bounds(s_canvas_layer);
        messaging_request_init(bounds.size.w, bounds.size.h, (int32_t)s_text_size);

        text_layer_set_text(s_status_layer, "Reloading...");
        layer_set_hidden(text_layer_get_layer(s_status_layer), false);
    }

    if (s_auto_scroll_active && !s_manual_pause_active) {
        schedule_auto_scroll();
    }
}

/** 設定視窗回呼（在手錶上更改設定） */
static void on_settings_changed(int32_t speed, TextSizeLevel size, bool show_status_bar) {
    bool needs_reinit = (size != s_text_size) || (show_status_bar != s_show_status_bar);

    s_scroll_speed    = speed;
    s_text_size       = size;
    s_show_status_bar = show_status_bar;

    persist_write_int(PERSIST_KEY_SCROLL_SPEED,    s_scroll_speed);
    persist_write_int(PERSIST_KEY_TEXT_SIZE,        (int32_t)s_text_size);
    persist_write_int(PERSIST_KEY_SHOW_STATUS_BAR,  (int32_t)s_show_status_bar);

    messaging_send_settings(s_scroll_speed, (int32_t)s_text_size);

    // 套用頂部時間欄變更（調整 canvas / StatusBarLayer）
    apply_status_bar();

    if (needs_reinit && s_canvas_layer) {
        s_images_initialized = false;
        image_manager_deinit(&s_img_mgr);

        // 使用更新後的 canvas 尺寸（已反映時間欄高度）
        GRect canvas_bounds = layer_get_bounds(s_canvas_layer);
        messaging_request_init(canvas_bounds.size.w, canvas_bounds.size.h, (int32_t)s_text_size);

        text_layer_set_text(s_status_layer, "Reloading...");
        layer_set_hidden(text_layer_get_layer(s_status_layer), false);
        layer_mark_dirty(s_canvas_layer);
    }

    if (s_auto_scroll_active && !s_manual_pause_active) {
        schedule_auto_scroll();
    }
}

/** 藍牙連線狀態改變 */
static void on_connection_changed(bool connected) {
    s_bt_connected = connected;

    if (connected) {
        s_show_disconnect_warning = false;
        layer_mark_dirty(s_canvas_layer);
        check_prefetch();
    }
}

// ============================================================
// 狀態欄輔助
// ============================================================

/**
 * 套用目前的 s_show_status_bar 設定：
 * 建立/銷毀 StatusBarLayer，並相應地調整 canvas 與狀態文字的框架。
 * 可在視窗載入後的任何時機呼叫。
 */
static void apply_status_bar(void) {
    if (!s_main_window) return;

    Layer *window_layer = window_get_root_layer(s_main_window);
    GRect  win_bounds   = layer_get_bounds(window_layer);
    GRect  canvas_rect  = win_bounds;

    if (s_show_status_bar) {
        canvas_rect.origin.y  = STATUS_BAR_LAYER_HEIGHT;
        canvas_rect.size.h   -= STATUS_BAR_LAYER_HEIGHT;

        if (!s_pebble_status_bar) {
            s_pebble_status_bar = status_bar_layer_create();
            layer_add_child(window_layer,
                            status_bar_layer_get_layer(s_pebble_status_bar));
        }
    } else {
        if (s_pebble_status_bar) {
            layer_remove_from_parent(
                status_bar_layer_get_layer(s_pebble_status_bar));
            status_bar_layer_destroy(s_pebble_status_bar);
            s_pebble_status_bar = NULL;
        }
    }

    if (s_canvas_layer) {
        layer_set_frame(s_canvas_layer, canvas_rect);
    }

    // 將「Waiting…」文字層置中於 canvas 區域（以視窗座標表示）
    if (s_status_layer) {
        GRect text_rect = GRect(
            canvas_rect.origin.x,
            canvas_rect.origin.y + canvas_rect.size.h / 2 - 15,
            canvas_rect.size.w,
            30
        );
        layer_set_frame(text_layer_get_layer(s_status_layer), text_rect);
    }
}

// ============================================================
// 主視窗生命週期
// ============================================================

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    // 建立自訂繪製圖層（初始全螢幕，apply_status_bar 會視需要縮減）
    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(window_layer, s_canvas_layer);

    // 建立狀態文字層（位置由 apply_status_bar 決定）
    s_status_layer = text_layer_create(
        GRect(0, bounds.size.h / 2 - 15, bounds.size.w, 30)
    );
    text_layer_set_background_color(s_status_layer, GColorClear);
    text_layer_set_text_color(s_status_layer, GColorWhite);
    text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
    text_layer_set_font(s_status_layer,
                        fonts_get_system_font(FONT_KEY_GOTHIC_18));
    text_layer_set_text(s_status_layer, "Waiting for text...");
    layer_set_hidden(text_layer_get_layer(s_status_layer), false);
    layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

    // 套用頂部時間欄設定（建立 StatusBarLayer 並調整 canvas 框架）
    apply_status_bar();

    // 向手機發送初始化請求，使用 canvas 實際尺寸（已扣除時間欄高度）
    GRect canvas_bounds = layer_get_bounds(s_canvas_layer);
    messaging_request_init(canvas_bounds.size.w, canvas_bounds.size.h, (int32_t)s_text_size);
    s_waiting_for_page = false;
}

static void main_window_unload(Window *window) {
    if (s_pebble_status_bar) {
        status_bar_layer_destroy(s_pebble_status_bar);
        s_pebble_status_bar = NULL;
    }
    layer_destroy(s_canvas_layer);
    text_layer_destroy(s_status_layer);
    s_canvas_layer = NULL;
    s_status_layer = NULL;
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
    if (persist_exists(PERSIST_KEY_SHOW_STATUS_BAR)) {
        s_show_status_bar = (bool)persist_read_int(PERSIST_KEY_SHOW_STATUS_BAR);
    }

    // 初始化圖片管理員
    image_manager_init(&s_img_mgr);

    // 初始化通訊
    messaging_init(on_images_initialized,
                   on_image_chunk_received,
                   on_settings_synced,
                   on_connection_changed);

    // 建立主視窗
    s_main_window = window_create();
    window_set_background_color(s_main_window, GColorBlack);
    window_set_click_config_provider(s_main_window, click_config_provider);
    window_set_window_handlers(s_main_window, (WindowHandlers){
        .load   = main_window_load,
        .unload = main_window_unload,
    });
    window_stack_push(s_main_window, true);
}

static void deinit(void) {
    stop_auto_scroll();
    if (s_manual_pause_timer) {
        app_timer_cancel(s_manual_pause_timer);
        s_manual_pause_timer = NULL;
    }
    if (s_manual_anim_timer) {
        app_timer_cancel(s_manual_anim_timer);
        s_manual_anim_timer = NULL;
    }

    image_manager_deinit(&s_img_mgr);
    messaging_deinit();
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
