package com.peleprompter.service

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.getpebble.android.kit.PebbleKit
import com.getpebble.android.kit.util.PebbleDictionary
import com.peleprompter.model.TextDocument
import com.peleprompter.model.WatchSettings
import com.peleprompter.util.PebbleProtocol
import com.peleprompter.util.PrefsManager

/**
 * PebbleCommManager - Pebble 通訊管理器
 *
 * 負責：
 * - 處理手錶的文字區塊請求
 * - 將文字分塊傳送到手錶
 * - 推送設定同步
 * - 追蹤閱讀進度（書籤）
 */
class PebbleCommManager(
    private val context: Context,
    private val prefsManager: PrefsManager
) {

    companion object {
        private const val TAG = "PebbleCommManager"
    }

    // 目前推送到手錶的文件
    private var currentDocument: TextDocument? = null

    // 通訊接收器
    private var dataReceiver: PebbleKit.PebbleDataReceiver? = null
    private var ackReceiver: PebbleKit.PebbleAckReceiver? = null
    private var nackReceiver: PebbleKit.PebbleNackReceiver? = null

    // 重試機制
    private val handler = Handler(Looper.getMainLooper())
    private var lastSentDict: PebbleDictionary? = null
    private var retryCount = 0
    private val maxRetries = 3

    // 狀態監聽
    var onBookmarkUpdated: ((Int) -> Unit)? = null
    var onWatchRequestReceived: ((Int) -> Unit)? = null

    // 防止重複註冊
    private var isRegistered = false

    // ================================================================
    // 生命週期
    // ================================================================

    /** 註冊所有 PebbleKit 接收器 */
    fun register() {
        // 防止重複註冊：先清理既有的接收器
        if (isRegistered) {
            unregister()
        }

        // 資料接收器 - 處理手錶的請求
        dataReceiver = object : PebbleKit.PebbleDataReceiver(PebbleProtocol.PEBBLE_APP_UUID) {
            override fun receiveData(context: Context, transactionId: Int, data: PebbleDictionary) {
                Log.d(TAG, "收到手錶訊息, transactionId=$transactionId")
                PebbleKit.sendAckToPebble(context, transactionId)
                handleWatchMessage(data)
            }
        }
        PebbleKit.registerReceivedDataHandler(context, dataReceiver)

        // ACK 接收器
        ackReceiver = object : PebbleKit.PebbleAckReceiver(PebbleProtocol.PEBBLE_APP_UUID) {
            override fun receiveAck(context: Context, transactionId: Int) {
                Log.d(TAG, "收到 ACK: transactionId=$transactionId")
                retryCount = 0
                lastSentDict = null
            }
        }
        PebbleKit.registerReceivedAckHandler(context, ackReceiver)

        // NACK 接收器
        nackReceiver = object : PebbleKit.PebbleNackReceiver(PebbleProtocol.PEBBLE_APP_UUID) {
            override fun receiveNack(context: Context, transactionId: Int) {
                Log.w(TAG, "收到 NACK: transactionId=$transactionId, 重試次數=$retryCount")
                if (retryCount < maxRetries && lastSentDict != null) {
                    retryCount++
                    handler.postDelayed({
                        lastSentDict?.let { sendToPebble(it) }
                    }, 500L * retryCount) // 遞增延遲重試
                }
            }
        }
        PebbleKit.registerReceivedNackHandler(context, nackReceiver)

        isRegistered = true
        Log.i(TAG, "PebbleKit 接收器已註冊")
    }

    /** 取消註冊所有接收器 */
    fun unregister() {
        if (!isRegistered) return

        try {
            dataReceiver?.let { context.unregisterReceiver(it) }
            ackReceiver?.let { context.unregisterReceiver(it) }
            nackReceiver?.let { context.unregisterReceiver(it) }
        } catch (e: Exception) {
            Log.w(TAG, "取消註冊時發生例外: ${e.message}")
        }
        dataReceiver = null
        ackReceiver = null
        nackReceiver = null
        handler.removeCallbacksAndMessages(null)
        isRegistered = false
        Log.i(TAG, "PebbleKit 接收器已取消註冊")
    }

    // ================================================================
    // 文件管理
    // ================================================================

    /** 設定目前要推送的文件 */
    fun setCurrentDocument(doc: TextDocument) {
        currentDocument = doc
        prefsManager.saveLastDocId(doc.id)
        Log.i(TAG, "設定目前文件: ${doc.title}, 長度=${doc.totalLength}")
    }

    /** 取得目前文件 */
    fun getCurrentDocument(): TextDocument? = currentDocument

    // ================================================================
    // 處理手錶訊息
    // ================================================================

    private fun handleWatchMessage(data: PebbleDictionary) {
        val command = data.getInteger(PebbleProtocol.KEY_COMMAND)?.toInt() ?: return

        when (command) {
            PebbleProtocol.CMD_REQUEST_TEXT -> {
                // 手錶請求文字區塊
                val offset = data.getInteger(PebbleProtocol.KEY_TEXT_OFFSET)?.toInt() ?: 0
                Log.d(TAG, "手錶請求文字, offset=$offset")
                onWatchRequestReceived?.invoke(offset)
                sendTextChunk(offset)
            }

            PebbleProtocol.CMD_SYNC_SETTINGS -> {
                // 手錶回報設定變更
                val speed = data.getInteger(PebbleProtocol.KEY_SCROLL_SPEED)?.toInt()
                val size = data.getInteger(PebbleProtocol.KEY_TEXT_SIZE)?.toInt()
                if (speed != null || size != null) {
                    val settings = prefsManager.getWatchSettings()
                    speed?.let { settings.scrollSpeed = it }
                    size?.let { settings.textSize = it }
                    prefsManager.saveWatchSettings(settings)
                    Log.d(TAG, "手錶設定已更新: speed=$speed, size=$size")
                }
            }

            else -> Log.w(TAG, "未知指令: $command")
        }
    }

    // ================================================================
    // 傳送文字區塊
    // ================================================================

    /** 傳送指定偏移量的文字區塊到手錶 */
    fun sendTextChunk(offset: Int) {
        val doc = currentDocument ?: run {
            Log.w(TAG, "無文件可傳送，忽略手錶的請求")
            // 不傳送空區塊，避免手錶誤判為「到達結尾」
            return
        }

        // 防禦：修正負數偏移量
        val safeOffset = offset.coerceAtLeast(0)

        val chunk = doc.getChunk(safeOffset, PebbleProtocol.DEFAULT_CHUNK_SIZE)

        // 只在有實際文字且偏移量前進時更新書籤
        if (chunk.isNotEmpty() && safeOffset > doc.bookmarkOffset) {
            doc.bookmarkOffset = safeOffset
            prefsManager.saveBookmark(doc.id, safeOffset)
            onBookmarkUpdated?.invoke(safeOffset)
        }

        val dict = PebbleDictionary().apply {
            addInt32(PebbleProtocol.KEY_COMMAND, PebbleProtocol.CMD_SEND_TEXT)
            addInt32(PebbleProtocol.KEY_TEXT_OFFSET, safeOffset)
            addString(PebbleProtocol.KEY_TEXT_CHUNK, chunk)
        }

        if (chunk.isEmpty()) {
            Log.d(TAG, "傳送空區塊 (到達文字結尾): offset=$safeOffset")
        } else {
            Log.d(TAG, "傳送文字區塊: offset=$safeOffset, length=${chunk.length}")
        }
        sendToPebble(dict)
    }

    /** 傳送空區塊（表示到達文字結尾） */
    private fun sendEmptyChunk(offset: Int) {
        val dict = PebbleDictionary().apply {
            addInt32(PebbleProtocol.KEY_COMMAND, PebbleProtocol.CMD_SEND_TEXT)
            addInt32(PebbleProtocol.KEY_TEXT_OFFSET, offset)
            addString(PebbleProtocol.KEY_TEXT_CHUNK, "")
        }
        sendToPebble(dict)
    }

    // ================================================================
    // 推送設定到手錶
    // ================================================================

    /** 推送設定同步到手錶 */
    fun pushSettingsToWatch(settings: WatchSettings) {
        val dict = PebbleDictionary().apply {
            addInt32(PebbleProtocol.KEY_COMMAND, PebbleProtocol.CMD_SYNC_SETTINGS)
            addInt32(PebbleProtocol.KEY_SCROLL_SPEED, settings.scrollSpeed)
            addInt32(PebbleProtocol.KEY_TEXT_SIZE, settings.textSize)
        }

        prefsManager.saveWatchSettings(settings)
        Log.d(TAG, "推送設定: speed=${settings.scrollSpeed}, size=${settings.textSize}")
        sendToPebble(dict)
    }

    // ================================================================
    // 啟動手錶 App
    // ================================================================

    /** 啟動手錶上的 Peleprompter App */
    fun launchWatchApp() {
        PebbleKit.startAppOnPebble(context, PebbleProtocol.PEBBLE_APP_UUID)
        Log.i(TAG, "已發送啟動手錶 App 指令")
    }

    /** 檢查手錶是否已連線 */
    fun isWatchConnected(): Boolean {
        return PebbleKit.isWatchConnected(context)
    }

    // ================================================================
    // 底層傳送
    // ================================================================

    private fun sendToPebble(dict: PebbleDictionary) {
        lastSentDict = dict
        retryCount = 0
        PebbleKit.sendDataToPebble(context, PebbleProtocol.PEBBLE_APP_UUID, dict)
    }
}
