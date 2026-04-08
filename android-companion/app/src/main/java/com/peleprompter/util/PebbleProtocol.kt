package com.peleprompter.util

import java.util.UUID

/**
 * PebbleProtocol - 通訊協定常數
 *
 * 與 Pebble 手錶端 constants.h 保持完全一致。
 * 圖片渲染模式：手機渲染文字成 1-bit 圖片後分頁傳送給手錶。
 */
object PebbleProtocol {
    /** Pebble App UUID - 必須與 package.json 中的一致 */
    val PEBBLE_APP_UUID: UUID = UUID.fromString("1ced8e88-c6d6-476d-8f55-dc51edf6d9a7")

    // ── AppMessage 字典鍵值 ─────────────────────────────────────
    const val KEY_COMMAND       = 0
    const val KEY_TEXT_OFFSET   = 1   // 保留相容
    const val KEY_TEXT_CHUNK    = 2   // 保留相容
    const val KEY_SCROLL_SPEED  = 3
    const val KEY_TEXT_SIZE     = 4
    // 圖片模式新增鍵值
    const val KEY_PAGE_NUM      = 5
    const val KEY_CHUNK_INDEX   = 6
    const val KEY_TOTAL_CHUNKS  = 7
    const val KEY_IMAGE_DATA    = 8
    const val KEY_TOTAL_PAGES   = 9
    const val KEY_WATCH_WIDTH   = 10
    const val KEY_WATCH_HEIGHT  = 11

    // ── 指令類型 ────────────────────────────────────────────────
    const val CMD_REQUEST_TEXT    = 0   // 手錶初始化請求（含螢幕尺寸）
    const val CMD_SEND_TEXT       = 1   // 保留相容
    const val CMD_SYNC_SETTINGS   = 2   // 同步設定
    const val CMD_INIT_IMAGES     = 3   // 手機→手錶：總頁數 + 尺寸
    const val CMD_REQUEST_PAGE    = 4   // 手錶→手機：請求第 N 頁
    const val CMD_SEND_IMAGE_CHUNK = 5  // 手機→手錶：圖片資料區塊

    // ── 圖片傳輸設定 ─────────────────────────────────────────────
    /**
     * 每個 AppMessage 圖片資料區塊的最大位元組數。
     * Pebble 手錶收件匣 = 2048 bytes，扣除 header 開銷約 148 bytes。
     */
    const val IMAGE_CHUNK_DATA_SIZE = 1900

    /** 連續傳送圖片區塊之間的延遲（ms），防止手錶收件匣溢位 */
    const val CHUNK_SEND_DELAY_MS = 60L

    // ── 文字大小常數 ─────────────────────────────────────────────
    const val TEXT_SIZE_SMALL  = 0
    const val TEXT_SIZE_MEDIUM = 1
    const val TEXT_SIZE_LARGE  = 2
}
