package com.peleprompter.ui

import android.app.Activity
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModelProvider
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.slider.Slider
import com.peleprompter.R
import com.peleprompter.databinding.ActivityMainBinding
import com.peleprompter.model.WatchSettings

/**
 * MainActivity - Peleprompter Android 主畫面
 *
 * 功能：
 * - 文字貼上 / .txt 匯入，以來源狀態列清楚顯示目前載入的稿件
 * - 匯入歷史管理
 * - 推送稿件到手錶（從頭開始 / 從書籤繼續）
 * - 遠端控制手錶的捲動速度和文字大小
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var viewModel: MainViewModel
    private lateinit var historyAdapter: HistoryAdapter

    // 檔案選擇器啟動器
    private val filePickerLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            result.data?.data?.let { uri ->
                try {
                    contentResolver.takePersistableUriPermission(
                        uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                    )
                } catch (_: Exception) { /* 部分裝置不支援 */ }

                val fileName = getFileNameFromUri(uri)
                viewModel.loadFromFileUri(uri, fileName)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        viewModel = ViewModelProvider(this)[MainViewModel::class.java]

        setupHistoryList()
        setupListeners()
        observeViewModel()

        handleIncomingIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        viewModel.registerComm()
    }

    override fun onPause() {
        super.onPause()
        viewModel.unregisterComm()
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        intent?.let { handleIncomingIntent(it) }
    }

    // ================================================================
    // 初始化
    // ================================================================

    private fun setupHistoryList() {
        historyAdapter = HistoryAdapter(
            onItemClick = { entry ->
                viewModel.reloadFromHistory(entry)
            },
            onDeleteClick = { entry ->
                AlertDialog.Builder(this)
                    .setTitle("Delete Entry")
                    .setMessage("Remove \"${entry.title}\" from history?")
                    .setPositiveButton("Delete") { _, _ ->
                        viewModel.deleteHistoryEntry(entry.id)
                    }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
        )

        binding.rvHistory.apply {
            layoutManager = LinearLayoutManager(this@MainActivity)
            adapter = historyAdapter
        }
    }

    private fun setupListeners() {
        // TextWatcher：有內容時才啟用「Use Pasted Text」按鈕
        binding.etTextContent.addTextChangedListener(object : TextWatcher {
            override fun afterTextChanged(s: Editable?) {
                binding.btnLoadText.isEnabled = !s.isNullOrBlank()
            }
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
        })

        // 使用貼上的文字
        binding.btnLoadText.setOnClickListener {
            val text = binding.etTextContent.text.toString()
            if (text.isBlank()) {
                Toast.makeText(this, "Please paste some text first", Toast.LENGTH_SHORT).show()
            } else {
                viewModel.loadFromPastedText(text)
                binding.etTextContent.clearFocus()
            }
        }

        // 匯入 .txt / .epub 檔案
        binding.btnImportFile.setOnClickListener {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES,
                    arrayOf("text/plain", "application/epub+zip"))
                addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            filePickerLauncher.launch(intent)
        }

        // 清除目前稿件
        binding.btnClearScript.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Clear Script")
                .setMessage("Remove the current script?")
                .setPositiveButton("Clear") { _, _ ->
                    viewModel.clearCurrentDocument()
                    binding.etTextContent.setText("")
                }
                .setNegativeButton("Cancel", null)
                .show()
        }

        // 推送到手錶
        binding.btnPushFromStart.setOnClickListener {
            viewModel.pushToWatchFromBeginning()
        }

        binding.btnPushResume.setOnClickListener {
            viewModel.pushToWatchFromBookmark()
        }

        // 捲動速度滑桿
        binding.sliderSpeed.addOnChangeListener(Slider.OnChangeListener { _, value, fromUser ->
            if (fromUser) {
                val speed = value.toInt()
                binding.tvSpeedValue.text = speed.toString()
                viewModel.setScrollSpeed(speed)
            }
        })

        // 文字大小切換
        binding.toggleTextSize.addOnButtonCheckedListener { _, checkedId, isChecked ->
            if (isChecked) {
                val size = when (checkedId) {
                    R.id.btnSizeTiny   -> WatchSettings.SIZE_TINY
                    R.id.btnSizeSmall  -> WatchSettings.SIZE_SMALL
                    R.id.btnSizeMedium -> WatchSettings.SIZE_MEDIUM
                    R.id.btnSizeLarge  -> WatchSettings.SIZE_LARGE
                    R.id.btnSizeXLarge -> WatchSettings.SIZE_XLARGE
                    else -> return@addOnButtonCheckedListener
                }
                viewModel.setTextSize(size)
            }
        }
    }

    // ================================================================
    // ViewModel 觀察
    // ================================================================

    private fun observeViewModel() {
        // 來源類型變更 → 更新來源狀態列
        viewModel.sourceType.observe(this) { sourceType ->
            updateSourceBadge(sourceType)
        }

        // 文件載入 → 啟用「Send to Watch」按鈕並更新提示
        viewModel.currentDocument.observe(this) { doc ->
            val loaded = doc != null
            binding.btnPushFromStart.isEnabled = loaded
            binding.btnPushResume.isEnabled = loaded && doc!!.hasBookmark
            binding.btnPushResume.text = getString(R.string.btn_resume)

            if (loaded) {
                binding.tvSendHint.text = "“${doc!!.title}”  ·  ${doc.totalLength} characters"
                binding.tvSendHint.setTextColor(ContextCompat.getColor(this, R.color.text_secondary))
            } else {
                binding.tvSendHint.text = getString(R.string.send_hint_disabled)
                binding.tvSendHint.setTextColor(ContextCompat.getColor(this, R.color.text_tertiary))
            }
        }

        viewModel.importHistory.observe(this) { history ->
            historyAdapter.submitList(history)
            binding.tvNoHistory.visibility = if (history.isEmpty()) View.VISIBLE else View.GONE
        }

        viewModel.watchConnected.observe(this) { connected ->
            binding.tvConnectionStatus.text =
                if (connected) "● ${getString(R.string.status_connected)}"
                else "● ${getString(R.string.status_disconnected)}"
            binding.tvConnectionStatus.setTextColor(
                ContextCompat.getColor(this, if (connected) R.color.success else R.color.danger)
            )
        }

        viewModel.watchSettings.observe(this) { settings ->
            binding.sliderSpeed.value = settings.scrollSpeed.toFloat()
            binding.tvSpeedValue.text = settings.scrollSpeed.toString()

            val sizeButtonId = when (settings.textSize) {
                WatchSettings.SIZE_TINY   -> R.id.btnSizeTiny
                WatchSettings.SIZE_SMALL  -> R.id.btnSizeSmall
                WatchSettings.SIZE_LARGE  -> R.id.btnSizeLarge
                WatchSettings.SIZE_XLARGE -> R.id.btnSizeXLarge
                else                      -> R.id.btnSizeMedium
            }
            binding.toggleTextSize.check(sizeButtonId)
        }

        viewModel.bookmarkOffset.observe(this) { _ ->
            // 手錶回報閱讀進度後即可使用「Resume」（不顯示原始位移數字，避免誤會）
            binding.btnPushResume.isEnabled = true
        }

        viewModel.statusMessage.observe(this) { msg ->
            binding.tvStatus.text = msg
        }
    }

    // ================================================================
    // 來源狀態列更新
    // ================================================================

    private fun updateSourceBadge(sourceType: MainViewModel.SourceType) {
        val doc = viewModel.currentDocument.value

        when (sourceType) {
            MainViewModel.SourceType.NONE -> {
                binding.layoutSourceBadge.background =
                    ContextCompat.getDrawable(this, R.drawable.source_badge_empty)
                binding.tvSourceIcon.text = ""
                binding.tvSourceName.text = "No script loaded"
                binding.tvSourceName.setTextColor(0xFF555555.toInt())
                binding.tvSourceName.setTypeface(null, Typeface.ITALIC)
                binding.tvSourceDetail.visibility = View.GONE
                binding.btnClearScript.visibility = View.GONE
            }

            MainViewModel.SourceType.PASTE -> {
                binding.layoutSourceBadge.background =
                    ContextCompat.getDrawable(this, R.drawable.source_badge_loaded)
                binding.tvSourceIcon.text = "📋"
                binding.tvSourceName.text = "Pasted text"
                binding.tvSourceName.setTextColor(0xFF90CAF9.toInt())
                binding.tvSourceName.setTypeface(null, Typeface.BOLD)
                doc?.let {
                    binding.tvSourceDetail.text = "${it.totalLength} characters"
                    binding.tvSourceDetail.visibility = View.VISIBLE
                }
                binding.btnClearScript.visibility = View.VISIBLE
            }

            MainViewModel.SourceType.FILE -> {
                binding.layoutSourceBadge.background =
                    ContextCompat.getDrawable(this, R.drawable.source_badge_loaded)
                binding.tvSourceIcon.text = "📄"
                binding.tvSourceName.text = doc?.title ?: "Imported file"
                binding.tvSourceName.setTextColor(0xFF90CAF9.toInt())
                binding.tvSourceName.setTypeface(null, Typeface.BOLD)
                doc?.let {
                    binding.tvSourceDetail.text = "${it.totalLength} characters"
                    binding.tvSourceDetail.visibility = View.VISIBLE
                }
                binding.btnClearScript.visibility = View.VISIBLE
            }
        }
    }

    // ================================================================
    // 輔助
    // ================================================================

    private fun handleIncomingIntent(intent: Intent) {
        if (intent.action == Intent.ACTION_VIEW) {
            intent.data?.let { uri ->
                val fileName = getFileNameFromUri(uri)
                viewModel.loadFromFileUri(uri, fileName)
            }
        }
    }

    private fun getFileNameFromUri(uri: Uri): String? {
        var name: String? = null
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (nameIndex >= 0 && cursor.moveToFirst()) {
                name = cursor.getString(nameIndex)
            }
        }
        return name
    }
}
