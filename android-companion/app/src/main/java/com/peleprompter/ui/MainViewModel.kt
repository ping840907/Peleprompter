package com.peleprompter.ui

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import com.peleprompter.PeleprompterApp
import com.peleprompter.model.ImportHistoryEntry
import com.peleprompter.model.TextDocument
import com.peleprompter.model.WatchSettings
import com.peleprompter.service.PebbleCommManager
import java.io.BufferedReader
import java.io.File
import java.io.FileInputStream
import java.io.InputStreamReader
import java.util.UUID

/**
 * MainViewModel - 主畫面的 ViewModel
 *
 * 管理文件載入、歷史紀錄、手錶通訊狀態。
 */
class MainViewModel(application: Application) : AndroidViewModel(application) {

    private val app = application as PeleprompterApp
    private val prefs = app.prefsManager

    val commManager = PebbleCommManager(application, prefs)

    // 目前載入的文件
    private val _currentDocument = MutableLiveData<TextDocument?>()
    val currentDocument: LiveData<TextDocument?> = _currentDocument

    // 匯入歷史
    private val _importHistory = MutableLiveData<List<ImportHistoryEntry>>()
    val importHistory: LiveData<List<ImportHistoryEntry>> = _importHistory

    // 手錶連線狀態
    private val _watchConnected = MutableLiveData<Boolean>()
    val watchConnected: LiveData<Boolean> = _watchConnected

    // 手錶設定
    private val _watchSettings = MutableLiveData<WatchSettings>()
    val watchSettings: LiveData<WatchSettings> = _watchSettings

    // 書籤更新
    private val _bookmarkOffset = MutableLiveData<Int>()
    val bookmarkOffset: LiveData<Int> = _bookmarkOffset

    // 狀態訊息
    private val _statusMessage = MutableLiveData<String>()
    val statusMessage: LiveData<String> = _statusMessage

    init {
        _watchSettings.value = prefs.getWatchSettings()
        refreshHistory()

        // 設定通訊回呼
        commManager.onBookmarkUpdated = { offset ->
            _bookmarkOffset.postValue(offset)
        }
        commManager.onWatchRequestReceived = { offset ->
            _statusMessage.postValue("Watch reading at offset: $offset")
        }
    }

    // ================================================================
    // 文件操作
    // ================================================================

    /** 從貼上的文字建立文件 */
    fun loadFromPastedText(text: String, title: String = "Pasted Text") {
        if (text.isBlank()) {
            _statusMessage.value = "Text is empty"
            return
        }

        val docId = UUID.randomUUID().toString()

        // 將貼上的文字儲存到內部檔案，以便日後從歷史重新載入
        val savedUri = savePastedTextToInternal(docId, text)

        val doc = TextDocument(
            id = docId,
            title = title,
            content = text,
            sourcePath = savedUri
        )
        setDocument(doc)

        // 儲存匯入歷史
        val preview = text.take(100).replace("\n", " ")
        val entry = ImportHistoryEntry(
            id = docId,
            title = title,
            bookmarkOffset = 0,
            importedAt = System.currentTimeMillis(),
            sourcePath = savedUri,
            contentPreview = preview
        )
        prefs.saveImportHistoryEntry(entry)
        refreshHistory()
    }

    /**
     * 將貼上的文字存入 app 內部檔案
     * @return 檔案的 URI 字串，供日後 reloadFromHistory 使用
     */
    private fun savePastedTextToInternal(docId: String, text: String): String {
        val context = getApplication<PeleprompterApp>()
        val file = java.io.File(context.filesDir, "pasted_$docId.txt")
        file.writeText(text)
        return Uri.fromFile(file).toString()
    }

    /** 從 .txt 檔案 URI 匯入 */
    fun loadFromFileUri(uri: Uri, fileName: String? = null) {
        try {
            val context = getApplication<PeleprompterApp>()
            val inputStream = context.contentResolver.openInputStream(uri)
                ?: throw Exception("Cannot open file")

            // use{} 會自動關閉 BufferedReader → InputStreamReader → inputStream
            val text = BufferedReader(InputStreamReader(inputStream)).use { it.readText() }

            if (text.isBlank()) {
                _statusMessage.value = "File is empty"
                return
            }

            val displayName = fileName ?: uri.lastPathSegment ?: "Imported File"
            val docId = UUID.randomUUID().toString()
            val doc = TextDocument(
                id = docId,
                title = displayName,
                content = text,
                sourcePath = uri.toString()
            )
            setDocument(doc)

            // 儲存匯入歷史
            val preview = text.take(100).replace("\n", " ")
            val entry = ImportHistoryEntry(
                id = docId,
                title = displayName,
                bookmarkOffset = 0,
                importedAt = System.currentTimeMillis(),
                sourcePath = uri.toString(),
                contentPreview = preview
            )
            prefs.saveImportHistoryEntry(entry)
            refreshHistory()

        } catch (e: Exception) {
            _statusMessage.value = "Failed to import file: ${e.message}"
        }
    }

    /** 從歷史紀錄重新載入文件 */
    fun reloadFromHistory(entry: ImportHistoryEntry) {
        if (entry.sourcePath == null) {
            _statusMessage.value = "This entry has no source file"
            return
        }
        val uri = Uri.parse(entry.sourcePath)
        try {
            val context = getApplication<PeleprompterApp>()

            // 根據 URI scheme 選擇讀取方式
            val inputStream = when (uri.scheme) {
                "file" -> {
                    // 內部儲存的貼上文字 (file:// URI)
                    val file = java.io.File(uri.path!!)
                    if (!file.exists()) throw Exception("File no longer exists")
                    java.io.FileInputStream(file)
                }
                else -> {
                    // SAF content:// URI (外部 .txt 匯入)
                    context.contentResolver.openInputStream(uri)
                        ?: throw Exception("Cannot open file — it may have been moved or deleted")
                }
            }

            // use{} 會自動關閉整個串流鏈
            val text = BufferedReader(InputStreamReader(inputStream)).use { it.readText() }

            val doc = TextDocument(
                id = entry.id,
                title = entry.title,
                content = text,
                bookmarkOffset = prefs.getBookmark(entry.id),
                sourcePath = entry.sourcePath
            )
            setDocument(doc)

        } catch (e: Exception) {
            _statusMessage.value = "Cannot reload file: ${e.message}"
        }
    }

    private fun setDocument(doc: TextDocument) {
        _currentDocument.value = doc
        commManager.setCurrentDocument(doc)
        _statusMessage.value = "Loaded: ${doc.title} (${doc.totalLength} chars)"
    }

    // ================================================================
    // 推送到手錶
    // ================================================================

    /** 推送文件到手錶（從頭開始） */
    fun pushToWatchFromBeginning() {
        val doc = _currentDocument.value ?: run {
            _statusMessage.value = "No text loaded"
            return
        }
        doc.bookmarkOffset = 0
        prefs.saveBookmark(doc.id, 0)
        commManager.setCurrentDocument(doc)
        commManager.launchWatchApp()
        _statusMessage.value = "Pushed to watch: starting from beginning"
    }

    /** 推送文件到手錶（從書籤繼續） */
    fun pushToWatchFromBookmark() {
        val doc = _currentDocument.value ?: run {
            _statusMessage.value = "No text loaded"
            return
        }
        commManager.setCurrentDocument(doc)
        commManager.launchWatchApp()

        val bookmark = doc.bookmarkOffset
        _statusMessage.value = "Pushed to watch: resuming from offset $bookmark"
    }

    // ================================================================
    // 設定操作
    // ================================================================

    /** 更新捲動速度並推送到手錶 */
    fun setScrollSpeed(speed: Int) {
        val settings = _watchSettings.value ?: WatchSettings()
        settings.scrollSpeed = speed.coerceIn(WatchSettings.SPEED_MIN, WatchSettings.SPEED_MAX)
        _watchSettings.value = settings
        commManager.pushSettingsToWatch(settings)
    }

    /** 更新文字大小並推送到手錶 */
    fun setTextSize(size: Int) {
        val settings = _watchSettings.value ?: WatchSettings()
        settings.textSize = size.coerceIn(WatchSettings.SIZE_SMALL, WatchSettings.SIZE_LARGE)
        _watchSettings.value = settings
        commManager.pushSettingsToWatch(settings)
    }

    // ================================================================
    // 歷史管理
    // ================================================================

    fun refreshHistory() {
        _importHistory.value = prefs.getImportHistory()
    }

    fun deleteHistoryEntry(docId: String) {
        prefs.removeHistoryEntry(docId)
        refreshHistory()
    }

    // ================================================================
    // 連線狀態
    // ================================================================

    fun checkWatchConnection() {
        _watchConnected.value = commManager.isWatchConnected()
    }

    // ================================================================
    // 生命週期
    // ================================================================

    fun registerComm() {
        commManager.register()
        checkWatchConnection()
    }

    fun unregisterComm() {
        commManager.unregister()
    }

    override fun onCleared() {
        super.onCleared()
        commManager.unregister()
    }
}
