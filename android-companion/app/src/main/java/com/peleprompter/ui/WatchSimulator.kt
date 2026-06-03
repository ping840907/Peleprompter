package com.peleprompter.ui

import android.graphics.Bitmap
import android.graphics.Color
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import com.peleprompter.model.TextDocument
import com.peleprompter.service.WatchImageRenderer
import com.peleprompter.util.PebbleProtocol
import java.util.concurrent.ConcurrentHashMap

// ─────────────────────────────────────────────────────────────────
// Platform definitions
// ─────────────────────────────────────────────────────────────────

enum class WatchPlatform(
    val displayName: String,
    val screenWidth: Int,
    val screenHeight: Int,
    val isCircular: Boolean
) {
    APLITE("Aplite  144×168", 144, 168, false),
    BASALT("Basalt  144×168", 144, 168, false),
    CHALK( "Chalk   180×180", 180, 180, true),
    DIORITE("Diorite 144×168", 144, 168, false),
    EMERY( "Emery   200×228", 200, 228, false)
}

// ─────────────────────────────────────────────────────────────────
// Log types & entries
// ─────────────────────────────────────────────────────────────────

enum class LogType { TX, RX, INFO, WARN, ERROR }

data class LogEntry(
    val timestamp: Long = System.currentTimeMillis(),
    val type: LogType,
    val tag: String,
    val message: String
)

// ─────────────────────────────────────────────────────────────────
// WatchSimulator
// ─────────────────────────────────────────────────────────────────

/**
 * WatchSimulator
 *
 * 模擬 Pebble 手錶端的狀態機，直接驅動 WatchImageRenderer 取得渲染資料，
 * 並記錄每一個等效的協定訊息以供除錯。
 *
 * 呼叫方：
 *  - initialize() → 模擬 CMD_REQUEST_TEXT + 回傳 CMD_INIT_IMAGES
 *  - pressUp/Down/Select() → 模擬按鍵事件
 *  - startAutoScroll() / stopAutoScroll() → 自動捲動
 *  - cleanup() → 停止所有後台執行緒
 */
class WatchSimulator(
    private val document: TextDocument,
    var platform: WatchPlatform,
    var textSizeLevel: Int,
    private val onLog: (LogEntry) -> Unit,
    /** topBmp, bottomBmp, yInTop (px into top page), pageHeight (px) */
    private val onDisplayUpdate: (Bitmap?, Bitmap?, Int, Int) -> Unit
) {
    companion object {
        private const val TAG = "WatchSimulator"
        /** 距下一頁邊界低於此像素時預取，同 constants.h IMAGE_PREFETCH_THRESHOLD_PX */
        private const val PREFETCH_THRESHOLD_PX = 336
        /** 最大日誌保留數量 */
        private const val MAX_LOG_ENTRIES = 500
    }

    // ── 渲染 ────────────────────────────────────────────────────
    private var renderer: WatchImageRenderer? = null

    /**
     * 已解碼頁面的 Bitmap 快取。
     * 手錶端 C code 只快取 2–3 頁；這裡不設上限，因 Android 記憶體充裕且測試目的不同。
     * 使用 ConcurrentHashMap：render 執行緒寫入、主執行緒讀取，需跨執行緒安全存取。
     */
    private val pageCache = ConcurrentHashMap<Int, Bitmap>()

    /** cleanup 後設為 true，阻止仍在佇列中的 render 任務回呼操作已釋放的資源 */
    @Volatile private var isCleanedUp = false

    // ── 手錶狀態 ────────────────────────────────────────────────
    var scrollPx: Int = 0
        private set
    var totalPages: Int = 0
        private set
    var isInitialized: Boolean = false
        private set

    // ── 自動捲動 ────────────────────────────────────────────────
    private var isAutoScrolling = false
    var scrollSpeed: Int = 3  // 1–6

    /** 捲動間隔（ms），索引 0 不用，與 C 端 SCROLL_INTERVALS_MS 對齊 */
    private val scrollIntervals = intArrayOf(0, 120, 90, 65, 45, 30, 18)

    // ── 執行緒 ─────────────────────────────────────────────────
    private val renderThread = HandlerThread("WatchSimRender").also { it.start() }
    private val renderHandler = Handler(renderThread.looper)
    private val mainHandler   = Handler(Looper.getMainLooper())

    // ── 自動捲動 Runnable ───────────────────────────────────────
    private val autoScrollRunnable = object : Runnable {
        override fun run() {
            if (!isAutoScrolling) return
            stepScroll(1)
            val interval = scrollIntervals.getOrElse(scrollSpeed) { 60 }.toLong()
            mainHandler.postDelayed(this, interval)
        }
    }

    // ================================================================
    // 生命週期
    // ================================================================

    /** 首次初始化：建立渲染器、計算總頁數並預取前幾頁 */
    fun initialize() {
        log(LogType.TX, "WATCH→PHONE",
            "CMD_REQUEST_TEXT | width=${platform.screenWidth} height=${platform.screenHeight} textSize=$textSizeLevel")

        renderHandler.post {
            val r = WatchImageRenderer(platform.screenWidth, platform.screenHeight, textSizeLevel)
            renderer   = r
            totalPages = r.computeTotalPages(document)
            scrollPx   = 0
            isInitialized = true

            mainHandler.post {
                if (isCleanedUp) return@post
                log(LogType.RX, "PHONE→WATCH",
                    "CMD_INIT_IMAGES | totalPages=$totalPages width=${platform.screenWidth} height=${platform.screenHeight}")
                log(LogType.INFO, "SIM",
                    "初始化完成：$totalPages 頁，${platform.screenWidth}×${platform.screenHeight}")
                prefetchFromPage(0)
            }
        }
    }

    /** 重新初始化（平台或字型大小改變時呼叫） */
    fun reinitialize() {
        stopAutoScroll()
        renderHandler.post {
            pageCache.values.forEach { it.recycle() }
            pageCache.clear()
            renderer?.invalidateCache()
            renderer = null
            isInitialized = false
            scrollPx = 0
            mainHandler.post {
                if (isCleanedUp) return@post
                log(LogType.INFO, "SIM",
                    "重新初始化：platform=${platform.displayName} textSize=$textSizeLevel")
                initialize()
            }
        }
    }

    /** 釋放所有資源，必須在 Activity/Fragment 銷毀時呼叫 */
    fun cleanup() {
        isCleanedUp = true   // 阻止仍在執行中的 render 任務之後回呼主執行緒
        stopAutoScroll()
        renderHandler.removeCallbacksAndMessages(null)
        renderThread.quitSafely()
        mainHandler.removeCallbacksAndMessages(null)
        pageCache.values.forEach { it.recycle() }
        pageCache.clear()
    }

    // ================================================================
    // 按鍵事件
    // ================================================================

    fun pressUp() {
        if (!isInitialized) return
        log(LogType.INFO, "BTN", "UP → 往上捲 60px")
        stepScroll(-60)
    }

    fun pressDown() {
        if (!isInitialized) return
        log(LogType.INFO, "BTN", "DOWN → 往下捲 60px")
        stepScroll(60)
    }

    fun pressSelect() {
        if (!isInitialized) return
        if (isAutoScrolling) {
            stopAutoScroll()
            log(LogType.INFO, "BTN", "SELECT → 停止自動捲動")
        } else {
            startAutoScroll()
            log(LogType.INFO, "BTN", "SELECT → 啟動自動捲動")
        }
    }

    // ================================================================
    // 自動捲動
    // ================================================================

    fun startAutoScroll() {
        if (isAutoScrolling) return
        isAutoScrolling = true
        val interval = scrollIntervals.getOrElse(scrollSpeed) { 60 }.toLong()
        mainHandler.postDelayed(autoScrollRunnable, interval)
        log(LogType.INFO, "AUTO", "自動捲動啟動，speed=$scrollSpeed，interval=${interval}ms")
    }

    fun stopAutoScroll() {
        if (!isAutoScrolling) return
        isAutoScrolling = false
        mainHandler.removeCallbacks(autoScrollRunnable)
        log(LogType.INFO, "AUTO", "自動捲動停止")
    }

    val isAutoScrollActive: Boolean get() = isAutoScrolling

    // ================================================================
    // 捲動
    // ================================================================

    private fun stepScroll(deltaPx: Int) {
        val ph = platform.screenHeight
        val maxScroll = ((totalPages - 1) * ph).coerceAtLeast(0)
        scrollPx = (scrollPx + deltaPx).coerceIn(0, maxScroll)
        checkPrefetch()
        updateDisplay()
    }

    // ================================================================
    // 頁面預取與快取
    // ================================================================

    private fun checkPrefetch() {
        val ph      = platform.screenHeight
        val topPage = scrollPx / ph
        val yInTop  = scrollPx % ph

        // 目前頁
        if (!pageCache.containsKey(topPage)) requestPage(topPage)

        // 下一頁預取
        val pixelsToNextPage = ph - yInTop
        if (pixelsToNextPage < PREFETCH_THRESHOLD_PX) {
            val next = topPage + 1
            if (next < totalPages && !pageCache.containsKey(next)) requestPage(next)
        }
    }

    private fun prefetchFromPage(startPage: Int) {
        val count = minOf(3, totalPages - startPage)
        for (i in 0 until count) requestPage(startPage + i)
    }

    private fun requestPage(pageNum: Int) {
        if (pageCache.containsKey(pageNum)) return
        val r = renderer ?: return

        log(LogType.TX, "WATCH→PHONE", "CMD_REQUEST_PAGE | pageNum=$pageNum")

        renderHandler.post {
            if (isCleanedUp) return@post
            val data = r.renderPage(document, pageNum)
            if (data == null) {
                mainHandler.post {
                    if (isCleanedUp) return@post
                    log(LogType.ERROR, "SIM", "頁面 $pageNum 渲染失敗")
                }
                return@post
            }

            val chunks = (data.size + PebbleProtocol.IMAGE_CHUNK_DATA_SIZE - 1) /
                         PebbleProtocol.IMAGE_CHUNK_DATA_SIZE
            val bmp = decode1BitToBitmap(data, platform.screenWidth, platform.screenHeight)

            mainHandler.post {
                if (isCleanedUp) { bmp.recycle(); return@post }
                log(LogType.RX, "PHONE→WATCH",
                    "CMD_SEND_IMAGE_CHUNK × $chunks | page=$pageNum size=${data.size}B")
                log(LogType.INFO, "SIM",
                    "頁面 $pageNum 就緒（${data.size}B → ${platform.screenWidth}×${platform.screenHeight}）")
                pageCache[pageNum] = bmp
                updateDisplay()
            }
        }
    }

    // ================================================================
    // 畫面更新
    // ================================================================

    private fun updateDisplay() {
        val ph      = platform.screenHeight
        val topPage = scrollPx / ph
        val yInTop  = scrollPx % ph

        val topBmp    = pageCache[topPage]
        val bottomBmp = if (yInTop > 0) pageCache[topPage + 1] else null

        onDisplayUpdate(topBmp, bottomBmp, yInTop, ph)
    }

    // ================================================================
    // 1-bit 解碼
    // ================================================================

    /**
     * 將 WatchImageRenderer 輸出的 GBitmapFormat1Bit 位元組陣列解碼為 ARGB_8888 Bitmap。
     * 使用 setPixels 批次寫入，避免逐像素 setPixel 的 JNI 呼叫開銷。
     */
    private fun decode1BitToBitmap(data: ByteArray, width: Int, height: Int): Bitmap {
        val pixels = IntArray(width * height)
        val stride = (width + 7) / 8
        for (y in 0 until height) {
            for (x in 0 until width) {
                val byteIdx = y * stride + x / 8
                val bitIdx  = 7 - (x % 8)
                val isWhite = byteIdx < data.size && ((data[byteIdx].toInt() ushr bitIdx) and 1 == 1)
                pixels[y * width + x] = if (isWhite) Color.WHITE else Color.BLACK
            }
        }
        val bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        bmp.setPixels(pixels, 0, width, 0, 0, width, height)
        return bmp
    }

    // ================================================================
    // 模擬連線事件
    // ================================================================

    fun simulateDisconnect() {
        stopAutoScroll()
        log(LogType.WARN, "CONN", "手錶中斷連線")
    }

    fun simulateReconnect() {
        log(LogType.INFO, "CONN", "手錶重新連線 → 重新初始化")
        reinitialize()
    }

    // ================================================================
    // 日誌
    // ================================================================

    private fun log(type: LogType, tag: String, message: String) {
        Log.d(TAG, "[$tag] $message")
        onLog(LogEntry(type = type, tag = tag, message = message))
    }
}
