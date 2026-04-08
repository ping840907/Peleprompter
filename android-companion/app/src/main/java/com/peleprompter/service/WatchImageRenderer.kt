package com.peleprompter.service

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.os.Build
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.util.Log
import com.peleprompter.model.TextDocument
import com.peleprompter.util.PebbleProtocol
import kotlin.math.ceil

/**
 * WatchImageRenderer
 *
 * 負責將文字文件渲染成 Pebble 相容的 1-bit 點陣圖頁面。
 *
 * 設計原則：
 * - 每頁尺寸 = 手錶螢幕解析度 (watchWidth × watchHeight pixels)
 * - 第 0 頁頂部、最後頁底部各保留 50px 黑色留白（對應 EXTRA_SCROLL_PADDING）
 * - 輸出格式：GBitmapFormat1Bit 相容
 *   · 每行以位元組對齊 (row_stride = ceil(width / 8))
 *   · MSB = 最左側像素，1 = 白色，0 = 黑色
 * - 全部頁面從同一個 StaticLayout 計算，確保分頁一致性
 *
 * @param watchWidth  手錶螢幕寬度 (pixels)
 * @param watchHeight 手錶螢幕高度 (pixels)
 * @param textSizeLevel 文字大小 (PebbleProtocol.TEXT_SIZE_*)
 */
class WatchImageRenderer(
    val watchWidth: Int,
    val watchHeight: Int,
    private val textSizeLevel: Int
) {
    companion object {
        private const val TAG = "WatchImageRenderer"

        /** 頭尾留白（像素），與手錶端 EXTRA_SCROLL_PADDING 一致 */
        const val EXTRA_PADDING_PX = 50

        /**
         * 文字大小對照（像素）
         * 對應手錶端 Gothic 18/24/28 的視覺大小，
         * 使用 Android 無抗鋸齒 sans-serif 在 1-bit 環境下呈現清晰效果。
         */
        private fun fontSizePx(sizeLevel: Int): Float = when (sizeLevel) {
            PebbleProtocol.TEXT_SIZE_SMALL  -> 16f
            PebbleProtocol.TEXT_SIZE_LARGE  -> 28f
            else                             -> 22f   // MEDIUM
        }

        /** 行距倍數：略寬鬆，提升提詞機閱讀舒適度 */
        private const val LINE_SPACING_MULT = 1.15f
    }

    // ── 內部快取 ────────────────────────────────────────────────
    private var cachedLayout: StaticLayout? = null
    private var cachedTotalPages: Int = -1
    private var cachedContent: String? = null

    // 每行的 stride (位元組數)
    val rowStride: Int = (watchWidth + 7) / 8

    // ============================================================
    // 公開 API
    // ============================================================

    /**
     * 計算文件的總頁數（快速，僅做排版量測，不渲染圖片）
     */
    fun computeTotalPages(doc: TextDocument): Int {
        val layout = getOrBuildLayout(doc.content)
        val totalTextH = layout.height
        // 虛擬捲軸總高度 = 上留白 + 文字 + 下留白
        val totalVirtualH = EXTRA_PADDING_PX + totalTextH + EXTRA_PADDING_PX
        val pages = (totalVirtualH + watchHeight - 1) / watchHeight
        cachedTotalPages = pages.coerceAtLeast(1)
        Log.d(TAG, "總頁數=$cachedTotalPages, textH=$totalTextH, virtualH=$totalVirtualH")
        return cachedTotalPages
    }

    /**
     * 渲染指定頁面並回傳 Pebble 1-bit 格式的位元組陣列。
     *
     * 演算法：
     *  1. 計算此頁在「虛擬卷軸」中的 y 範圍 [virtual_y_start, virtual_y_end)
     *  2. 換算成文字座標：text_y = virtual_y - EXTRA_PADDING_PX
     *  3. 將 Canvas 垂直平移，讓文字正確出現在當頁位置
     *  4. 轉換成 1-bit 格式
     *
     * @param doc     文字文件
     * @param pageNum 頁碼 (0-indexed)
     * @return 1-bit 位元組陣列，長度 = rowStride × watchHeight；失敗回傳 null
     */
    fun renderPage(doc: TextDocument, pageNum: Int): ByteArray? {
        val layout = getOrBuildLayout(doc.content)

        // 建立頁面 Bitmap（ARGB_8888 以利 Canvas 操作，之後轉換成 1-bit）
        val bitmap = try {
            Bitmap.createBitmap(watchWidth, watchHeight, Bitmap.Config.ARGB_8888)
        } catch (e: OutOfMemoryError) {
            Log.e(TAG, "頁 $pageNum 無法分配 Bitmap: ${e.message}")
            return null
        }

        val canvas = Canvas(bitmap)

        // 黑色背景
        canvas.drawColor(Color.BLACK)

        // 計算文字在此頁的垂直偏移
        // virtual_y_start = pageNum * watchHeight（虛擬卷軸起始 y）
        // text 在虛擬卷軸的起點 = EXTRA_PADDING_PX
        // 因此 text_y_offset（在 canvas 上）= EXTRA_PADDING_PX - pageNum * watchHeight
        val textYOnCanvas = EXTRA_PADDING_PX - pageNum.toLong() * watchHeight

        // 繪製文字（StaticLayout 白色文字，canvas 裁切到頁面範圍）
        canvas.save()
        canvas.translate(0f, textYOnCanvas.toFloat())
        layout.draw(canvas)
        canvas.restore()

        // 轉換成 1-bit
        val result = encodeTo1Bit(bitmap)
        bitmap.recycle()

        return result
    }

    // ============================================================
    // 內部輔助
    // ============================================================

    /** 取得或建立 StaticLayout（內容不變時重用） */
    private fun getOrBuildLayout(content: String): StaticLayout {
        if (cachedContent == content && cachedLayout != null) {
            return cachedLayout!!
        }

        val paint = buildTextPaint()

        val layout = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            StaticLayout.Builder
                .obtain(content, 0, content.length, paint, watchWidth)
                .setAlignment(Layout.Alignment.ALIGN_NORMAL)
                .setLineSpacing(0f, LINE_SPACING_MULT)
                .setIncludePad(false)
                .build()
        } else {
            @Suppress("DEPRECATION")
            StaticLayout(
                content, paint, watchWidth,
                Layout.Alignment.ALIGN_NORMAL,
                LINE_SPACING_MULT, 0f, false
            )
        }

        cachedContent = content
        cachedLayout  = layout
        return layout
    }

    /** 建立文字繪製用的 TextPaint */
    private fun buildTextPaint(): TextPaint {
        return TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            color     = Color.WHITE
            textSize  = fontSizePx(textSizeLevel)
            typeface  = Typeface.create(Typeface.MONOSPACE, Typeface.NORMAL)
            // 關閉子像素渲染，避免彩色邊緣在二值化後產生雜訊
            isSubpixelText = false
        }
    }

    /**
     * 將 ARGB_8888 Bitmap 轉換成 Pebble GBitmapFormat1Bit 格式。
     *
     * 格式規範：
     *  - 每列以整數位元組儲存，row_stride = ceil(width / 8)
     *  - 位元 7（MSB）= 最左側像素，位元 0（LSB）= 第 7 個像素
     *  - 1 = 白色，0 = 黑色
     *  - 亮度閾值：> 64 視為白色（略低以讓白色文字更飽滿）
     */
    private fun encodeTo1Bit(bitmap: Bitmap): ByteArray {
        val width  = bitmap.width
        val height = bitmap.height
        val stride = (width + 7) / 8
        val result = ByteArray(stride * height)  // 預設全 0（黑色）

        // 一次取一列像素以降低 getPixel 呼叫次數
        val rowPixels = IntArray(width)
        for (y in 0 until height) {
            bitmap.getPixels(rowPixels, 0, width, 0, y, width, 1)
            val baseIndex = y * stride
            for (x in 0 until width) {
                val pixel = rowPixels[x]
                // 使用標準亮度公式（BT.601）
                val r = (pixel shr 16) and 0xFF
                val g = (pixel shr 8)  and 0xFF
                val b =  pixel         and 0xFF
                val luminance = (r * 299 + g * 587 + b * 114) / 1000

                if (luminance > 64) {   // 白色
                    val byteIdx = baseIndex + x / 8
                    val bitIdx  = 7 - (x % 8)  // MSB = leftmost
                    result[byteIdx] = (result[byteIdx].toInt() or (1 shl bitIdx)).toByte()
                }
            }
        }
        return result
    }

    /** 清除已快取的排版（文件更換或字型大小改變時呼叫） */
    fun invalidateCache() {
        cachedLayout     = null
        cachedContent    = null
        cachedTotalPages = -1
    }
}
