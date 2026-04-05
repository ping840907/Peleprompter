package com.peleprompter.util

import java.util.UUID

/**
 * PebbleProtocol - 通訊協定常數
 *
 * 與 Pebble 手錶端 constants.h 保持完全一致。
 */
object PebbleProtocol {
    /** Pebble App UUID - 必須與 package.json 中的一致 */
    val PEBBLE_APP_UUID: UUID = UUID.fromString("1ced8e88-c6d6-476d-8f55-dc51edf6d9a7")

    // AppMessage 字典鍵值
    const val KEY_COMMAND       = 0
    const val KEY_TEXT_OFFSET   = 1
    const val KEY_TEXT_CHUNK    = 2
    const val KEY_SCROLL_SPEED  = 3
    const val KEY_TEXT_SIZE     = 4

    // 指令類型
    const val CMD_REQUEST_TEXT   = 0   // 手錶請求文字區塊
    const val CMD_SEND_TEXT      = 1   // 手機回傳文字區塊
    const val CMD_SYNC_SETTINGS  = 2   // 同步設定

    // 文字區塊大小（與手錶端 CHUNK_SIZE 對應，但受 AppMessage 大小限制）
    const val DEFAULT_CHUNK_SIZE = 512
}
