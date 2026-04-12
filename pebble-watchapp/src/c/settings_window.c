/**
 * settings_window.c - 設定視窗實作
 *
 * 使用 MenuLayer 提供捲動速度、文字大小與頂部時間欄的調整介面。
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
static SettingsChangedCallback s_change_callback = NULL;

// 選單區段
#define SECTION_SPEED     0
#define SECTION_SIZE      1
#define SECTION_STATUSBAR 2
#define NUM_SECTIONS      3

static const char *SIZE_NAMES[] = { "Tiny", "Small", "Medium", "Large", "XLarge" };

// ============================================================
// MenuLayer 回呼
// ============================================================

static uint16_t menu_get_num_sections(MenuLayer *menu_layer, void *data) {
    return NUM_SECTIONS;
}

static uint16_t menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                  void *data) {
    switch (section_index) {
        case SECTION_SPEED:     return SCROLL_SPEED_MAX;  // 6 個速度等級
        case SECTION_SIZE:      return 5;                  // Tiny/Small/Medium/Large/XLarge
        case SECTION_STATUSBAR: return 1;                  // 開/關切換
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
            // 顯示目前狀態；點擊即切換
            const char *state = s_current_status_bar ? "On" : "Off";
            menu_cell_basic_draw(ctx, cell_layer, "Time Bar", state, NULL);
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
            // 切換顯示狀態
            s_current_status_bar = !s_current_status_bar;
            changed = true;
            break;
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
    Window *window = (Window *)data;
    if (window) window_destroy(window);
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
                          SettingsChangedCallback callback) {
    s_current_speed      = speed;
    s_current_size       = size;
    s_current_status_bar = show_status_bar;
    s_change_callback    = callback;

    s_settings_window = window_create();
    window_set_window_handlers(s_settings_window, (WindowHandlers){
        .load   = settings_window_load,
        .unload = settings_window_unload,
    });
    window_stack_push(s_settings_window, true);
}
