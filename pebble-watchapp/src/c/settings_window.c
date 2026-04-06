/**
 * settings_window.c - 設定視窗實作
 *
 * 使用 MenuLayer 提供捲動速度和文字大小的調整介面。
 */

#include "settings_window.h"

// ============================================================
// 私有狀態
// ============================================================
static Window *s_settings_window = NULL;
static MenuLayer *s_menu_layer = NULL;

static int32_t s_current_speed;
static TextSizeLevel s_current_size;
static SettingsChangedCallback s_change_callback = NULL;

// 選單區段
#define SECTION_SPEED 0
#define SECTION_SIZE  1
#define NUM_SECTIONS  2

// 文字大小名稱
static const char *SIZE_NAMES[] = { "Small", "Medium", "Large" };

// ============================================================
// MenuLayer 回呼
// ============================================================

static uint16_t menu_get_num_sections(MenuLayer *menu_layer, void *data) {
    return NUM_SECTIONS;
}

static uint16_t menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                  void *data) {
    switch (section_index) {
        case SECTION_SPEED: return SCROLL_SPEED_MAX;  // 6 個速度等級
        case SECTION_SIZE:  return 3;                  // 小/中/大
        default: return 0;
    }
}

static int16_t menu_get_header_height(MenuLayer *menu_layer, uint16_t section_index,
                                      void *data) {
    return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void menu_draw_header(GContext *ctx, const Layer *cell_layer,
                             uint16_t section_index, void *data) {
    const char *title = (section_index == SECTION_SPEED) ? "Scroll Speed" : "Text Size";

    GRect bounds = layer_get_bounds(cell_layer);
    
    #if defined(PBL_COLOR)
    graphics_context_set_fill_color(ctx, GColorLightGray);
    #else
    graphics_context_set_fill_color(ctx, GColorWhite);
    #endif
    
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorBlack);
    
    // 強制設定寬度為螢幕寬度，忽略 MenuLayer 自帶的縮進，確保絕對置中
    #if defined(PBL_ROUND)
    int16_t screen_width = 180;
    #else
    int16_t screen_width = bounds.size.w > 0 ? bounds.size.w : 144;
    #endif
    
    GRect text_bounds = GRect(0, bounds.origin.y - 2, screen_width, bounds.size.h + 4);

    graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       text_bounds, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer,
                           MenuIndex *cell_index, void *data) {
    char buf[32];

    switch (cell_index->section) {
        case SECTION_SPEED: {
            int level = cell_index->row + 1;
            snprintf(buf, sizeof(buf), "Speed %d", level);

            // 顯示目前選取的速度
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
    }

    if (changed) {
        // 重繪選單以更新 "Current" 標記
        menu_layer_reload_data(s_menu_layer);

        // 通知主程式設定已變更
        if (s_change_callback) {
            s_change_callback(s_current_speed, s_current_size);
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
        .get_num_rows = menu_get_num_rows,
        .get_header_height = menu_get_header_height,
        .draw_header = menu_draw_header,
        .draw_row = menu_draw_row,
        .select_click = menu_select_click,
    });

    menu_layer_set_click_config_onto_window(s_menu_layer, window);

    #ifdef PBL_ROUND
    menu_layer_set_center_focused(s_menu_layer, true);
    #endif

    layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

// 延遲銷毀視窗的計時器回呼 (不可在 unload 中直接銷毀自身)
static void deferred_window_destroy(void *data) {
    Window *window = (Window *)data;
    if (window) {
        window_destroy(window);
    }
}

static void settings_window_unload(Window *window) {
    menu_layer_destroy(s_menu_layer);
    s_menu_layer = NULL;
    // 延遲銷毀視窗，避免在 unload 回呼中銷毀自身
    app_timer_register(0, deferred_window_destroy, s_settings_window);
    s_settings_window = NULL;
}

// ============================================================
// 公開 API
// ============================================================

void settings_window_push(int32_t speed, TextSizeLevel size,
                          SettingsChangedCallback callback) {
    s_current_speed = speed;
    s_current_size = size;
    s_change_callback = callback;

    s_settings_window = window_create();
    window_set_window_handlers(s_settings_window, (WindowHandlers){
        .load = settings_window_load,
        .unload = settings_window_unload,
    });

    window_stack_push(s_settings_window, true /* animated */);
}
