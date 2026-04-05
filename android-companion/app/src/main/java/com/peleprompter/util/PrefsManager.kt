package com.peleprompter.util

import android.content.Context
import android.content.SharedPreferences
import com.google.gson.Gson
import com.google.gson.reflect.TypeToken
import com.peleprompter.model.ImportHistoryEntry
import com.peleprompter.model.WatchSettings

/**
 * PrefsManager - 持久化管理器
 *
 * 使用 SharedPreferences 儲存：
 * - .txt 匯入歷史（含書籤）
 * - 手錶設定（速度、字型大小）
 * - 最後使用的文件 ID
 */
class PrefsManager(context: Context) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val gson = Gson()

    companion object {
        private const val PREFS_NAME = "peleprompter_prefs"
        private const val KEY_IMPORT_HISTORY = "import_history"
        private const val KEY_WATCH_SETTINGS = "watch_settings"
        private const val KEY_LAST_DOC_ID = "last_document_id"
        private const val KEY_BOOKMARKS = "bookmarks"       // Map<docId, offset>
        private const val MAX_HISTORY_SIZE = 50
    }

    // ================================================================
    // 匯入歷史
    // ================================================================

    /** 取得所有匯入歷史（按時間倒序） */
    fun getImportHistory(): List<ImportHistoryEntry> {
        val json = prefs.getString(KEY_IMPORT_HISTORY, null) ?: return emptyList()
        val type = object : TypeToken<List<ImportHistoryEntry>>() {}.type
        return try {
            gson.fromJson<List<ImportHistoryEntry>>(json, type)
                .sortedByDescending { it.importedAt }
        } catch (e: Exception) {
            emptyList()
        }
    }

    /** 新增或更新匯入歷史項目 */
    fun saveImportHistoryEntry(entry: ImportHistoryEntry) {
        val history = getImportHistory().toMutableList()

        // 移除同 ID 的舊項目
        history.removeAll { it.id == entry.id }
        // 加入新項目
        history.add(0, entry)

        // 限制歷史數量
        val trimmed = if (history.size > MAX_HISTORY_SIZE) {
            history.take(MAX_HISTORY_SIZE)
        } else {
            history
        }

        prefs.edit()
            .putString(KEY_IMPORT_HISTORY, gson.toJson(trimmed))
            .apply()
    }

    /** 刪除指定歷史項目 */
    fun removeHistoryEntry(docId: String) {
        val history = getImportHistory().toMutableList()
        history.removeAll { it.id == docId }
        prefs.edit()
            .putString(KEY_IMPORT_HISTORY, gson.toJson(history))
            .apply()
        // 同時清除書籤
        removeBookmark(docId)
    }

    // ================================================================
    // 書籤（閱讀進度）
    // ================================================================

    /** 取得所有書籤 */
    private fun getAllBookmarks(): MutableMap<String, Int> {
        val json = prefs.getString(KEY_BOOKMARKS, null) ?: return mutableMapOf()
        val type = object : TypeToken<MutableMap<String, Int>>() {}.type
        return try {
            gson.fromJson(json, type) ?: mutableMapOf()
        } catch (e: Exception) {
            mutableMapOf()
        }
    }

    /** 儲存書籤 */
    fun saveBookmark(docId: String, offset: Int) {
        val bookmarks = getAllBookmarks()
        bookmarks[docId] = offset
        prefs.edit()
            .putString(KEY_BOOKMARKS, gson.toJson(bookmarks))
            .apply()
    }

    /** 取得指定文件的書籤偏移量，若不存在回傳 0 */
    fun getBookmark(docId: String): Int {
        return getAllBookmarks()[docId] ?: 0
    }

    /** 移除指定文件的書籤 */
    fun removeBookmark(docId: String) {
        val bookmarks = getAllBookmarks()
        bookmarks.remove(docId)
        prefs.edit()
            .putString(KEY_BOOKMARKS, gson.toJson(bookmarks))
            .apply()
    }

    // ================================================================
    // 手錶設定
    // ================================================================

    /** 儲存手錶設定 */
    fun saveWatchSettings(settings: WatchSettings) {
        prefs.edit()
            .putString(KEY_WATCH_SETTINGS, gson.toJson(settings))
            .apply()
    }

    /** 取得手錶設定 */
    fun getWatchSettings(): WatchSettings {
        val json = prefs.getString(KEY_WATCH_SETTINGS, null) ?: return WatchSettings()
        return try {
            gson.fromJson(json, WatchSettings::class.java)
        } catch (e: Exception) {
            WatchSettings()
        }
    }

    // ================================================================
    // 最後使用的文件
    // ================================================================

    fun saveLastDocId(docId: String) {
        prefs.edit().putString(KEY_LAST_DOC_ID, docId).apply()
    }

    fun getLastDocId(): String? {
        return prefs.getString(KEY_LAST_DOC_ID, null)
    }
}
