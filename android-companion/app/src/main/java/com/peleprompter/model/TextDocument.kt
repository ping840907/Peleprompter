package com.peleprompter.model

/**
 * TextDocument - 文字文件資料模型
 *
 * 儲存文字內容、書籤（閱讀進度）、以及匯入歷史資訊。
 */
data class TextDocument(
    val id: String,                  // 唯一識別碼 (UUID)
    val title: String,               // 檔案名稱或自訂標題
    val content: String,             // 完整文字內容
    var bookmarkOffset: Int = 0,     // 閱讀書籤（位元組偏移量）
    val importedAt: Long = System.currentTimeMillis(), // 匯入時間戳
    val sourcePath: String? = null   // 原始檔案路徑（若為 .txt 匯入）
) {
    /** 快取 UTF-8 位元組陣列，避免重複轉換耗能 */
    private val contentBytes: ByteArray by lazy { content.toByteArray(Charsets.UTF_8) }

    /** 文字總長度（位元組） */
    val totalLength: Int get() = contentBytes.size

    /** 是否有已儲存的閱讀進度 */
    val hasBookmark: Boolean get() = bookmarkOffset > 0

    /**
     * 取得指定位元組偏移量開始的文字區塊
     * @param byteOffset 位元組偏移量 (UTF-8)
     * @param maxBytes 最大位元組數
     * @return 文字區塊，若超出範圍則回傳空字串。保證不切斷多位元組字元。
     */
    fun getChunk(byteOffset: Int, maxBytes: Int): String {
        val bytes = contentBytes
        if (byteOffset < 0 || byteOffset >= bytes.size) return ""

        var end = minOf(byteOffset + maxBytes, bytes.size)

        // 確保不會在多位元組 UTF-8 字元中間切斷
        // UTF-8 接續位元組以 10xxxxxx (0x80-0xBF) 開頭
        while (end > byteOffset && end < bytes.size && (bytes[end].toInt() and 0xC0) == 0x80) {
            end--
        }

        return String(bytes, byteOffset, end - byteOffset, Charsets.UTF_8)
    }
}

/**
 * WatchSettings - 手錶端設定
 */
data class WatchSettings(
    var scrollSpeed: Int = 3,                       // 捲動速度 1-6
    var textSize: Int = SIZE_MEDIUM,                // 文字大小（見下方常數）
    var settingsVersion: Int = 0                    // 用於從舊 3 等級遷移至新 5 等級
) {
    companion object {
        const val SPEED_MIN = 1
        const val SPEED_MAX = 6
        const val SIZE_TINY   = 0   // 10px — 細筆畫（新）
        const val SIZE_SMALL  = 1   // 13px — 細筆畫（新）
        const val SIZE_MEDIUM = 2   // 16px — 原 Small
        const val SIZE_LARGE  = 3   // 22px — 原 Medium
        const val SIZE_XLARGE = 4   // 28px — 原 Large

        /** 設定系統版本：舊存值 settingsVersion < CURRENT_VERSION 時需要遷移 */
        const val CURRENT_VERSION = 1
    }
}

/**
 * ImportHistoryEntry - .txt 匯入歷史項目（用於 SharedPreferences 序列化）
 */
data class ImportHistoryEntry(
    val id: String,
    val title: String,
    val bookmarkOffset: Int,
    val importedAt: Long,
    val sourcePath: String?,
    val contentPreview: String,      // 前 100 字元預覽
    // 來源類型："PASTE" 或 "FILE"。可為 null（舊資料），視為 "FILE"。
    val sourceType: String? = "FILE"
)
