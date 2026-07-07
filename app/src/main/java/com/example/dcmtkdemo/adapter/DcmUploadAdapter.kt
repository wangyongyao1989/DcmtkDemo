package com.example.dcmtkdemo.adapter

import android.annotation.SuppressLint
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.bumptech.glide.Glide
import com.example.dcmtk.model.DicomImageRecord
import com.example.dcmtkdemo.R
import java.io.File

class DcmUploadAdapter(
    private var records: List<DicomImageRecord>
) : RecyclerView.Adapter<DcmUploadAdapter.ViewHolder>() {

    var isUploadMode = false
        @SuppressLint("NotifyDataSetChanged")
        set(value) {
            field = value
            notifyDataSetChanged()
        }

    private var listener: ((DicomImageRecord) -> Unit)? = null

    fun setOnItemClickListener(listener: (DicomImageRecord) -> Unit) {
        this.listener = listener
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_dcm_upload, parent, false)
        return ViewHolder(view)
    }

    @SuppressLint("SetTextI18n")
    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val record = records[position]
        holder.tvName.text = record.name
        holder.tvId.text = "ID: ${record.id}"
        holder.tvSex.text = "Sex: ${record.sex}"

        holder.cbSelect.visibility = if (isUploadMode) View.VISIBLE else View.GONE
        holder.cbSelect.isChecked = record.isSelected
        holder.ivUploaded.visibility = if (record.isUploaded) View.VISIBLE else View.GONE

        Glide.with(holder.itemView.context)
            .load(record.jpgPath?.let { File(it) })
            .centerCrop()
            .placeholder(android.R.color.darker_gray)
            .error(android.R.drawable.ic_menu_gallery)
            .into(holder.ivThumb)

        holder.itemView.setOnClickListener {
            if (isUploadMode) {
                record.isSelected = !record.isSelected
                notifyItemChanged(position)
            } else {
                listener?.invoke(record)
            }
        }

        holder.cbSelect.setOnClickListener {
            record.isSelected = !record.isSelected
            notifyItemChanged(position)
        }
    }

    override fun getItemCount(): Int = records.size

    @SuppressLint("NotifyDataSetChanged")
    fun updateData(newRecords: List<DicomImageRecord>) {
        this.records = newRecords
        notifyDataSetChanged()
    }

    val selectedRecords: List<DicomImageRecord>
        get() = records.filter { it.isSelected }

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val ivThumb: ImageView = view.findViewById(R.id.iv_dcm_thumb)
        val ivUploaded: ImageView = view.findViewById(R.id.iv_uploaded)
        val tvName: TextView = view.findViewById(R.id.tv_dcm_name)
        val tvId: TextView = view.findViewById(R.id.tv_dcm_id)
        val tvSex: TextView = view.findViewById(R.id.tv_dcm_sex)
        val cbSelect: CheckBox = view.findViewById(R.id.cb_select)
    }
}
