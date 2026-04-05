package com.peleprompter.ui

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
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
 * - 文字貼上 / .txt 匯入
 * - 匯入歷史管理
 * - 推送文字到手錶（從頭開始 / 從書籤繼續）
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
                // 取得持久化讀取權限
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

        // 處理外部 Intent (開啟 .txt 檔案)
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
        // 載入貼上的文字
        binding.btnLoadText.setOnClickListener {
            val text = binding.etTextContent.text.toString()
            if (text.isBlank()) {
                Toast.makeText(this, "Please paste some text first", Toast.LENGTH_SHORT).show()
            } else {
                viewModel.loadFromPastedText(text)
            }
        }

        // 匯入 .txt 檔案
        binding.btnImportFile.setOnClickListener {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "text/plain"
                addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            filePickerLauncher.launch(intent)
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
                    R.id.btnSizeSmall -> WatchSettings.SIZE_SMALL
                    R.id.btnSizeMedium -> WatchSettings.SIZE_MEDIUM
                    R.id.btnSizeLarge -> WatchSettings.SIZE_LARGE
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
        viewModel.currentDocument.observe(this) { doc ->
            if (doc != null) {
                binding.cardDocInfo.visibility = View.VISIBLE
                binding.tvDocTitle.text = doc.title
                binding.tvDocInfo.text = "${doc.totalLength} characters"

                // 顯示/隱藏 Resume 按鈕
                binding.btnPushResume.isEnabled = doc.hasBookmark
                if (doc.hasBookmark) {
                    binding.btnPushResume.text = "Resume (${doc.bookmarkOffset})"
                } else {
                    binding.btnPushResume.text = "Resume"
                }
            } else {
                binding.cardDocInfo.visibility = View.GONE
            }
        }

        viewModel.importHistory.observe(this) { history ->
            historyAdapter.submitList(history)
            binding.tvNoHistory.visibility = if (history.isEmpty()) View.VISIBLE else View.GONE
        }

        viewModel.watchConnected.observe(this) { connected ->
            binding.tvConnectionStatus.text = if (connected) "Connected" else "Disconnected"
            binding.tvConnectionStatus.setTextColor(
                if (connected) 0xFF03DAC5.toInt() else 0xFFFF5252.toInt()
            )
        }

        viewModel.watchSettings.observe(this) { settings ->
            binding.sliderSpeed.value = settings.scrollSpeed.toFloat()
            binding.tvSpeedValue.text = settings.scrollSpeed.toString()

            val sizeButtonId = when (settings.textSize) {
                WatchSettings.SIZE_SMALL -> R.id.btnSizeSmall
                WatchSettings.SIZE_LARGE -> R.id.btnSizeLarge
                else -> R.id.btnSizeMedium
            }
            binding.toggleTextSize.check(sizeButtonId)
        }

        viewModel.bookmarkOffset.observe(this) { offset ->
            val doc = viewModel.currentDocument.value ?: return@observe
            binding.btnPushResume.isEnabled = true
            binding.btnPushResume.text = "Resume ($offset)"
        }

        viewModel.statusMessage.observe(this) { msg ->
            binding.tvStatus.text = msg
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
