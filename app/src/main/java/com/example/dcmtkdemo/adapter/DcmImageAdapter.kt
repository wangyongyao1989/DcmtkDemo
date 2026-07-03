package com.example.dcmtkdemo.adapter

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.bumptech.glide.Glide
import com.example.dcmtk.model.DicomImageRecord
import com.example.dcmtkdemo.R
import java.io.File

/**
 * 用于 DcmShowFragment 列表的适配器。
 * 每个 item 展示转换后的 JPG 缩略图，以及 Patient Name / ID / Sex。
 */
class DcmImageAdapter(
    private var records: List<DicomImageRecord>,
    private val listener: (DicomImageRecord) -> Unit
) : RecyclerView.Adapter<DcmImageAdapter.ViewHolder>() {

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_dcm_image, parent, false)
        return ViewHolder(view)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val record = records[position]
        holder.tvName.text = record.name
        holder.tvId.text = "ID: ${record.id}"
        holder.tvSex.text = "Sex: ${record.sex}"

        // 使用 Glide 加载转换后的 JPG 缩略图
        Glide.with(holder.itemView.context)
            .load(record.jpgPath?.let { File(it) })
            .centerCrop()
            .placeholder(android.R.color.darker_gray)
            .error(android.R.drawable.ic_menu_gallery)
            .into(holder.ivThumb)

        holder.itemView.setOnClickListener { listener(record) }
    }

    override fun getItemCount(): Int = records.size

    fun updateData(newRecords: List<DicomImageRecord>) {
        this.records = newRecords
        notifyDataSetChanged()
    }

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val ivThumb: ImageView = view.findViewById(R.id.iv_dcm_thumb)
        val tvName: TextView = view.findViewById(R.id.tv_dcm_name)
        val tvId: TextView = view.findViewById(R.id.tv_dcm_id)
        val tvSex: TextView = view.findViewById(R.id.tv_dcm_sex)
    }
}
