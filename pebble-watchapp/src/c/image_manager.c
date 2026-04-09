/**
 * image_manager.c - 手錶端圖片頁面快取管理員實作
 *
 * 記憶體碎片化防護設計：
 * 1. 預分配：set_dimensions 時一次性分配全部 MAX_PAGES_CACHED 個 GBitmap，
 *    確保它們在堆積上緊鄰，避免後續插入其他物件而破壞連續性。
 * 2. 永不在淘汰時釋放 GBitmap：evict 只重置狀態並清零像素資料，
 *    GBitmap 物件本身保持在固定的堆積位置直到 app 結束。
 * 3. 僅在尺寸真正改變時才執行完整的 destroy→recreate：
 *    同一塊手錶切換字型大小時（螢幕解析度不變），跳過 destroy 直接清零資料，
 *    消除無意義的 destroy→create 循環。
 */

#include "image_manager.h"
#include <string.h>
#include <stdlib.h>

// ============================================================
// 內部輔助
// ============================================================

/**
 * 完全釋放插槽（包含 GBitmap）：僅用於 deinit 路徑。
 */
static void slot_destroy(PageSlot *slot) {
    if (slot->bitmap) {
        gbitmap_destroy(slot->bitmap);
        slot->bitmap = NULL;
    }
    slot->page_num        = -1;
    slot->is_ready        = false;
    slot->is_loading      = false;
    slot->chunks_received = 0;
    slot->total_chunks    = 0;
}

/**
 * 重置插槽狀態（不釋放 GBitmap）：用於淘汰與 soft reset 路徑。
 * GBitmap 留在原位，僅清零像素資料。
 */
static void slot_reset(PageSlot *slot, int32_t page_data_size) {
    if (slot->bitmap) {
        uint8_t *data = (uint8_t *)gbitmap_get_data(slot->bitmap);
        if (data) memset(data, 0, page_data_size);
    }
    slot->page_num        = -1;
    slot->is_ready        = false;
    slot->is_loading      = false;
    slot->chunks_received = 0;
    slot->total_chunks    = 0;
}

/**
 * 預分配所有空置插槽的 GBitmap。
 * 在 set_dimensions 確定尺寸後立即呼叫，讓所有 bitmap 在堆積上連續分配。
 */
static void prewarm_slots(ImageManager *mgr) {
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        if (!mgr->slots[i].bitmap) {
            mgr->slots[i].bitmap = gbitmap_create_blank(
                GSize(mgr->page_width, mgr->page_height),
                GBitmapFormat1Bit
            );
            if (!mgr->slots[i].bitmap) {
                APP_LOG(APP_LOG_LEVEL_ERROR,
                        "prewarm: 插槽 %d 無法分配 GBitmap", i);
            }
        }
    }
}

// ============================================================
// 公開 API
// ============================================================

void image_manager_init(ImageManager *mgr) {
    memset(mgr, 0, sizeof(ImageManager));
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        mgr->slots[i].page_num = -1;
        mgr->slots[i].bitmap   = NULL;
    }
}

void image_manager_deinit(ImageManager *mgr) {
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        slot_destroy(&mgr->slots[i]);
    }
    mgr->total_pages    = 0;
    mgr->page_width     = 0;
    mgr->page_height    = 0;
    mgr->row_stride     = 0;
    mgr->page_data_size = 0;
}

void image_manager_set_dimensions(ImageManager *mgr,
                                   int32_t total_pages,
                                   int32_t page_width,
                                   int32_t page_height) {
    int32_t new_stride    = (page_width + 7) / 8;
    int32_t new_data_size = new_stride * page_height;

    bool dims_changed = (mgr->page_width     != page_width  ||
                         mgr->page_height    != page_height ||
                         mgr->page_data_size == 0);

    if (dims_changed) {
        // 螢幕解析度改變（不同手錶型號）：必須重建 GBitmap
        APP_LOG(APP_LOG_LEVEL_INFO,
                "尺寸改變 %ldx%ld → %ldx%ld，重建 GBitmap",
                (long)mgr->page_width, (long)mgr->page_height,
                (long)page_width,       (long)page_height);

        // 先完整釋放舊 bitmap
        for (int i = 0; i < MAX_PAGES_CACHED; i++) {
            slot_destroy(&mgr->slots[i]);
        }

        // 更新尺寸
        mgr->page_width     = page_width;
        mgr->page_height    = page_height;
        mgr->row_stride     = new_stride;
        mgr->page_data_size = new_data_size;

        // ── 關鍵：一次性預分配全部插槽 ──────────────────────
        // 所有 GBitmap 大小相同，連續分配可讓堆積分配器將它們緊鄰擺放，
        // 避免後續惰性分配時被其他物件插入縫隙。
        prewarm_slots(mgr);
    } else {
        // 尺寸不變（同一手錶切換字型大小）：只重置狀態，不碰 GBitmap
        // 這避免了不必要的 destroy→create 循環造成碎片。
        APP_LOG(APP_LOG_LEVEL_INFO,
                "尺寸相同 (%ldx%ld)，soft reset（保留 GBitmap）",
                (long)page_width, (long)page_height);

        for (int i = 0; i < MAX_PAGES_CACHED; i++) {
            slot_reset(&mgr->slots[i], mgr->page_data_size);
        }
    }

    mgr->total_pages = total_pages;

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

    // 若已有此頁的插槽，直接重用（不重建 GBitmap）
    PageSlot *existing = image_manager_get_slot(mgr, page_num);
    if (existing) {
        slot_reset(existing, mgr->page_data_size);
        existing->page_num   = page_num;
        existing->is_loading = true;
        return existing;
    }

    // 找空置插槽（prewarm 後這些插槽有 GBitmap 但 page_num == -1）
    for (int i = 0; i < MAX_PAGES_CACHED; i++) {
        if (mgr->slots[i].page_num == -1) {
            PageSlot *slot = &mgr->slots[i];
            // bitmap 應已由 prewarm_slots 預分配；若因 OOM 未分配，在此補建
            if (!slot->bitmap) {
                slot->bitmap = gbitmap_create_blank(
                    GSize(mgr->page_width, mgr->page_height),
                    GBitmapFormat1Bit
                );
                if (!slot->bitmap) {
                    APP_LOG(APP_LOG_LEVEL_ERROR,
                            "alloc_slot: 無法分配 GBitmap (page %ld)", (long)page_num);
                    return NULL;
                }
            } else {
                // 清零舊資料（prewarm 已做過，此為防禦性清除）
                uint8_t *data = (uint8_t *)gbitmap_get_data(slot->bitmap);
                if (data) memset(data, 0, mgr->page_data_size);
            }
            slot->page_num        = page_num;
            slot->is_ready        = false;
            slot->is_loading      = true;
            slot->chunks_received = 0;
            slot->total_chunks    = 0;
            return slot;
        }
    }

    // 所有插槽已滿：淘汰距 page_num 最遠的頁面，但保留其 GBitmap
    int evict_idx   = 0;
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

    // ── 關鍵：不呼叫 gbitmap_destroy，只清零資料 ──────────────
    // 這使 GBitmap 保持在固定堆積位置，不留下「洞」。
    PageSlot *slot = &mgr->slots[evict_idx];
    slot_reset(slot, mgr->page_data_size);
    slot->page_num   = page_num;
    slot->is_loading = true;
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

    int32_t offset   = chunk_idx * IMAGE_CHUNK_DATA_SIZE;
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
    // 只重置狀態，不釋放 GBitmap（防止堆積碎片化）
    PageSlot *slot = image_manager_get_slot(mgr, page_num);
    if (slot) {
        slot_reset(slot, mgr->page_data_size);
    }
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
