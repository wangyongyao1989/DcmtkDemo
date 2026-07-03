package com.example.dcmtkdemo.adapter

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.example.dcmtk.model.PatientRecord
import com.example.dcmtkdemo.R

class PatientAdapter(
    private var records: List<PatientRecord>,
    private val listener: (PatientRecord) -> Unit
) : RecyclerView.Adapter<PatientAdapter.ViewHolder>() {

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context).inflate(R.layout.item_patient, parent, false)
        return ViewHolder(view)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val record = records[position]
        holder.tvName.text = record.name
        holder.tvId.text = "ID: ${record.id}"
        holder.tvSex.text = "Sex: ${record.sex}"
        holder.tvBirth.text = "Birth: ${record.birthDate}"

        holder.cbSelect.setOnCheckedChangeListener(null)
        holder.cbSelect.isChecked = record.isSelected
        holder.cbSelect.setOnCheckedChangeListener { _, isChecked ->
            record.isSelected = isChecked
        }

        holder.ivDownloaded.visibility = if (record.isDownloaded) View.VISIBLE else View.GONE

        holder.itemView.setOnClickListener {
            listener(record)
        }
    }

    override fun getItemCount(): Int = records.size

    fun updateData(newRecords: List<PatientRecord>) {
        this.records = newRecords
        notifyDataSetChanged()
    }

    val selectedRecords: List<PatientRecord>
        get() = records.filter { it.isSelected }

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val tvName: TextView = view.findViewById(R.id.tv_patient_name)
        val tvId: TextView = view.findViewById(R.id.tv_patient_id)
        val tvSex: TextView = view.findViewById(R.id.tv_patient_sex)
        val tvBirth: TextView = view.findViewById(R.id.tv_patient_birth)
        val cbSelect: CheckBox = view.findViewById(R.id.cb_select)
        val ivDownloaded: ImageView = view.findViewById(R.id.iv_downloaded)
    }
}
