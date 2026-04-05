package com.peleprompter.ui

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageButton
import android.widget.TextView
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.peleprompter.R
import com.peleprompter.model.ImportHistoryEntry
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * HistoryAdapter - 匯入歷史列表適配器
 */
class HistoryAdapter(
    private val onItemClick: (ImportHistoryEntry) -> Unit,
    private val onDeleteClick: (ImportHistoryEntry) -> Unit
) : ListAdapter<ImportHistoryEntry, HistoryAdapter.ViewHolder>(DiffCallback) {

    object DiffCallback : DiffUtil.ItemCallback<ImportHistoryEntry>() {
        override fun areItemsTheSame(a: ImportHistoryEntry, b: ImportHistoryEntry) = a.id == b.id
        override fun areContentsTheSame(a: ImportHistoryEntry, b: ImportHistoryEntry) = a == b
    }

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val tvTitle: TextView = view.findViewById(R.id.tvHistoryTitle)
        val tvPreview: TextView = view.findViewById(R.id.tvHistoryPreview)
        val tvBookmark: TextView = view.findViewById(R.id.tvHistoryBookmark)
        val btnDelete: ImageButton = view.findViewById(R.id.btnHistoryDelete)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_history, parent, false)
        return ViewHolder(view)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val entry = getItem(position)
        val dateFormat = SimpleDateFormat("MMM d, HH:mm", Locale.getDefault())

        holder.tvTitle.text = entry.title
        holder.tvPreview.text = "${dateFormat.format(Date(entry.importedAt))} — ${entry.contentPreview}"

        if (entry.bookmarkOffset > 0) {
            holder.tvBookmark.visibility = View.VISIBLE
            holder.tvBookmark.text = "Bookmark at offset ${entry.bookmarkOffset}"
        } else {
            holder.tvBookmark.visibility = View.GONE
        }

        holder.itemView.setOnClickListener { onItemClick(entry) }
        holder.btnDelete.setOnClickListener { onDeleteClick(entry) }
    }
}
