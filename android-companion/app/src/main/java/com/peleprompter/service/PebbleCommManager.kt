package com.peleprompter.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.ContextWrapper
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.getpebble.android.kit.PebbleKit
import com.getpebble.android.kit.util.PebbleDictionary
import com.peleprompter.model.TextDocument
import com.peleprompter.model.WatchSettings
import com.peleprompter.util.PebbleProtocol
import com.peleprompter.util.PrefsManager
import java.util.ArrayDeque

/**
 * PebbleCommManager - Pebble 圖片模式通訊管理員
 *
 * 主要職責：
 * 1. 收到手錶的初始化請求（CMD_REQUEST_TEXT + 螢幕尺寸）
 *    → 用 WatchImageRenderer 計算總頁數
 *    → 回傳 CMD_INIT_IMAGES（總頁數 + 尺寸）
 * 2. 收到手錶的頁面請求（CMD_REQUEST_PAGE）
 *    → 渲染指定頁面（惰性，含快取）
 *    → 分成多個 CMD_SEND_IMAGE_CHUNK 依序傳送
 * 3. 設定同步（CMD_SYNC_SETTINGS）
 *    → 若文字大小改變，清除渲染快取並重新初始化
 */
class PebbleCommManager(
    private val context: Context,
    private val prefsManager: PrefsManager
) {
    companion object {
        private const val TAG = "PebbleCommManager"
    }

    // ── 文件 & 渲染器 ────────────────────────────────────────────
    private var currentDocument: TextDocument? = null
    private var renderer: WatchImageRenderer? = null

    /**
     * 已渲染頁面的快取（pageNum → 1-bit 位元組陣列）
     *
     * 每頁大小：Aplite ≈3KB, Chalk ≈4.1KB, Emery ≈5.7KB。
     * 清除時機：
     *  - setCurrentDocument()：切換文件時清除（pageCache.clear()）
     *  - handleInitRequest()：尺寸改變時清除
     *
     * 為何不需要 LRU 上限：
     * 手錶每次只需 2-3 頁，而 Android 端快取只在手錶主動 request 時才
     * 增長。一部典型提詞稿 200 頁 × 6KB = 1.2MB，在 Android 堆積空間
     * （通常 ≥192MB）中佔比極低，且頁面大小固定、配置方式一致，
     * Android GC 的壓縮式收集器能有效消除小型 ByteArray 的碎片。
     */
    private val pageCache: HashMap<Int, ByteArray> = HashMap()

    // ── PebbleKit 接收器 ─────────────────────────────────────────
    private var dataReceiver: PebbleKit.PebbleDataReceiver? = null
    private var ackReceiver: PebbleKit.PebbleAckReceiver? = null
    private var nackReceiver: PebbleKit.PebbleNackReceiver? = null
    private var isRegistered = false

    // ── 傳送佇列（確保圖片區塊依序到達） ───────────────────────
    private val handler = Handler(Looper.getMainLooper())
    private val messageQueue: ArrayDeque<PebbleDictionary> = ArrayDeque()
    private var isSendingMessage = false
    private var lastSentDict: PebbleDictionary? = null
    private var retryCount = 0
    private val maxRetries = 3

    // ── 狀態回呼 ────────────────────────────────────────────────
    var onBookmarkUpdated: ((Int) -> Unit)? = null
    var onWatchRequestReceived: ((Int) -> Unit)? = null
    var onSettingsUpdated: ((WatchSettings) -> Unit)? = null

    // ================================================================
    // 生命週期
    // ================================================================

    fun register() {
        if (isRegistered) unregister()

        // Android 14+ 相容：強制注入 RECEIVER_EXPORTED 旗標
        val exportedContext = object : ContextWrapper(context) {
            override fun registerReceiver(
                receiver: BroadcastReceiver?, filter: IntentFilter?
            ): android.content.Intent? {
                return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    super.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
                } else {
                    super.registerReceiver(receiver, filter)
                }
            }

            override fun registerReceiver(
                receiver: BroadcastReceiver?, filter: IntentFilter?, flags: Int
            ): android.content.Intent? {
                return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    super.registerReceiver(receiver, filter, flags or Context.RECEIVER_EXPORTED)
                } else {
                    super.registerReceiver(receiver, filter, flags)
                }
            }
        }

        dataReceiver = object : PebbleKit.PebbleDataReceiver(PebbleProtocol.PEBBLE_APP_UUID) {
            override fun receiveData(context: Context, transactionId: Int, data: PebbleDictionary) {
                Log.d(TAG, "收到手錶訊息, txId=$transactionId")
                PebbleKit.sendAckToPebble(context, transactionId)
                handleWatchMessage(data)
            }
        }
        PebbleKit.registerReceivedDataHandler(exportedContext, dataReceiver)

        ackReceiver = object : PebbleKit.PebbleAckReceiver(PebbleProtocol.PEBBLE_APP_UUID) {
            override fun receiveAck(context: Context, transactionId: Int) {
                Log.d(TAG, "ACK: txId=$transactionId")
                retryCount = 0
                lastSentDict = null
                isSendingMessage = false
                processQueue()
            }
        }
        PebbleKit.registerReceivedAckHandler(exportedContext, ackReceiver)

        nackReceiver = object : PebbleKit.PebbleNackReceiver(PebbleProtocol.PEBBLE_APP_UUID) {
            override fun receiveNack(context: Context, transactionId: Int) {
                Log.w(TAG, "NACK: txId=$transactionId, retry=$retryCount")
                if (retryCount < maxRetries && lastSentDict != null) {
                    retryCount++
                    handler.postDelayed({
                        lastSentDict?.let {
                            sendToPebble(it, isRetry = true)
                        }
                    }, 500L * retryCount)
                } else {
                    // 重試耗盡，放棄此訊息繼續佇列
                    retryCount = 0
                    lastSentDict = null
                    isSendingMessage = false
                    processQueue()
                }
            }
        }
        PebbleKit.registerReceivedNackHandler(exportedContext, nackReceiver)

        isRegistered = true
        Log.i(TAG, "PebbleKit 接收器已註冊")
    }

    fun unregister() {
        if (!isRegistered) return
        try {
            dataReceiver?.let { context.unregisterReceiver(it) }
            ackReceiver?.let  { context.unregisterReceiver(it) }
            nackReceiver?.let { context.unregisterReceiver(it) }
        } catch (e: Exception) {
            Log.w(TAG, "取消註冊時發生例外: ${e.message}")
        }
        dataReceiver    = null
        ackReceiver     = null
        nackReceiver    = null
        handler.removeCallbacksAndMessages(null)
        messageQueue.clear()
        isSendingMessage = false
        isRegistered     = false
        Log.i(TAG, "PebbleKit 接收器已取消註冊")
    }

    // ================================================================
    // 文件管理
    // ================================================================

    fun setCurrentDocument(doc: TextDocument) {
        currentDocument = doc
        // 文件更換時清除渲染快取
        pageCache.clear()
        renderer?.invalidateCache()
        prefsManager.saveLastDocId(doc.id)
        Log.i(TAG, "設定目前文件: ${doc.title}, 長度=${doc.totalLength}")
    }

    fun getCurrentDocument(): TextDocument? = currentDocument

    // ================================================================
    // 處理手錶訊息
    // ================================================================

    private fun handleWatchMessage(data: PebbleDictionary) {
        val command = data.getInteger(PebbleProtocol.KEY_COMMAND)?.toInt() ?: return

        when (command) {
            PebbleProtocol.CMD_REQUEST_TEXT -> {
                // 手錶初始化請求，附帶螢幕尺寸與偏好文字大小
                val watchW    = data.getInteger(PebbleProtocol.KEY_WATCH_WIDTH)?.toInt()  ?: 144
                val watchH    = data.getInteger(PebbleProtocol.KEY_WATCH_HEIGHT)?.toInt() ?: 168
                val textSize  = data.getInteger(PebbleProtocol.KEY_TEXT_SIZE)?.toInt()
                    ?: prefsManager.getWatchSettings().textSize

                Log.i(TAG, "收到初始化請求: ${watchW}x${watchH}, textSize=$textSize")
                onWatchRequestReceived?.invoke(0)
                handleInitRequest(watchW, watchH, textSize)
            }

            PebbleProtocol.CMD_REQUEST_PAGE -> {
                val pageNum = data.getInteger(PebbleProtocol.KEY_PAGE_NUM)?.toInt() ?: 0
                Log.d(TAG, "收到頁面請求: page=$pageNum")
                handlePageRequest(pageNum)
            }

            PebbleProtocol.CMD_SYNC_SETTINGS -> {
                val speed = data.getInteger(PebbleProtocol.KEY_SCROLL_SPEED)?.toInt()
                val size  = data.getInteger(PebbleProtocol.KEY_TEXT_SIZE)?.toInt()
                if (speed != null || size != null) {
                    val settings = prefsManager.getWatchSettings()
                    speed?.let { settings.scrollSpeed = it }

                    val sizeChanged = size != null && size != settings.textSize
                    size?.let { settings.textSize = it }

                    prefsManager.saveWatchSettings(settings)
                    onSettingsUpdated?.invoke(settings)

                    // 文字大小改變→清除快取，手錶端會另外發送 CMD_REQUEST_TEXT 觸發重新初始化
                    if (sizeChanged) {
                        Log.i(TAG, "文字大小已改變，清除渲染快取")
                        pageCache.clear()
                        renderer?.invalidateCache()
                    }
                }
            }

            else -> Log.w(TAG, "未知指令: $command")
        }
    }

    // ================================================================
    // 初始化：計算總頁數並通知手錶
    // ================================================================

    private fun handleInitRequest(watchW: Int, watchH: Int, textSizeLevel: Int) {
        val doc = currentDocument ?: run {
            Log.w(TAG, "無文件可渲染，忽略初始化請求")
            return
        }

        // 若渲染器尺寸或文字大小改變，重建渲染器並清除快取
        val existingRenderer = renderer
        if (existingRenderer == null
            || existingRenderer.watchWidth  != watchW
            || existingRenderer.watchHeight != watchH) {
            Log.i(TAG, "建立新渲染器: ${watchW}x${watchH}, size=$textSizeLevel")
            renderer = WatchImageRenderer(watchW, watchH, textSizeLevel)
            pageCache.clear()
        }

        val r = renderer!!
        val totalPages = r.computeTotalPages(doc)

        Log.i(TAG, "回傳 INIT_IMAGES: pages=$totalPages, ${watchW}x${watchH}")

        val dict = PebbleDictionary().apply {
            addInt32(PebbleProtocol.KEY_COMMAND,      PebbleProtocol.CMD_INIT_IMAGES)
            addInt32(PebbleProtocol.KEY_TOTAL_PAGES,  totalPages)
            addInt32(PebbleProtocol.KEY_WATCH_WIDTH,  watchW)
            addInt32(PebbleProtocol.KEY_WATCH_HEIGHT, watchH)
        }
        enqueue(dict)
    }

    // ================================================================
    // 頁面請求：渲染並分塊傳送
    // ================================================================

    private fun handlePageRequest(pageNum: Int) {
        val r   = renderer ?: run { Log.w(TAG, "渲染器尚未初始化"); return }
        val doc = currentDocument ?: run { Log.w(TAG, "無文件"); return }

        if (pageNum < 0 || pageNum >= r.computeTotalPages(doc)) {
            Log.w(TAG, "頁碼超出範圍: $pageNum")
            return
        }

        // 更新書籤（以頁碼估算位元組偏移）
        val estimatedOffset = (pageNum.toLong() * doc.totalLength / r.computeTotalPages(doc)).toInt()
        if (estimatedOffset > doc.bookmarkOffset) {
            doc.bookmarkOffset = estimatedOffset
            prefsManager.saveBookmark(doc.id, estimatedOffset)
            onBookmarkUpdated?.invoke(estimatedOffset)
        }

        // 從快取或重新渲染
        val pageData = pageCache.getOrPut(pageNum) {
            Log.d(TAG, "渲染頁面 $pageNum")
            r.renderPage(doc, pageNum) ?: run {
                Log.e(TAG, "頁面 $pageNum 渲染失敗")
                return
            }
        }

        sendPageInChunks(pageNum, pageData)
    }

    /**
     * 將頁面資料拆分成多個區塊並加入傳送佇列。
     * 每個區塊 ≤ IMAGE_CHUNK_DATA_SIZE bytes。
     */
    private fun sendPageInChunks(pageNum: Int, data: ByteArray) {
        val chunkSize   = PebbleProtocol.IMAGE_CHUNK_DATA_SIZE
        val totalChunks = (data.size + chunkSize - 1) / chunkSize

        Log.d(TAG, "分塊傳送頁面 $pageNum: ${data.size} bytes → $totalChunks 個區塊")

        for (chunkIdx in 0 until totalChunks) {
            val start     = chunkIdx * chunkSize
            val end       = minOf(start + chunkSize, data.size)
            val chunkData = data.copyOfRange(start, end)

            val dict = PebbleDictionary().apply {
                addInt32(PebbleProtocol.KEY_COMMAND,       PebbleProtocol.CMD_SEND_IMAGE_CHUNK)
                addInt32(PebbleProtocol.KEY_PAGE_NUM,      pageNum)
                addInt32(PebbleProtocol.KEY_CHUNK_INDEX,   chunkIdx)
                addInt32(PebbleProtocol.KEY_TOTAL_CHUNKS,  totalChunks)
                addBytes(PebbleProtocol.KEY_IMAGE_DATA,    chunkData)
            }
            enqueue(dict)
        }
    }

    // ================================================================
    // 傳送佇列管理（ACK-driven，保證順序送達）
    // ================================================================

    private fun enqueue(dict: PebbleDictionary) {
        messageQueue.addLast(dict)
        if (!isSendingMessage) {
            processQueue()
        }
    }

    private fun processQueue() {
        val next = messageQueue.pollFirst() ?: return
        isSendingMessage = true
        sendToPebble(next)
    }

    private fun sendToPebble(dict: PebbleDictionary, isRetry: Boolean = false) {
        lastSentDict = dict
        if (!isRetry) retryCount = 0
        PebbleKit.sendDataToPebble(context, PebbleProtocol.PEBBLE_APP_UUID, dict)
    }

    // ================================================================
    // 推送設定到手錶
    // ================================================================

    fun pushSettingsToWatch(settings: WatchSettings) {
        val dict = PebbleDictionary().apply {
            addInt32(PebbleProtocol.KEY_COMMAND,      PebbleProtocol.CMD_SYNC_SETTINGS)
            addInt32(PebbleProtocol.KEY_SCROLL_SPEED, settings.scrollSpeed)
            addInt32(PebbleProtocol.KEY_TEXT_SIZE,    settings.textSize)
        }
        prefsManager.saveWatchSettings(settings)
        Log.d(TAG, "推送設定: speed=${settings.scrollSpeed}, size=${settings.textSize}")
        enqueue(dict)
    }

    // ================================================================
    // 啟動手錶 App / 連線查詢
    // ================================================================

    fun launchWatchApp() {
        PebbleKit.startAppOnPebble(context, PebbleProtocol.PEBBLE_APP_UUID)
        Log.i(TAG, "已發送啟動手錶 App 指令")
    }

    fun isWatchConnected(): Boolean = PebbleKit.isWatchConnected(context)
}
