/**
 * image_manager.c - 手錶端圖片頁面快取管理員實作
 */

#include "image_manager.h"
#include <string.h>
#include <stdlib.h>

// ============================================================
// 內部輔助：清空單一插槽
// ============================================================
static void slot_clear(PageSlot *slot) {
    if (slot->bitmap) {
        gbitmap_destroy(slot->bitmap);
        slot->bitmap = NULL;
    }
    slot->page_num       = -1;
    slot->is_ready       = false;
    slot->is_loading     = false;
    slot->chunks_received = 0;
    slot->total_chunks   = 0;
}

// ============================================================
// 公開 API
// ============================================================

void image_manager_init(ImageManager *mgr) {
    memset(mgr, 0, sizeof(ImageManager));
    mgr->total_pages = 0;
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        mgr->slots[i].page_num = -1;
        mgr->slots[i].bitmap   = NULL;
    }
}

void image_manager_deinit(ImageManager *mgr) {
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        slot_clear(&mgr->slots[i]);
    }
}

void image_manager_set_dimensions(ImageManager *mgr,
                                   int32_t total_pages,
                                   int32_t page_width,
                                   int32_t page_height) {
    // 釋放所有舊快取
    image_manager_deinit(mgr);

    mgr->total_pages   = total_pages;
    mgr->page_width    = page_width;
    mgr->page_height   = page_height;
    mgr->row_stride    = (page_width + 7) / 8;
    mgr->page_data_size = mgr->row_stride * page_height;

    APP_LOG(APP_LOG_LEVEL_INFO,
            "ImageManager: %ld 頁, %ldx%ld px, stride=%ld, data=%ld bytes/page",
            (long)total_pages, (long)page_width, (long)page_height,
            (long)mgr->row_stride, (long)mgr->page_data_size);
}

PageSlot *image_manager_get_slot(ImageManager *mgr, int32_t page_num) {
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        if (mgr->slots[i].page_num == page_num) {
            return &mgr->slots[i];
        }
    }
    return NULL;
}

PageSlot *image_manager_alloc_slot(ImageManager *mgr, int32_t page_num) {
    if (mgr->page_data_size <= 0) return NULL;

    // 若已有此頁的插槽，直接重用
    PageSlot *existing = image_manager_get_slot(mgr, page_num);
    if (existing) {
        // 重置接收狀態，保留已分配的 bitmap
        existing->is_ready        = false;
        existing->is_loading      = true;
        existing->chunks_received  = 0;
        existing->total_chunks    = 0;
        // 清空 bitmap 資料
        if (existing->bitmap) {
            uint8_t *data = (uint8_t *)gbitmap_get_data(existing->bitmap);
            if (data) memset(data, 0, mgr->page_data_size);
        }
        return existing;
    }

    // 找空置插槽
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        if (mgr->slots[i].page_num == -1) {
            PageSlot *slot = &mgr->slots[i];
            slot->page_num        = page_num;
            slot->is_ready        = false;
            slot->is_loading      = true;
            slot->chunks_received  = 0;
            slot->total_chunks    = 0;
            slot->bitmap = gbitmap_create_blank(
                GSize(mgr->page_width, mgr->page_height),
                GBitmapFormat1Bit
            );
            if (!slot->bitmap) {
                APP_LOG(APP_LOG_LEVEL_ERROR, "無法分配 GBitmap (page %ld)", (long)page_num);
                slot->page_num = -1;
                return NULL;
            }
            return slot;
        }
    }

    // 所有插槽已滿，淘汰離 page_num 最遠的頁面
    int evict_idx = 0;
    int32_t max_dist = 0;
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        int32_t dist = mgr->slots[i].page_num - page_num;
        if (dist < 0) dist = -dist;
        if (dist > max_dist) {
            max_dist  = dist;
            evict_idx = i;
        }
    }

    APP_LOG(APP_LOG_LEVEL_DEBUG, "淘汰頁面 %ld 以載入頁面 %ld",
            (long)mgr->slots[evict_idx].page_num, (long)page_num);

    // 重用已分配的 bitmap 記憶體
    PageSlot *slot = &mgr->slots[evict_idx];
    if (!slot->bitmap) {
        slot->bitmap = gbitmap_create_blank(
            GSize(mgr->page_width, mgr->page_height),
            GBitmapFormat1Bit
        );
        if (!slot->bitmap) {
            APP_LOG(APP_LOG_LEVEL_ERROR, "無法分配 GBitmap (page %ld)", (long)page_num);
            slot->page_num = -1;
            return NULL;
        }
    } else {
        // 清空舊資料
        uint8_t *data = (uint8_t *)gbitmap_get_data(slot->bitmap);
        if (data) memset(data, 0, mgr->page_data_size);
    }

    slot->page_num        = page_num;
    slot->is_ready        = false;
    slot->is_loading      = true;
    slot->chunks_received  = 0;
    slot->total_chunks    = 0;
    return slot;
}

bool image_manager_receive_chunk(ImageManager *mgr,
                                  int32_t page_num,
                                  int32_t chunk_idx,
                                  int32_t total_chunks,
                                  const uint8_t *data,
                                  int32_t data_len) {
    PageSlot *slot = image_manager_get_slot(mgr, page_num);
    if (!slot || !slot->bitmap) {
        APP_LOG(APP_LOG_LEVEL_WARNING,
                "收到未知頁面 %ld 的區塊，忽略", (long)page_num);
        return false;
    }

    if (slot->total_chunks == 0) {
        slot->total_chunks = total_chunks;
    }

    // 寫入 bitmap 資料
    int32_t offset = chunk_idx * IMAGE_CHUNK_DATA_SIZE;
    int32_t capacity = mgr->page_data_size - offset;
    if (capacity <= 0) {
        APP_LOG(APP_LOG_LEVEL_ERROR,
                "區塊偏移 %ld 超出頁面資料大小 %ld",
                (long)offset, (long)mgr->page_data_size);
        return false;
    }
    int32_t write_len = data_len < capacity ? data_len : capacity;
    uint8_t *bmp_data = (uint8_t *)gbitmap_get_data(slot->bitmap);
    if (bmp_data) {
        memcpy(bmp_data + offset, data, write_len);
    }

    slot->chunks_received++;
    APP_LOG(APP_LOG_LEVEL_DEBUG,
            "頁 %ld 區塊 %ld/%ld 已收到",
            (long)page_num, (long)slot->chunks_received, (long)total_chunks);

    if (slot->chunks_received >= total_chunks) {
        slot->is_ready   = true;
        slot->is_loading = false;
        APP_LOG(APP_LOG_LEVEL_INFO, "頁 %ld 已完整載入", (long)page_num);
        return true;
    }
    return false;
}

void image_manager_evict(ImageManager *mgr, int32_t page_num) {
    PageSlot *slot = image_manager_get_slot(mgr, page_num);
    if (slot) slot_clear(slot);
}

bool image_manager_is_ready(ImageManager *mgr, int32_t page_num) {
    PageSlot *slot = image_manager_get_slot(mgr, page_num);
    return slot && slot->is_ready;
}

bool image_manager_is_loading(ImageManager *mgr, int32_t page_num) {
    PageSlot *slot = image_manager_get_slot(mgr, page_num);
    return slot && slot->is_loading;
}

GBitmap *image_manager_get_bitmap(ImageManager *mgr, int32_t page_num) {
    PageSlot *slot = image_manager_get_slot(mgr, page_num);
    if (slot && slot->is_ready) return slot->bitmap;
    return NULL;
}
