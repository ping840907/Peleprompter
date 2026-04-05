/**
 * ring_buffer.c - 環形文字緩衝區實作
 *
 * 使用雙向鏈結串列管理文字區塊，當總記憶體超過上限時
 * 自動淘汰最舊/最新的區塊。
 */

#include "ring_buffer.h"

// ============================================================
// 內部輔助函式
// ============================================================

/** 建立新的區塊節點並複製文字內容 */
static TextChunkNode *create_node(int32_t offset, const char *text, int32_t len) {
    TextChunkNode *node = malloc(sizeof(TextChunkNode));
    if (!node) return NULL;

    node->text = malloc(len + 1);
    if (!node->text) {
        free(node);
        return NULL;
    }

    memcpy(node->text, text, len);
    node->text[len] = '\0';
    node->offset = offset;
    node->length = len;
    node->next = NULL;
    node->prev = NULL;
    return node;
}

/** 釋放單一節點的記憶體 */
static void free_node(TextChunkNode *node) {
    if (node) {
        if (node->text) free(node->text);
        free(node);
    }
}

// ============================================================
// 公開 API 實作
// ============================================================

void ring_buffer_init(RingBuffer *rb) {
    rb->head = NULL;
    rb->tail = NULL;
    rb->count = 0;
    rb->total_bytes = 0;
    rb->total_text_len = 0;
    rb->reached_end = false;
    rb->reached_start = false;
}

void ring_buffer_destroy(RingBuffer *rb) {
    TextChunkNode *current = rb->head;
    while (current) {
        TextChunkNode *next = current->next;
        free_node(current);
        current = next;
    }
    ring_buffer_init(rb);  // 重設所有欄位
}

bool ring_buffer_append(RingBuffer *rb, int32_t offset, const char *text, int32_t len) {
    if (!text || len <= 0) return false;

    // 檢查是否需要先淘汰舊區塊以騰出空間
    while (rb->total_bytes + len + (int32_t)sizeof(TextChunkNode) > RING_BUFFER_CAPACITY
           && rb->count > 0) {
        ring_buffer_evict(rb, true);  // 從頭部淘汰
    }

    TextChunkNode *node = create_node(offset, text, len);
    if (!node) return false;

    if (rb->tail) {
        rb->tail->next = node;
        node->prev = rb->tail;
        rb->tail = node;
    } else {
        // 緩衝區為空
        rb->head = node;
        rb->tail = node;
    }

    rb->count++;
    rb->total_bytes += len + (int32_t)sizeof(TextChunkNode);
    rb->total_text_len += len;
    return true;
}

bool ring_buffer_prepend(RingBuffer *rb, int32_t offset, const char *text, int32_t len) {
    if (!text || len <= 0) return false;

    // 檢查是否需要先淘汰新區塊以騰出空間
    while (rb->total_bytes + len + (int32_t)sizeof(TextChunkNode) > RING_BUFFER_CAPACITY
           && rb->count > 0) {
        ring_buffer_evict(rb, false);  // 從尾部淘汰
    }

    TextChunkNode *node = create_node(offset, text, len);
    if (!node) return false;

    if (rb->head) {
        rb->head->prev = node;
        node->next = rb->head;
        rb->head = node;
    } else {
        rb->head = node;
        rb->tail = node;
    }

    rb->count++;
    rb->total_bytes += len + (int32_t)sizeof(TextChunkNode);
    rb->total_text_len += len;
    return true;
}

void ring_buffer_evict(RingBuffer *rb, bool from_head) {
    if (rb->count == 0) return;

    TextChunkNode *victim;
    if (from_head) {
        victim = rb->head;
        rb->head = victim->next;
        if (rb->head) {
            rb->head->prev = NULL;
        } else {
            rb->tail = NULL;
        }
        // 從頭部淘汰表示我們不再持有最前面的區塊
        rb->reached_start = false;
    } else {
        victim = rb->tail;
        rb->tail = victim->prev;
        if (rb->tail) {
            rb->tail->next = NULL;
        } else {
            rb->head = NULL;
        }
        // 從尾部淘汰表示我們不再持有最後面的區塊
        rb->reached_end = false;
    }

    rb->count--;
    rb->total_bytes -= (victim->length + (int32_t)sizeof(TextChunkNode));
    rb->total_text_len -= victim->length;
    free_node(victim);
}

int32_t ring_buffer_get_head_offset(const RingBuffer *rb) {
    return rb->head ? rb->head->offset : -1;
}

int32_t ring_buffer_get_tail_end_offset(const RingBuffer *rb) {
    if (!rb->tail) return 0;
    return rb->tail->offset + rb->tail->length;
}

char *ring_buffer_flatten(const RingBuffer *rb) {
    if (rb->count == 0 || rb->total_text_len == 0) return NULL;

    char *result = malloc(rb->total_text_len + 1);
    if (!result) return NULL;

    char *ptr = result;
    TextChunkNode *current = rb->head;
    while (current) {
        memcpy(ptr, current->text, current->length);
        ptr += current->length;
        current = current->next;
    }
    *ptr = '\0';
    return result;
}

int32_t ring_buffer_get_used_bytes(const RingBuffer *rb) {
    return rb->total_bytes;
}
