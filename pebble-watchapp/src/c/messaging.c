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

// ── 待發送請求佇列 ───────────────────────────────────────────
// 取代過去的單槽重試：當 outbox 忙碌時，所有外送請求（init / page /
// settings）都進入此環形佇列依序發送，避免後到的請求覆蓋前一個待發者，
// 也避免設定同步在忙碌時被靜默丟棄。
typedef enum { REQ_INIT, REQ_PAGE, REQ_SETTINGS } ReqType;

typedef struct {
    ReqType type;
    int32_t p1, p2, p3;   // INIT: w,h,size  PAGE: page  SETTINGS: speed,size
} PendingReq;

#define PENDING_CAP 8
static PendingReq s_pending[PENDING_CAP];
static int        s_pending_head  = 0;
static int        s_pending_count = 0;

static void pending_push(ReqType type, int32_t p1, int32_t p2, int32_t p3) {
    if (s_pending_count >= PENDING_CAP) {
        // 佇列已滿：丟棄最舊一筆以容納最新請求（最新狀態通常較重要）
        s_pending_head = (s_pending_head + 1) % PENDING_CAP;
        s_pending_count--;
        APP_LOG(APP_LOG_LEVEL_WARNING, "待發送佇列已滿，丟棄最舊請求");
    }
    int tail = (s_pending_head + s_pending_count) % PENDING_CAP;
    s_pending[tail] = (PendingReq){ type, p1, p2, p3 };
    s_pending_count++;
}

static bool pending_pop(PendingReq *out) {
    if (s_pending_count == 0) return false;
    *out = s_pending[s_pending_head];
    s_pending_head = (s_pending_head + 1) % PENDING_CAP;
    s_pending_count--;
    return true;
}

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
            Tuple *start_t  = dict_find(iterator, KEY_START_PAGE);

            if (pages_t && width_t && height_t && s_init_callback) {
                int32_t total_pages  = pages_t->value->int32;
                int32_t page_width   = width_t->value->int32;
                int32_t page_height  = height_t->value->int32;
                int32_t start_page   = start_t ? start_t->value->int32 : 0;
                APP_LOG(APP_LOG_LEVEL_INFO,
                        "收到 INIT_IMAGES: %ld 頁, %ldx%ld, 起始頁 %ld",
                        (long)total_pages, (long)page_width, (long)page_height,
                        (long)start_page);
                s_init_callback(total_pages, page_width, page_height, start_page);
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
// 內部：實際送出與佇列幫浦
// ============================================================

/** 依請求型態組裝並送出一筆 AppMessage；成功回傳 true。 */
static bool actually_send(const PendingReq *req) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_begin 失敗");
        return false;
    }
    switch (req->type) {
        case REQ_INIT:
            dict_write_int32(iter, KEY_COMMAND,      CMD_REQUEST_TEXT);
            dict_write_int32(iter, KEY_WATCH_WIDTH,  req->p1);
            dict_write_int32(iter, KEY_WATCH_HEIGHT, req->p2);
            dict_write_int32(iter, KEY_TEXT_SIZE,    req->p3);
            break;
        case REQ_PAGE:
            dict_write_int32(iter, KEY_COMMAND,  CMD_REQUEST_PAGE);
            dict_write_int32(iter, KEY_PAGE_NUM, req->p1);
            break;
        case REQ_SETTINGS:
            dict_write_int32(iter, KEY_COMMAND,      CMD_SYNC_SETTINGS);
            dict_write_int32(iter, KEY_SCROLL_SPEED, req->p1);
            dict_write_int32(iter, KEY_TEXT_SIZE,    req->p2);
            break;
    }
    AppMessageResult result = app_message_outbox_send();
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_send 失敗: %d", (int)result);
        return false;
    }
    return true;
}

/** 若 outbox 空閒且已連線，從佇列取出下一筆送出。 */
static void pump_queue(void) {
    if (s_is_sending || !s_is_connected) return;
    PendingReq req;
    while (pending_pop(&req)) {
        if (actually_send(&req)) {
            s_is_sending = true;
            return;
        }
        // 送出失敗：丟棄該筆並嘗試下一筆，避免卡住整條佇列
    }
}

// ============================================================
// 內部：處理發送結果
// ============================================================
static void outbox_sent_handler(DictionaryIterator *iterator, void *context) {
    s_is_sending = false;
    pump_queue();
}

static void outbox_failed_handler(DictionaryIterator *iterator,
                                  AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "發送失敗: %d", (int)reason);
    s_is_sending = false;
    pump_queue();
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
    // 重新連線後嘗試送出仍滯留佇列中的請求
    if (connected) pump_queue();
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
    if (!s_is_connected) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "藍牙未連線，無法發送初始化請求");
        return;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "排入初始化請求: %ldx%ld, size=%ld",
            (long)watch_width, (long)watch_height, (long)text_size);
    pending_push(REQ_INIT, watch_width, watch_height, text_size);
    pump_queue();
}

void messaging_request_page(int32_t page_num) {
    if (!s_is_connected) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "藍牙未連線，無法請求頁面 %ld", (long)page_num);
        return;
    }
    APP_LOG(APP_LOG_LEVEL_DEBUG, "排入頁面請求 %ld", (long)page_num);
    pending_push(REQ_PAGE, page_num, 0, 0);
    pump_queue();
}

void messaging_send_settings(int32_t scroll_speed, int32_t text_size) {
    if (!s_is_connected) return;
    pending_push(REQ_SETTINGS, scroll_speed, text_size, 0);
    pump_queue();
}

bool messaging_is_connected(void) {
    return s_is_connected;
}
