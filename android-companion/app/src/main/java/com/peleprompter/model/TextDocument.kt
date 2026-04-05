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
    var bookmarkOffset: Int = 0,     // 閱讀書籤（字元偏移量）
    val importedAt: Long = System.currentTimeMillis(), // 匯入時間戳
    val sourcePath: String? = null   // 原始檔案路徑（若為 .txt 匯入）
) {
    /** 文字總長度 */
    val totalLength: Int get() = content.length

    /** 是否有已儲存的閱讀進度 */
    val hasBookmark: Boolean get() = bookmarkOffset > 0

    /**
     * 取得指定偏移量開始的文字區塊
     * @param offset 字元偏移量
     * @param chunkSize 區塊大小
     * @return 文字區塊，若超出範圍則回傳空字串
     */
    fun getChunk(offset: Int, chunkSize: Int): String {
        if (offset < 0 || offset >= content.length) return ""
        val end = minOf(offset + chunkSize, content.length)
        return content.substring(offset, end)
    }
}

/**
 * WatchSettings - 手錶端設定
 */
data class WatchSettings(
    var scrollSpeed: Int = 3,        // 捲動速度 1-6
    var textSize: Int = 1            // 文字大小 0=小, 1=中, 2=大
) {
    companion object {
        const val SPEED_MIN = 1
        const val SPEED_MAX = 6
        const val SIZE_SMALL = 0
        const val SIZE_MEDIUM = 1
        const val SIZE_LARGE = 2
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
    val contentPreview: String       // 前 100 字元預覽
)
