/**
 * settings_window.c - 設定視窗實作
 *
 * 使用 MenuLayer 提供捲動速度、文字大小、頂部時間欄以及跳頁功能的調整介面。
 * Navigation 區段支援兩種跳轉模式：按頁碼 (By Page) 與按百分比 (By %)。
 */

#include "settings_window.h"

// ============================================================
// 私有狀態
// ============================================================
static Window    *s_settings_window  = NULL;
static MenuLayer *s_menu_layer       = NULL;

static int32_t       s_current_speed      = SCROLL_SPEED_DEFAULT;
static TextSizeLevel s_current_size       = TEXT_SIZE_MEDIUM;
static bool          s_current_status_bar = false;
static int32_t       s_current_page       = 0;    // 1-indexed, 0=未載入
static int32_t       s_total_pages        = 0;    // 0=未載入
static int32_t       s_current_pct        = 0;    // 0-100
static SettingsChangedCallback s_change_callback   = NULL;
static JumpToPageCallback      s_jump_page_cb      = NULL;
static JumpToPercentCallback   s_jump_pct_cb       = NULL;

// 選單區段
#define SECTION_SPEED     0
#define SECTION_SIZE      1
#define SECTION_STATUSBAR 2
#define SECTION_JUMP      3
#define NUM_SECTIONS      4

// SECTION_JUMP 內的行
#define JUMP_ROW_PAGE  0   // "By Page"
#define JUMP_ROW_PCT   1   // "By %"

static const char *SIZE_NAMES[] = { "Tiny", "Small", "Medium", "Large", "XLarge" };

// ============================================================
// 跳頁視窗（By Page）
// ============================================================
static Window  *s_jump_window      = NULL;
static Layer   *s_jump_layer       = NULL;
static int32_t  s_jump_target_page = 1;

static void jump_layer_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int sw = bounds.size.w;
    int sh = bounds.size.h;

    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    GFont label_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    GFont big_font   = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
    GFont small_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);

    graphics_context_set_text_color(ctx, GColorWhite);

    // "Jump to:" 標題
#ifdef PBL_ROUND
    int title_y = 30;
#else
    int title_y = 18;
#endif
    graphics_draw_text(ctx, "Jump to:",
                       label_font, GRect(0, title_y, sw, 24),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // 大號目標頁碼
    char num_buf[8];
    snprintf(num_buf, sizeof(num_buf), "%d", (int)s_jump_target_page);
    graphics_draw_text(ctx, num_buf,
                       big_font, GRect(0, sh / 2 - 28, sw, 55),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // 目前頁碼 / 總頁數（提示用）
    if (s_total_pages > 0) {
        char info_buf[16];
        snprintf(info_buf, sizeof(info_buf), "%d/%d",
                 (int)s_current_page, (int)s_total_pages);
#ifdef PBL_COLOR
        graphics_context_set_text_color(ctx, GColorLightGray);
#else
        graphics_context_set_text_color(ctx, GColorWhite);
#endif
#ifdef PBL_ROUND
        graphics_draw_text(ctx, info_buf, small_font,
                           GRect(sw / 2 - 30, sh - 22, 60, 16),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
#else
        graphics_draw_text(ctx, info_buf, small_font,
                           GRect(sw - 58, sh - 20, 56, 16),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
#endif
    }
}

static void jump_up_handler(ClickRecognizerRef recognizer, void *context) {
    int32_t max = (s_total_pages > 0) ? s_total_pages : 9999;
    if (s_jump_target_page < max) {
        s_jump_target_page++;
        layer_mark_dirty(s_jump_layer);
    }
}

static void jump_down_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_jump_target_page > 1) {
        s_jump_target_page--;
        layer_mark_dirty(s_jump_layer);
    }
}

static void jump_select_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_jump_page_cb && s_total_pages > 0) {
        s_jump_page_cb(s_jump_target_page - 1);
    }
    if (s_jump_window)    window_stack_remove(s_jump_window, true);
    if (s_settings_window) window_stack_remove(s_settings_window, true);
}

static void jump_click_provider(void *context) {
    window_single_repeating_click_subscribe(BUTTON_ID_UP,   150, jump_up_handler);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, jump_down_handler);
    window_single_click_subscribe(BUTTON_ID_SELECT, jump_select_handler);
}

static void jump_window_load(Window *window) {
    Layer *wl   = window_get_root_layer(window);
    GRect  bnds = layer_get_bounds(wl);
    s_jump_layer = layer_create(bnds);
    layer_set_update_proc(s_jump_layer, jump_layer_update_proc);
    layer_add_child(wl, s_jump_layer);
}

static void deferred_jump_destroy(void *data) {
    if (data) window_destroy((Window *)data);
}

static void jump_window_unload(Window *window) {
    layer_destroy(s_jump_layer);
    s_jump_layer = NULL;
    app_timer_register(0, deferred_jump_destroy, s_jump_window);
    s_jump_window = NULL;
}

static void open_jump_window(void) {
    s_jump_target_page = (s_current_page > 0) ? s_current_page : 1;

    s_jump_window = window_create();
    window_set_background_color(s_jump_window, GColorBlack);
    window_set_click_config_provider(s_jump_window, jump_click_provider);
    window_set_window_handlers(s_jump_window, (WindowHandlers){
        .load   = jump_window_load,
        .unload = jump_window_unload,
    });
    window_stack_push(s_jump_window, true);
}

// ============================================================
// 百分比跳轉視窗（By %）
// ============================================================
static Window  *s_pct_window       = NULL;
static Layer   *s_pct_layer        = NULL;
static int32_t  s_jump_target_pct  = 0;   // 0-100

static void pct_layer_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int sw = bounds.size.w;
    int sh = bounds.size.h;

    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    GFont label_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    GFont big_font   = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
    GFont small_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);

    graphics_context_set_text_color(ctx, GColorWhite);

    // "Jump to:" 標題
#ifdef PBL_ROUND
    int title_y = 30;
#else
    int title_y = 18;
#endif
    graphics_draw_text(ctx, "Jump to:",
                       label_font, GRect(0, title_y, sw, 24),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // 大號目標百分比（e.g. "42%"）
    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)s_jump_target_pct);
    graphics_draw_text(ctx, pct_buf,
                       big_font, GRect(0, sh / 2 - 28, sw, 55),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // 目前頁碼 / 總頁數（提示用）
    if (s_total_pages > 0) {
        char info_buf[16];
        snprintf(info_buf, sizeof(info_buf), "%d/%d",
                 (int)s_current_page, (int)s_total_pages);
#ifdef PBL_COLOR
        graphics_context_set_text_color(ctx, GColorLightGray);
#else
        graphics_context_set_text_color(ctx, GColorWhite);
#endif
#ifdef PBL_ROUND
        graphics_draw_text(ctx, info_buf, small_font,
                           GRect(sw / 2 - 30, sh - 22, 60, 16),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
#else
        graphics_draw_text(ctx, info_buf, small_font,
                           GRect(sw - 58, sh - 20, 56, 16),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
#endif
    }
}

static void pct_up_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_jump_target_pct < 100) {
        s_jump_target_pct++;
        layer_mark_dirty(s_pct_layer);
    }
}

static void pct_down_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_jump_target_pct > 0) {
        s_jump_target_pct--;
        layer_mark_dirty(s_pct_layer);
    }
}

static void pct_select_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_jump_pct_cb) {
        s_jump_pct_cb(s_jump_target_pct);
    }
    if (s_pct_window)      window_stack_remove(s_pct_window, true);
    if (s_settings_window) window_stack_remove(s_settings_window, true);
}

static void pct_click_provider(void *context) {
    window_single_repeating_click_subscribe(BUTTON_ID_UP,   150, pct_up_handler);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, pct_down_handler);
    window_single_click_subscribe(BUTTON_ID_SELECT, pct_select_handler);
}

static void pct_window_load(Window *window) {
    Layer *wl   = window_get_root_layer(window);
    GRect  bnds = layer_get_bounds(wl);
    s_pct_layer = layer_create(bnds);
    layer_set_update_proc(s_pct_layer, pct_layer_update_proc);
    layer_add_child(wl, s_pct_layer);
}

static void deferred_pct_destroy(void *data) {
    if (data) window_destroy((Window *)data);
}

static void pct_window_unload(Window *window) {
    layer_destroy(s_pct_layer);
    s_pct_layer = NULL;
    app_timer_register(0, deferred_pct_destroy, s_pct_window);
    s_pct_window = NULL;
}

static void open_pct_window(void) {
    // 預設值為目前的進度百分比
    s_jump_target_pct = s_current_pct;

    s_pct_window = window_create();
    window_set_background_color(s_pct_window, GColorBlack);
    window_set_click_config_provider(s_pct_window, pct_click_provider);
    window_set_window_handlers(s_pct_window, (WindowHandlers){
        .load   = pct_window_load,
        .unload = pct_window_unload,
    });
    window_stack_push(s_pct_window, true);
}

// ============================================================
// MenuLayer 回呼
// ============================================================

static uint16_t menu_get_num_sections(MenuLayer *menu_layer, void *data) {
    return NUM_SECTIONS;
}

static uint16_t menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                  void *data) {
    switch (section_index) {
        case SECTION_SPEED:     return SCROLL_SPEED_MAX;
        case SECTION_SIZE:      return 5;
        case SECTION_STATUSBAR: return 1;
        case SECTION_JUMP:      return 2;   // By Page + By %
        default: return 0;
    }
}

static int16_t menu_get_header_height(MenuLayer *menu_layer, uint16_t section_index,
                                      void *data) {
    return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void menu_draw_header(GContext *ctx, const Layer *cell_layer,
                             uint16_t section_index, void *data) {
    const char *title;
    switch (section_index) {
        case SECTION_SPEED:     title = "Scroll Speed"; break;
        case SECTION_SIZE:      title = "Text Size";    break;
        case SECTION_STATUSBAR: title = "Display";      break;
        case SECTION_JUMP:      title = "Navigation";   break;
        default:                title = "";              break;
    }

    GRect bounds = layer_get_bounds(cell_layer);

#if defined(PBL_COLOR)
    graphics_context_set_fill_color(ctx, GColorLightGray);
#else
    graphics_context_set_fill_color(ctx, GColorWhite);
#endif

    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorBlack);

#if defined(PBL_ROUND)
    int16_t screen_width = 180;
#else
    int16_t screen_width = bounds.size.w > 0 ? bounds.size.w : 144;
#endif

    GRect text_bounds = GRect(0, bounds.origin.y - 2, screen_width, bounds.size.h + 4);
    graphics_draw_text(ctx, title,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       text_bounds, GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer,
                           MenuIndex *cell_index, void *data) {
    char buf[32];

    switch (cell_index->section) {
        case SECTION_SPEED: {
            int level = cell_index->row + 1;
            snprintf(buf, sizeof(buf), "Speed %d", level);
            if (level == s_current_speed) {
                menu_cell_basic_draw(ctx, cell_layer, buf, "Current", NULL);
            } else {
                menu_cell_basic_draw(ctx, cell_layer, buf, NULL, NULL);
            }
            break;
        }
        case SECTION_SIZE: {
            const char *name = SIZE_NAMES[cell_index->row];
            if ((int)cell_index->row == (int)s_current_size) {
                menu_cell_basic_draw(ctx, cell_layer, name, "Current", NULL);
            } else {
                menu_cell_basic_draw(ctx, cell_layer, name, NULL, NULL);
            }
            break;
        }
        case SECTION_STATUSBAR: {
            const char *state = s_current_status_bar ? "On" : "Off";
            menu_cell_basic_draw(ctx, cell_layer, "Time Bar", state, NULL);
            break;
        }
        case SECTION_JUMP: {
            if (cell_index->row == JUMP_ROW_PAGE) {
                // By Page：副標題顯示目前頁碼
                if (s_total_pages > 0) {
                    char sub[16];
                    snprintf(sub, sizeof(sub), "Pg %d/%d",
                             (int)s_current_page, (int)s_total_pages);
                    menu_cell_basic_draw(ctx, cell_layer, "By Page", sub, NULL);
                } else {
                    menu_cell_basic_draw(ctx, cell_layer, "By Page", "No doc", NULL);
                }
            } else {
                // By %：副標題顯示目前百分比
                if (s_total_pages > 0) {
                    char sub[8];
                    snprintf(sub, sizeof(sub), "%d%%", (int)s_current_pct);
                    menu_cell_basic_draw(ctx, cell_layer, "By %", sub, NULL);
                } else {
                    menu_cell_basic_draw(ctx, cell_layer, "By %", "No doc", NULL);
                }
            }
            break;
        }
    }
}

static void menu_select_click(MenuLayer *menu_layer, MenuIndex *cell_index,
                               void *data) {
    bool changed = false;

    switch (cell_index->section) {
        case SECTION_SPEED: {
            int32_t new_speed = cell_index->row + 1;
            if (new_speed != s_current_speed) {
                s_current_speed = new_speed;
                changed = true;
            }
            break;
        }
        case SECTION_SIZE: {
            TextSizeLevel new_size = (TextSizeLevel)cell_index->row;
            if (new_size != s_current_size) {
                s_current_size = new_size;
                changed = true;
            }
            break;
        }
        case SECTION_STATUSBAR: {
            s_current_status_bar = !s_current_status_bar;
            changed = true;
            break;
        }
        case SECTION_JUMP: {
            if (cell_index->row == JUMP_ROW_PAGE) {
                open_jump_window();
            } else {
                open_pct_window();
            }
            return;  // 不觸發 changed callback
        }
    }

    if (changed) {
        menu_layer_reload_data(s_menu_layer);
        if (s_change_callback) {
            s_change_callback(s_current_speed, s_current_size, s_current_status_bar);
        }
    }
}

// ============================================================
// 視窗生命週期
// ============================================================

static void settings_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
        .get_num_sections = menu_get_num_sections,
        .get_num_rows     = menu_get_num_rows,
        .get_header_height= menu_get_header_height,
        .draw_header      = menu_draw_header,
        .draw_row         = menu_draw_row,
        .select_click     = menu_select_click,
    });
    menu_layer_set_click_config_onto_window(s_menu_layer, window);

#ifdef PBL_ROUND
    menu_layer_set_center_focused(s_menu_layer, true);
#endif

    layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void deferred_window_destroy(void *data) {
    if (data) window_destroy((Window *)data);
}

static void settings_window_unload(Window *window) {
    menu_layer_destroy(s_menu_layer);
    s_menu_layer = NULL;
    app_timer_register(0, deferred_window_destroy, s_settings_window);
    s_settings_window = NULL;
}

// ============================================================
// 公開 API
// ============================================================

void settings_window_push(int32_t speed, TextSizeLevel size,
                          bool show_status_bar,
                          int32_t current_page,
                          int32_t total_pages,
                          int32_t current_percent,
                          SettingsChangedCallback changed_cb,
                          JumpToPageCallback jump_page_cb,
                          JumpToPercentCallback jump_pct_cb) {
    s_current_speed      = speed;
    s_current_size       = size;
    s_current_status_bar = show_status_bar;
    s_current_page       = current_page;
    s_total_pages        = total_pages;
    s_current_pct        = current_percent;
    s_change_callback    = changed_cb;
    s_jump_page_cb       = jump_page_cb;
    s_jump_pct_cb        = jump_pct_cb;

    s_settings_window = window_create();
    window_set_window_handlers(s_settings_window, (WindowHandlers){
        .load   = settings_window_load,
        .unload = settings_window_unload,
    });
    window_stack_push(s_settings_window, true);
}
