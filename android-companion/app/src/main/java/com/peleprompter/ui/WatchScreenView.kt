package com.peleprompter.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View

/**
 * WatchScreenView
 *
 * 模擬 Pebble 手錶螢幕的自訂 View。
 *
 * 功能：
 *  - 保持手錶螢幕長寬比（watchWidth × watchHeight），最寬 240dp
 *  - 跨頁縫合：以 yInTop 為邊界，上半部顯示 topBitmap 的底部區域，
 *    下半部顯示 bottomBitmap 的頂部區域，還原 C 端兩頁拼接的捲動效果
 *  - Chalk (圓形) 平台支援圓形裁切
 *  - 頁面尚未就緒時顯示「Loading…」提示
 */
class WatchScreenView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    // ── 手錶規格 ────────────────────────────────────────────────
    var watchWidth:  Int     = 144
    var watchHeight: Int     = 168
    var isCircular:  Boolean = false

    // ── 目前顯示內容 ────────────────────────────────────────────
    private var topBitmap:    Bitmap? = null
    private var bottomBitmap: Bitmap? = null
    /** topBitmap 中可見區域的起始 y（像素） */
    private var yInTop:    Int = 0
    /** 頁面高度（= watchHeight） */
    private var pageHeight: Int = 168

    // ── 畫筆 ────────────────────────────────────────────────────
    private val bgPaint = Paint().apply { color = Color.BLACK }

    private val borderPaint = Paint().apply {
        color       = Color.parseColor("#555555")
        style       = Paint.Style.STROKE
        strokeWidth = 3f
        isAntiAlias = true
    }

    private val loadingPaint = Paint().apply {
        color     = Color.DKGRAY
        textSize  = 36f
        textAlign = Paint.Align.CENTER
        isAntiAlias = true
        typeface  = android.graphics.Typeface.MONOSPACE
    }

    // ── 暫存 Rect，避免 onDraw 每次分配 ─────────────────────────
    private val srcRect = Rect()
    private val dstRect = RectF()
    private val clipPath = Path()

    // ================================================================
    // 公開 API
    // ================================================================

    /**
     * 更新顯示內容並重繪。
     *
     * @param top        目前「頂部頁面」的已解碼 Bitmap（可為 null = 載入中）
     * @param bottom     緊接在下一頁的 Bitmap（僅當 yInTopPage > 0 時有意義）
     * @param yInTopPage 滾動位置距頂部頁面頂端的像素偏移
     * @param pagePx     頁面高度（= watchHeight）
     */
    fun update(top: Bitmap?, bottom: Bitmap?, yInTopPage: Int, pagePx: Int) {
        topBitmap    = top
        bottomBitmap = bottom
        yInTop       = yInTopPage
        pageHeight   = pagePx
        invalidate()
    }

    // ================================================================
    // 量測：固定寬高比，最大寬度 240dp
    // ================================================================

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val density   = resources.displayMetrics.density
        val maxPxWide = (240 * density).toInt()
        val specWidth = MeasureSpec.getSize(widthMeasureSpec)
        val w = minOf(specWidth, maxPxWide)
        val h = if (watchWidth > 0) w * watchHeight / watchWidth else w
        setMeasuredDimension(w, h)
    }

    // ================================================================
    // 繪製
    // ================================================================

    override fun onDraw(canvas: Canvas) {
        val vw = width.toFloat()
        val vh = height.toFloat()
        val ph = pageHeight.takeIf { it > 0 } ?: return

        // 黑色背景
        canvas.drawRect(0f, 0f, vw, vh, bgPaint)

        // 圓形裁切（Chalk 平台）
        if (isCircular) {
            clipPath.reset()
            clipPath.addCircle(vw / 2f, vh / 2f, minOf(vw, vh) / 2f, Path.Direction.CW)
            canvas.save()
            canvas.clipPath(clipPath)
        }

        drawContent(canvas, vw, vh, ph)

        if (isCircular) canvas.restore()

        // 外框
        if (isCircular) {
            canvas.drawCircle(vw / 2f, vh / 2f, minOf(vw, vh) / 2f - 1.5f, borderPaint)
        } else {
            canvas.drawRect(1.5f, 1.5f, vw - 1.5f, vh - 1.5f, borderPaint)
        }
    }

    // ================================================================
    // 內部：跨頁縫合
    // ================================================================

    /**
     * 模擬 C 端 canvas_update_proc 的兩頁拼接邏輯：
     *
     *   rowsFromTop = pageHeight - yInTop   （topBitmap 中可見的行數）
     *   螢幕 [0 .. rowsFromTop)  ← topBitmap 的 [yInTop .. pageHeight)
     *   螢幕 [rowsFromTop .. vh) ← bottomBitmap 的 [0 .. yInTop)
     */
    private fun drawContent(canvas: Canvas, vw: Float, vh: Float, ph: Int) {
        val rowsFromTop = ph - yInTop   // 頂部頁面可見行數

        // ── 頂部頁面 ────────────────────────────────────────────
        val topBmp = topBitmap
        if (topBmp != null && !topBmp.isRecycled) {
            val srcTop    = yInTop.coerceIn(0, topBmp.height)
            val srcBottom = (yInTop + rowsFromTop).coerceAtMost(topBmp.height)
            if (srcBottom > srcTop) {
                srcRect.set(0, srcTop, topBmp.width.coerceAtMost(watchWidth), srcBottom)
                val dstH = rowsFromTop.toFloat() / ph * vh
                dstRect.set(0f, 0f, vw, dstH)
                canvas.drawBitmap(topBmp, srcRect, dstRect, null)
            }
        } else {
            // 頁面尚未就緒
            canvas.drawText("Loading…", vw / 2f, vh / 2f, loadingPaint)
        }

        // ── 底部頁面（僅在跨頁時需要） ─────────────────────────
        if (yInTop > 0) {
            val bottomBmp = bottomBitmap
            if (bottomBmp != null && !bottomBmp.isRecycled) {
                val srcBottom = yInTop.coerceAtMost(bottomBmp.height)
                if (srcBottom > 0) {
                    srcRect.set(0, 0, bottomBmp.width.coerceAtMost(watchWidth), srcBottom)
                    val topPortionH = rowsFromTop.toFloat() / ph * vh
                    dstRect.set(0f, topPortionH, vw, vh)
                    canvas.drawBitmap(bottomBmp, srcRect, dstRect, null)
                }
            }
        }
    }
}
