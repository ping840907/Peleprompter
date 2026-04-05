/**
 * ring_buffer.h - 環形文字緩衝區
 * 
 * 管理從 Android 端接收的文字區塊，支援雙向捲動。
 * 以環形方式重複利用記憶體，避免頻繁分配。
 */
#pragma once

#include <pebble.h>
#include "constants.h"

// ============================================================
// 區塊節點結構
// ============================================================
typedef struct TextChunkNode {
    int32_t offset;          // 此區塊在原文中的字元偏移量
    int32_t length;          // 此區塊的字元長度
    char *text;              // 指向文字內容的指標
    struct TextChunkNode *next;
    struct TextChunkNode *prev;
} TextChunkNode;

// ============================================================
// 環形緩衝區結構
// ============================================================
typedef struct {
    TextChunkNode *head;     // 最早（最上方）的區塊
    TextChunkNode *tail;     // 最新（最下方）的區塊
    int32_t count;           // 目前的區塊數量
    int32_t total_bytes;     // 目前佔用的總位元組數
    int32_t total_text_len;  // 所有區塊的文字總長度
    bool reached_end;        // 是否已到達原文結尾
    bool reached_start;      // 是否已到達原文開頭
} RingBuffer;

// ============================================================
// API
// ============================================================

/** 初始化環形緩衝區 */
void ring_buffer_init(RingBuffer *rb);

/** 釋放環形緩衝區所有記憶體 */
void ring_buffer_destroy(RingBuffer *rb);

/** 
 * 在尾端追加一個新區塊 (向下載入)
 * @param rb     緩衝區指標
 * @param offset 此區塊在原文中的偏移量
 * @param text   文字內容 (會被複製)
 * @param len    文字長度
 * @return true  成功追加
 */
bool ring_buffer_append(RingBuffer *rb, int32_t offset, const char *text, int32_t len);

/**
 * 在頭部插入一個新區塊 (向上載入)
 * @param rb     緩衝區指標
 * @param offset 此區塊在原文中的偏移量
 * @param text   文字內容 (會被複製)
 * @param len    文字長度
 * @return true  成功插入
 */
bool ring_buffer_prepend(RingBuffer *rb, int32_t offset, const char *text, int32_t len);

/**
 * 當緩衝區超過容量限制時，從頭部或尾部移除區塊
 * @param rb          緩衝區指標
 * @param from_head   true = 移除最舊(頂部), false = 移除最新(底部)
 */
void ring_buffer_evict(RingBuffer *rb, bool from_head);

/**
 * 取得目前緩衝區中最前面區塊的偏移量
 * @return 偏移量，若緩衝區為空則回傳 -1
 */
int32_t ring_buffer_get_head_offset(const RingBuffer *rb);

/**
 * 取得目前緩衝區中最後一個區塊的結束偏移量 (offset + length)
 * @return 結束偏移量，若緩衝區為空則回傳 0
 */
int32_t ring_buffer_get_tail_end_offset(const RingBuffer *rb);

/**
 * 將緩衝區內所有文字串接成一個完整字串
 * 呼叫者必須負責釋放回傳的指標
 * @param rb  緩衝區指標
 * @return 串接後的字串 (需 free)，失敗回傳 NULL
 */
char *ring_buffer_flatten(const RingBuffer *rb);

/**
 * 取得目前緩衝區佔用的位元組數
 */
int32_t ring_buffer_get_used_bytes(const RingBuffer *rb);
