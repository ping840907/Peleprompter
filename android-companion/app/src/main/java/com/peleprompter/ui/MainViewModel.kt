package com.peleprompter.ui

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.viewModelScope
import com.peleprompter.PeleprompterApp
import com.peleprompter.model.ImportHistoryEntry
import com.peleprompter.model.TextDocument
import com.peleprompter.model.WatchSettings
import com.peleprompter.service.PebbleCommManager
import com.peleprompter.util.EpubTextExtractor
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.UUID

/**
 * MainViewModel - 主畫面的 ViewModel
 *
 * 管理文件載入、歷史紀錄、手錶通訊狀態。
 */
class MainViewModel(application: Application) : AndroidViewModel(application) {

    /** 稿件來源類型 */
    enum class SourceType { NONE, PASTE, FILE }

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

    // 稿件來源類型
    private val _sourceType = MutableLiveData<SourceType>(SourceType.NONE)
    val sourceType: LiveData<SourceType> = _sourceType

    // 狀態訊息
    private val _statusMessage = MutableLiveData<String>()
    val statusMessage: LiveData<String> = _statusMessage

    init {
        // 從舊 3 等級（0/1/2）遷移至新 5 等級（0-4）：舊值整體上移 +2
        val rawSettings = prefs.getWatchSettings()
        if (rawSettings.settingsVersion < WatchSettings.CURRENT_VERSION) {
            if (rawSettings.textSize in 0..2) rawSettings.textSize += 2
            rawSettings.settingsVersion = WatchSettings.CURRENT_VERSION
            prefs.saveWatchSettings(rawSettings)
        }
        _watchSettings.value = rawSettings
        refreshHistory()

        // 啟動時自動還原上次載入的稿件（從內部儲存讀回，不依賴 SAF 權限）
        restoreLastDocument()

        // 設定通訊回呼
        commManager.onBookmarkUpdated = { offset ->
            _bookmarkOffset.postValue(offset)
        }
        commManager.onWatchRequestReceived = { offset ->
            _statusMessage.postValue("Watch reading at offset: $offset")
        }
        commManager.onSettingsUpdated = { settings ->
            _watchSettings.postValue(settings)
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

        viewModelScope.launch {
            // 檔案寫入與歷史儲存移至背景執行緒，避免阻塞 UI（ANR）
            val savedUri = withContext(Dispatchers.IO) { saveTextToInternal(docId, text) }

            val doc = TextDocument(
                id = docId,
                title = title,
                content = text,
                sourcePath = savedUri
            )
            _sourceType.value = SourceType.PASTE
            setDocument(doc)
            saveHistoryEntry(docId, title, savedUri, text, "PASTE")
        }
    }

    /**
     * 將稿件文字（貼上或匯入後的純文字）存入 app 內部檔案。
     * 所有來源都複製到內部儲存，重新載入時不再依賴 SAF content:// 權限，
     * 因此關閉 App 再開啟仍能可靠地從歷史重載。
     * @return 內部檔案的 file:// URI 字串
     */
    private fun saveTextToInternal(docId: String, text: String): String {
        val context = getApplication<PeleprompterApp>()
        val file = java.io.File(context.filesDir, "script_$docId.txt")
        file.writeText(text)
        return Uri.fromFile(file).toString()
    }

    /** 寫入一筆匯入歷史（背景執行緒）並刷新列表 */
    private suspend fun saveHistoryEntry(
        docId: String, title: String, sourcePath: String,
        text: String, sourceType: String
    ) {
        val entry = ImportHistoryEntry(
            id = docId,
            title = title,
            bookmarkOffset = 0,
            importedAt = System.currentTimeMillis(),
            sourcePath = sourcePath,
            contentPreview = text.take(100).replace("\n", " "),
            sourceType = sourceType
        )
        withContext(Dispatchers.IO) { prefs.saveImportHistoryEntry(entry) }
        refreshHistory()
    }

    /** 從 .txt 或 .epub 檔案 URI 匯入 */
    fun loadFromFileUri(uri: Uri, fileName: String? = null) {
        viewModelScope.launch {
            try {
                val context = getApplication<PeleprompterApp>()
                // 讀檔與（EPUB）解析在背景執行緒，避免大檔阻塞 UI（ANR）
                val text = withContext(Dispatchers.IO) {
                    val inputStream = context.contentResolver.openInputStream(uri)
                        ?: throw Exception("Cannot open file")
                    val isEpub = fileName?.endsWith(".epub", ignoreCase = true) == true ||
                                 context.contentResolver.getType(uri) == "application/epub+zip"
                    if (isEpub) {
                        EpubTextExtractor.extract(inputStream).also { inputStream.close() }
                    } else {
                        BufferedReader(InputStreamReader(inputStream)).use { it.readText() }
                    }
                }

                if (text.isBlank()) {
                    _statusMessage.value = "File is empty"
                    return@launch
                }

                val displayName = fileName ?: uri.lastPathSegment ?: "Imported File"
                val docId = UUID.randomUUID().toString()

                // 將解析後的純文字複製到內部儲存，往後重載不依賴外部 content:// 權限
                val savedUri = withContext(Dispatchers.IO) { saveTextToInternal(docId, text) }

                val doc = TextDocument(
                    id = docId,
                    title = displayName,
                    content = text,
                    sourcePath = savedUri
                )
                _sourceType.value = SourceType.FILE
                setDocument(doc)
                saveHistoryEntry(docId, displayName, savedUri, text, "FILE")

            } catch (e: Exception) {
                _statusMessage.value = "Failed to import file: ${e.message}"
            }
        }
    }

    /**
     * 從歷史紀錄重新載入文件。
     * @param silent true 時不顯示錯誤訊息（用於啟動自動還原，避免打擾使用者）
     */
    fun reloadFromHistory(entry: ImportHistoryEntry, silent: Boolean = false) {
        if (entry.sourcePath == null) {
            if (!silent) _statusMessage.value = "This entry has no source file"
            return
        }
        val uri = Uri.parse(entry.sourcePath)
        viewModelScope.launch {
            try {
                val context = getApplication<PeleprompterApp>()

                // 讀檔在背景執行緒，避免阻塞 UI（ANR）
                val text = withContext(Dispatchers.IO) {
                    val inputStream = when (uri.scheme) {
                        "file" -> {
                            // 內部儲存的稿件 (file:// URI)
                            val file = java.io.File(uri.path!!)
                            if (!file.exists()) throw Exception("Script file no longer exists")
                            java.io.FileInputStream(file)
                        }
                        else -> {
                            // 舊版外部 content:// URI（向後相容；新匯入皆存內部）
                            context.contentResolver.openInputStream(uri)
                                ?: throw Exception("Cannot open file — it may have been moved or deleted")
                        }
                    }
                    // use{} 會自動關閉整個串流鏈
                    BufferedReader(InputStreamReader(inputStream)).use { it.readText() }
                }

                val doc = TextDocument(
                    id = entry.id,
                    title = entry.title,
                    content = text,
                    bookmarkOffset = prefs.getBookmark(entry.id),
                    sourcePath = entry.sourcePath
                )
                _sourceType.value =
                    if (entry.sourceType == "PASTE") SourceType.PASTE else SourceType.FILE
                setDocument(doc)

            } catch (e: Exception) {
                // 重載失敗（例如檔案已被清除）：清掉指向它的 last-doc，避免每次啟動都失敗
                if (prefs.getLastDocId() == entry.id) prefs.clearLastDocId()
                if (!silent) _statusMessage.value = "Cannot reload script: ${e.message}"
            }
        }
    }

    /** 啟動時還原上次載入的稿件（若內部檔案仍在） */
    private fun restoreLastDocument() {
        val lastId = prefs.getLastDocId() ?: return
        val entry = prefs.getImportHistory().find { it.id == lastId } ?: return
        reloadFromHistory(entry, silent = true)
    }

    private fun setDocument(doc: TextDocument) {
        _currentDocument.value = doc
        commManager.setCurrentDocument(doc)
        _statusMessage.value = "Loaded: ${doc.title} (${doc.totalLength} chars)"
    }

    /**
     * 清除目前載入的稿件。
     * 同時清除通訊管理員狀態與「上次稿件」記錄，避免手錶仍收到舊內容、
     * 或下次啟動又自動還原已清除的稿件。歷史與內部檔案保留（可從歷史再載入）。
     */
    fun clearCurrentDocument() {
        _currentDocument.value = null
        _sourceType.value = SourceType.NONE
        commManager.clearDocument()
        prefs.clearLastDocId()
        _statusMessage.value = "Script cleared"
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
        settings.textSize = size.coerceIn(WatchSettings.SIZE_TINY, WatchSettings.SIZE_XLARGE)
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
        // 找出該筆以便連同內部稿件檔一起刪除（避免內部儲存累積垃圾）
        val entry = prefs.getImportHistory().find { it.id == docId }
        prefs.removeHistoryEntry(docId)

        entry?.sourcePath?.let { sp ->
            val u = Uri.parse(sp)
            if (u.scheme == "file") {
                u.path?.let { p ->
                    val f = java.io.File(p)
                    val filesDir = getApplication<PeleprompterApp>().filesDir
                    // 僅刪除位於本 app 內部儲存的檔案
                    if (f.exists() && f.parentFile == filesDir) f.delete()
                }
            }
        }

        // 若刪除的是目前/上次稿件，連帶清理狀態
        if (prefs.getLastDocId() == docId) prefs.clearLastDocId()
        if (_currentDocument.value?.id == docId) {
            _currentDocument.value = null
            _sourceType.value = SourceType.NONE
            commManager.clearDocument()
        }
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
