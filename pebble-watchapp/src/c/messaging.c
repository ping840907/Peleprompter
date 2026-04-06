/**
 * messaging.c - AppMessage 通訊模組實作
 *
 * 處理手錶與 Android 手機之間的雙向通訊。
 */

#include "messaging.h"
#include <string.h>
#include <stdlib.h>

// ============================================================
// 私有狀態
// ============================================================
static TextChunkReceivedCallback s_text_callback = NULL;
static SettingsSyncCallback s_settings_callback = NULL;
static ConnectionChangeCallback s_conn_callback = NULL;

static bool s_is_connected = false;
static bool s_is_sending = false;      // 防止重複發送

// 重試佇列 (簡易實作：只記住最後一次失敗的請求)
static bool s_retry_pending = false;
static int32_t s_retry_offset = 0;

#include "constants.h" // CHUNK_SIZE & ENABLE_MOCK_MODE

#if ENABLE_MOCK_MODE
// ============================================================
// Mock Mode (離線測試用)
// ============================================================
static const char *s_mock_text = "Peleprompter Offline Test Mode.\n\n"
"Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.\n\n"
"Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni dolores eos qui ratione voluptatem sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci velit, sed quia non numquam eius modi tempora incidunt ut labore et dolore magnam aliquam quaerat voluptatem.\n\n"
"At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium voluptatum deleniti atque corrupti quos dolores et quas molestias excepturi sint occaecati cupiditate non provident, similique sunt in culpa qui officia deserunt mollitia animi, id est laborum et dolorum fuga.\n\n"
"End of mock text.";

static void mock_text_timer_callback(void *data) {
    int32_t offset = (int32_t)(uintptr_t)data;
    if (!s_text_callback) return;

    int32_t mock_len = (int32_t)strlen(s_mock_text);
    if (offset >= mock_len) {
        s_text_callback(offset, "", 0); // EOF
        return;
    }

    int32_t send_len = CHUNK_SIZE;
    if (offset + send_len > mock_len) {
        send_len = mock_len - offset;
    }

    char *buf = malloc(send_len + 1);
    if (!buf) return;
    strncpy(buf, s_mock_text + offset, send_len);
    buf[send_len] = '\0';

    s_text_callback(offset, buf, send_len);
    free(buf);
}
#endif

// ============================================================
// 內部：處理收到的訊息
// ============================================================
static void inbox_received_handler(DictionaryIterator *iterator, void *context) {
    // 讀取指令類型
    Tuple *cmd_tuple = dict_find(iterator, KEY_COMMAND);
    if (!cmd_tuple) return;

    int32_t command = cmd_tuple->value->int32;

    switch (command) {
        case CMD_SEND_TEXT: {
            // 手機回傳文字區塊
            Tuple *offset_tuple = dict_find(iterator, KEY_TEXT_OFFSET);
            Tuple *text_tuple = dict_find(iterator, KEY_TEXT_CHUNK);

            if (offset_tuple && text_tuple && s_text_callback) {
                int32_t offset = offset_tuple->value->int32;
                const char *text = text_tuple->value->cstring;
                int32_t len = (int32_t)strlen(text);
                s_text_callback(offset, text, len);
            }
            break;
        }

        case CMD_SYNC_SETTINGS: {
            // 手機推送設定
            Tuple *speed_tuple = dict_find(iterator, KEY_SCROLL_SPEED);
            Tuple *size_tuple = dict_find(iterator, KEY_TEXT_SIZE);

            if (s_settings_callback) {
                int32_t speed = speed_tuple ? speed_tuple->value->int32 : -1;
                int32_t size = size_tuple ? size_tuple->value->int32 : -1;
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

    // 如果有待重試的請求，現在送出
    if (s_retry_pending) {
        s_retry_pending = false;
        messaging_request_text(s_retry_offset);
    }
}

static void outbox_failed_handler(DictionaryIterator *iterator,
                                  AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "發送失敗: %d", (int)reason);
    s_is_sending = false;

    // 如果已有待重試的偏移量且未超過重試上限，延遲重試
    if (s_retry_pending) {
        // retry_pending 已由先前的 messaging_request_text 設定
        // 這裡不清除它，讓 outbox_sent_handler 在下次成功時處理
        // 但也不能無限重試，所以透過 s_retry_pending 本身限制為一次
        int32_t offset_to_retry = s_retry_offset;
        s_retry_pending = false;
        messaging_request_text(offset_to_retry);
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

void messaging_init(TextChunkReceivedCallback text_cb,
                    SettingsSyncCallback settings_cb,
                    ConnectionChangeCallback conn_cb) {
    s_text_callback = text_cb;
    s_settings_callback = settings_cb;
    s_conn_callback = conn_cb;

    // 註冊 AppMessage 回呼
    app_message_register_inbox_received(inbox_received_handler);
    app_message_register_inbox_dropped(inbox_dropped_handler);
    app_message_register_outbox_sent(outbox_sent_handler);
    app_message_register_outbox_failed(outbox_failed_handler);

    // 開啟 AppMessage
    AppMessageResult result = app_message_open(INBOX_SIZE, OUTBOX_SIZE);
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage 開啟失敗: %d", (int)result);
    }

    // 註冊藍牙連線監聽
    connection_service_subscribe((ConnectionHandlers){
        .pebble_app_connection_handler = connection_handler
    });
    s_is_connected = connection_service_peek_pebble_app_connection();
}

void messaging_deinit(void) {
    connection_service_unsubscribe();
    app_message_deregister_callbacks();
}

void messaging_request_text(int32_t offset) {
#if ENABLE_MOCK_MODE
    // 無視藍牙連線狀態，一律模擬網路延遲後回傳假資料
    app_timer_register(300, mock_text_timer_callback, (void *)(uintptr_t)offset);
    return;
#endif

    if (s_is_sending) {
        // 佇列這個請求，等目前的發送完成
        s_retry_pending = true;
        s_retry_offset = offset;
        return;
    }

    if (!s_is_connected) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "藍牙未連線，無法請求文字");
        return;
    }

    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_begin 失敗: %d", (int)result);
        return;
    }

    dict_write_int32(iter, KEY_COMMAND, CMD_REQUEST_TEXT);
    dict_write_int32(iter, KEY_TEXT_OFFSET, offset);

    result = app_message_outbox_send();
    if (result == APP_MSG_OK) {
        s_is_sending = true;
    } else {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_send 失敗: %d", (int)result);
    }
}

void messaging_send_settings(int32_t scroll_speed, int32_t text_size) {
    if (!s_is_connected) return;

    if (s_is_sending) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "Outbox 忙碌中，延遲發送設定");
        // Outbox 正在使用中，跳過此次同步
        // 使用者可從設定視窗再次觸發
        return;
    }

    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "設定同步 outbox_begin 失敗: %d", (int)result);
        return;
    }

    dict_write_int32(iter, KEY_COMMAND, CMD_SYNC_SETTINGS);
    dict_write_int32(iter, KEY_SCROLL_SPEED, scroll_speed);
    dict_write_int32(iter, KEY_TEXT_SIZE, text_size);

    result = app_message_outbox_send();
    if (result == APP_MSG_OK) {
        s_is_sending = true;
    }
}

bool messaging_is_connected(void) {
    return s_is_connected;
}
