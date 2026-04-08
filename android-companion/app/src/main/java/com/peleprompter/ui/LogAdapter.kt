package com.peleprompter.ui

import android.graphics.Color
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.peleprompter.R
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * LogAdapter
 *
 * RecyclerView Adapter，顯示 WatchSimulator 的協定日誌。
 * 最新條目置頂（prepend），保留最多 MAX_ENTRIES 筆以防無限增長。
 */
class LogAdapter : RecyclerView.Adapter<LogAdapter.ViewHolder>() {

    companion object {
        private const val MAX_ENTRIES = 300
    }

    private val entries    = mutableListOf<LogEntry>()
    private val timeFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault())

    // ── 顏色對應 ────────────────────────────────────────────────
    private val colorTx    = Color.parseColor("#64B5F6")  // 藍：手錶→手機
    private val colorRx    = Color.parseColor("#81C784")  // 綠：手機→手錶
    private val colorInfo  = Color.parseColor("#B0BEC5")  // 灰：一般資訊
    private val colorWarn  = Color.parseColor("#FFB74D")  // 橙：警告
    private val colorError = Color.parseColor("#EF9A9A")  // 紅：錯誤

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val tvTime:    TextView = view.findViewById(R.id.tvLogTime)
        val tvTag:     TextView = view.findViewById(R.id.tvLogTag)
        val tvMessage: TextView = view.findViewById(R.id.tvLogMessage)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_log_entry, parent, false)
        return ViewHolder(view)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val entry = entries[position]
        holder.tvTime.text    = timeFormat.format(Date(entry.timestamp))
        holder.tvTag.text     = "[${entry.tag}]"
        holder.tvMessage.text = entry.message

        val color = when (entry.type) {
            LogType.TX    -> colorTx
            LogType.RX    -> colorRx
            LogType.INFO  -> colorInfo
            LogType.WARN  -> colorWarn
            LogType.ERROR -> colorError
        }
        holder.tvTag.setTextColor(color)
        holder.tvMessage.setTextColor(color)
    }

    override fun getItemCount(): Int = entries.size

    /** 將最新條目插入最前面，超過上限時刪除最舊的 */
    fun addEntry(entry: LogEntry) {
        entries.add(0, entry)
        notifyItemInserted(0)
        if (entries.size > MAX_ENTRIES) {
            entries.removeAt(entries.size - 1)
            notifyItemRemoved(entries.size)
        }
    }

    /** 清空所有日誌 */
    fun clearAll() {
        val size = entries.size
        entries.clear()
        notifyItemRangeRemoved(0, size)
    }
}
