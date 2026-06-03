package com.peleprompter.ui

import android.os.Bundle
import android.view.MenuItem
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.slider.Slider
import com.peleprompter.PeleprompterApp
import com.peleprompter.R
import com.peleprompter.databinding.ActivityWatchEmulatorBinding
import com.peleprompter.util.PebbleProtocol

/**
 * WatchEmulatorActivity
 *
 * 提供一個全功能的手錶模擬測試頁面：
 *  - 平台選擇（Aplite / Basalt / Chalk / Diorite / Emery）
 *  - 文字大小切換（Small / Medium / Large）
 *  - 模擬手錶螢幕（WatchScreenView，保持長寬比，支援圓形裁切）
 *  - 上 / 下 / Select 三鍵模擬
 *  - 自動捲動速度滑桿
 *  - 模擬連線/斷線
 *  - 底部協定日誌面板（LogAdapter）
 *
 * 需要先在主畫面載入文件，否則顯示錯誤訊息。
 */
class WatchEmulatorActivity : AppCompatActivity() {

    private lateinit var binding: ActivityWatchEmulatorBinding
    private lateinit var logAdapter: LogAdapter
    private var simulator: WatchSimulator? = null

    private var selectedPlatform  = WatchPlatform.BASALT
    private var selectedTextSize  = PebbleProtocol.TEXT_SIZE_MEDIUM

    // ================================================================
    // 生命週期
    // ================================================================

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityWatchEmulatorBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // 支援返回鍵
        supportActionBar?.apply {
            setDisplayHomeAsUpEnabled(true)
            title = "Preview on phone"
        }

        setupLog()
        setupPlatformSpinner()
        setupTextSizeToggle()
        setupButtons()
        setupSpeedSlider()
        setupConnectionControls()

        startSimulator()
    }

    override fun onDestroy() {
        super.onDestroy()
        simulator?.cleanup()
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == android.R.id.home) { finish(); return true }
        return super.onOptionsItemSelected(item)
    }

    // ================================================================
    // 初始化
    // ================================================================

    private fun setupLog() {
        logAdapter = LogAdapter()
        binding.rvLog.apply {
            layoutManager = LinearLayoutManager(this@WatchEmulatorActivity)
            adapter        = logAdapter
        }
        binding.btnClearLog.setOnClickListener { logAdapter.clearAll() }
    }

    private fun setupPlatformSpinner() {
        val platforms = WatchPlatform.values()
        val names     = platforms.map { it.displayName }.toTypedArray()
        val adapter   = ArrayAdapter(this, android.R.layout.simple_spinner_item, names)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        binding.spinnerPlatform.adapter = adapter
        // 預設選 Basalt（index 1）
        binding.spinnerPlatform.setSelection(1)

        binding.spinnerPlatform.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, pos: Int, id: Long) {
                val newPlatform = platforms[pos]
                if (newPlatform != selectedPlatform) {
                    selectedPlatform = newPlatform
                    updateWatchScreenSpec()
                    restartSimulator()
                }
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
    }

    private fun setupTextSizeToggle() {
        binding.toggleTextSize.addOnButtonCheckedListener { _, checkedId, isChecked ->
            if (!isChecked) return@addOnButtonCheckedListener
            val newSize = when (checkedId) {
                R.id.btnEmuSizeTiny   -> PebbleProtocol.TEXT_SIZE_TINY
                R.id.btnEmuSizeSmall  -> PebbleProtocol.TEXT_SIZE_SMALL
                R.id.btnEmuSizeMedium -> PebbleProtocol.TEXT_SIZE_MEDIUM
                R.id.btnEmuSizeLarge  -> PebbleProtocol.TEXT_SIZE_LARGE
                R.id.btnEmuSizeXLarge -> PebbleProtocol.TEXT_SIZE_XLARGE
                else -> return@addOnButtonCheckedListener
            }
            if (newSize != selectedTextSize) {
                selectedTextSize = newSize
                restartSimulator()
            }
        }
        binding.toggleTextSize.check(R.id.btnEmuSizeMedium)
    }

    private fun setupButtons() {
        binding.btnScrollUp.setOnClickListener   { simulator?.pressUp() }
        binding.btnScrollDown.setOnClickListener { simulator?.pressDown() }
        binding.btnSelect.setOnClickListener {
            simulator?.pressSelect()
            updateSelectButtonLabel()
        }
    }

    private fun setupSpeedSlider() {
        binding.sliderSpeed.addOnChangeListener(Slider.OnChangeListener { _, value, fromUser ->
            if (fromUser) {
                val speed = value.toInt()
                binding.tvSpeedValue.text = speed.toString()
                simulator?.scrollSpeed = speed
            }
        })
    }

    private fun setupConnectionControls() {
        binding.btnDisconnect.setOnClickListener { simulator?.simulateDisconnect() }
        binding.btnReconnect.setOnClickListener  { simulator?.simulateReconnect() }
    }

    // ================================================================
    // 模擬器生命週期
    // ================================================================

    private fun startSimulator() {
        val doc = PeleprompterApp.instance.currentDocument

        if (doc == null) {
            logAdapter.addEntry(LogEntry(
                type    = LogType.ERROR,
                tag     = "SIM",
                message = "未載入文件。請先在主畫面載入文字後再開啟模擬器。"
            ))
            binding.watchScreen.update(null, null, 0, selectedPlatform.screenHeight)
            binding.tvScrollInfo.text = "No document loaded"
            return
        }

        updateWatchScreenSpec()

        simulator = WatchSimulator(
            document      = doc,
            platform      = selectedPlatform,
            textSizeLevel = selectedTextSize,
            onLog         = { entry ->
                runOnUiThread {
                    logAdapter.addEntry(entry)
                    // 最新一筆自動捲到頂端
                    binding.rvLog.scrollToPosition(0)
                }
            },
            onDisplayUpdate = { topBmp, bottomBmp, yInTop, pageH ->
                binding.watchScreen.update(topBmp, bottomBmp, yInTop, pageH)
                updateScrollInfo()
                updateSelectButtonLabel()
            }
        ).also { sim ->
            sim.scrollSpeed = binding.sliderSpeed.value.toInt()
            sim.initialize()
        }
    }

    private fun restartSimulator() {
        simulator?.cleanup()
        simulator = null
        logAdapter.clearAll()
        startSimulator()
    }

    // ================================================================
    // UI 更新
    // ================================================================

    private fun updateWatchScreenSpec() {
        binding.watchScreen.watchWidth  = selectedPlatform.screenWidth
        binding.watchScreen.watchHeight = selectedPlatform.screenHeight
        binding.watchScreen.isCircular  = selectedPlatform.isCircular
        binding.watchScreen.requestLayout()
    }

    private fun updateScrollInfo() {
        val sim = simulator ?: return
        val page = sim.scrollPx / selectedPlatform.screenHeight + 1
        binding.tvScrollInfo.text =
            "scroll: ${sim.scrollPx}px | page $page / ${sim.totalPages}"
    }

    private fun updateSelectButtonLabel() {
        val autoOn = simulator?.isAutoScrollActive ?: false
        binding.btnSelect.text = if (autoOn) "❚❚\nStop" else "▶\nAuto"
    }
}
