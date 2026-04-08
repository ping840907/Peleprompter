/**
 * messaging.c - AppMessage 通訊模組實作（圖片模式）
 *
 * 處理手錶與 Android 手機之間的雙向通訊。
 */

#include "constants.h"
#include "messaging.h"
#include <string.h>
#include <stdlib.h>

// ============================================================
// 私有狀態
// ============================================================
static ImagesInitCallback     s_init_callback     = NULL;
static ImageChunkCallback     s_chunk_callback    = NULL;
static SettingsSyncCallback   s_settings_callback = NULL;
static ConnectionChangeCallback s_conn_callback   = NULL;

static bool s_is_connected = false;
static bool s_is_sending   = false;   // Outbox 使用鎖，防止重複發送

// 重試佇列：記錄最後一次失敗的請求頁碼
static bool    s_retry_pending  = false;
static int32_t s_retry_page_num = 0;

// ============================================================
// 內部：處理收到的訊息
// ============================================================
static void inbox_received_handler(DictionaryIterator *iterator, void *context) {
    Tuple *cmd_tuple = dict_find(iterator, KEY_COMMAND);
    if (!cmd_tuple) return;

    int32_t command = cmd_tuple->value->int32;

    switch (command) {

        case CMD_INIT_IMAGES: {
            // 手機回傳圖片初始化資訊
            Tuple *pages_t  = dict_find(iterator, KEY_TOTAL_PAGES);
            Tuple *width_t  = dict_find(iterator, KEY_WATCH_WIDTH);
            Tuple *height_t = dict_find(iterator, KEY_WATCH_HEIGHT);

            if (pages_t && width_t && height_t && s_init_callback) {
                int32_t total_pages  = pages_t->value->int32;
                int32_t page_width   = width_t->value->int32;
                int32_t page_height  = height_t->value->int32;
                APP_LOG(APP_LOG_LEVEL_INFO,
                        "收到 INIT_IMAGES: %ld 頁, %ldx%ld",
                        (long)total_pages, (long)page_width, (long)page_height);
                s_init_callback(total_pages, page_width, page_height);
            }
            break;
        }

        case CMD_SEND_IMAGE_CHUNK: {
            // 手機回傳圖片區塊
            Tuple *page_t   = dict_find(iterator, KEY_PAGE_NUM);
            Tuple *cidx_t   = dict_find(iterator, KEY_CHUNK_INDEX);
            Tuple *ctot_t   = dict_find(iterator, KEY_TOTAL_CHUNKS);
            Tuple *data_t   = dict_find(iterator, KEY_IMAGE_DATA);

            if (page_t && cidx_t && ctot_t && data_t && s_chunk_callback) {
                int32_t page_num     = page_t->value->int32;
                int32_t chunk_idx    = cidx_t->value->int32;
                int32_t total_chunks = ctot_t->value->int32;
                const uint8_t *data  = data_t->value->data;
                int32_t data_len     = (int32_t)data_t->length;

                s_chunk_callback(page_num, chunk_idx, total_chunks, data, data_len);
            }
            break;
        }

        case CMD_SYNC_SETTINGS: {
            Tuple *speed_t = dict_find(iterator, KEY_SCROLL_SPEED);
            Tuple *size_t  = dict_find(iterator, KEY_TEXT_SIZE);

            if (s_settings_callback) {
                int32_t speed = speed_t ? speed_t->value->int32 : -1;
                int32_t size  = size_t  ? size_t->value->int32  : -1;
                s_settings_callback(speed, size);
            }
            break;
        }

        default:
            APP_LOG(APP_LOG_LEVEL_WARNING, "未知指令: %ld", (long)command);
            break;
    }
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "收件匣訊息遺失: %d", (int)reason);
}

// ============================================================
// 內部：處理發送結果
// ============================================================
static void outbox_sent_handler(DictionaryIterator *iterator, void *context) {
    s_is_sending = false;

    if (s_retry_pending) {
        s_retry_pending = false;
        messaging_request_page(s_retry_page_num);
    }
}

static void outbox_failed_handler(DictionaryIterator *iterator,
                                  AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "發送失敗: %d", (int)reason);
    s_is_sending = false;

    if (s_retry_pending) {
        int32_t page_to_retry = s_retry_page_num;
        s_retry_pending = false;
        messaging_request_page(page_to_retry);
    }
}

// ============================================================
// 內部：藍牙連線狀態
// ============================================================
static void connection_handler(bool connected) {
    s_is_connected = connected;
    if (s_conn_callback) {
        s_conn_callback(connected);
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "藍牙連線狀態: %s",
            connected ? "已連線" : "已斷線");
}

// ============================================================
// 公開 API
// ============================================================

void messaging_init(ImagesInitCallback init_cb,
                    ImageChunkCallback chunk_cb,
                    SettingsSyncCallback settings_cb,
                    ConnectionChangeCallback conn_cb) {
    s_init_callback     = init_cb;
    s_chunk_callback    = chunk_cb;
    s_settings_callback = settings_cb;
    s_conn_callback     = conn_cb;

    app_message_register_inbox_received(inbox_received_handler);
    app_message_register_inbox_dropped(inbox_dropped_handler);
    app_message_register_outbox_sent(outbox_sent_handler);
    app_message_register_outbox_failed(outbox_failed_handler);

    AppMessageResult result = app_message_open(INBOX_SIZE, OUTBOX_SIZE);
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage 開啟失敗: %d", (int)result);
    }

    connection_service_subscribe((ConnectionHandlers){
        .pebble_app_connection_handler = connection_handler
    });
    s_is_connected = connection_service_peek_pebble_app_connection();
}

void messaging_deinit(void) {
    connection_service_unsubscribe();
    app_message_deregister_callbacks();
}

void messaging_request_init(int32_t watch_width,
                             int32_t watch_height,
                             int32_t text_size) {
    if (s_is_sending) {
        // Outbox 忙碌，排入重試（以 page 0 作為哨兵，實際由 init 觸發）
        s_retry_pending  = true;
        s_retry_page_num = -1;  // 特殊值：代表重試 init
        return;
    }
    if (!s_is_connected) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "藍牙未連線，無法發送初始化請求");
        return;
    }

    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);
    if (result != APP_MSG_OK) return;

    dict_write_int32(iter, KEY_COMMAND,      CMD_REQUEST_TEXT);
    dict_write_int32(iter, KEY_WATCH_WIDTH,  watch_width);
    dict_write_int32(iter, KEY_WATCH_HEIGHT, watch_height);
    dict_write_int32(iter, KEY_TEXT_SIZE,    text_size);

    result = app_message_outbox_send();
    if (result == APP_MSG_OK) {
        s_is_sending = true;
        APP_LOG(APP_LOG_LEVEL_INFO,
                "發送初始化請求: %ldx%ld, size=%ld",
                (long)watch_width, (long)watch_height, (long)text_size);
    }
}

void messaging_request_page(int32_t page_num) {
    if (s_is_sending) {
        s_retry_pending  = true;
        s_retry_page_num = page_num;
        return;
    }
    if (!s_is_connected) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "藍牙未連線，無法請求頁面 %ld", (long)page_num);
        return;
    }

    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_begin 失敗: %d", (int)result);
        return;
    }

    dict_write_int32(iter, KEY_COMMAND,  CMD_REQUEST_PAGE);
    dict_write_int32(iter, KEY_PAGE_NUM, page_num);

    result = app_message_outbox_send();
    if (result == APP_MSG_OK) {
        s_is_sending = true;
        APP_LOG(APP_LOG_LEVEL_DEBUG, "已請求頁面 %ld", (long)page_num);
    } else {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_send 失敗: %d", (int)result);
    }
}

void messaging_send_settings(int32_t scroll_speed, int32_t text_size) {
    if (!s_is_connected || s_is_sending) return;

    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);
    if (result != APP_MSG_OK) return;

    dict_write_int32(iter, KEY_COMMAND,      CMD_SYNC_SETTINGS);
    dict_write_int32(iter, KEY_SCROLL_SPEED, scroll_speed);
    dict_write_int32(iter, KEY_TEXT_SIZE,    text_size);

    result = app_message_outbox_send();
    if (result == APP_MSG_OK) {
        s_is_sending = true;
    }
}

bool messaging_is_connected(void) {
    return s_is_connected;
}
