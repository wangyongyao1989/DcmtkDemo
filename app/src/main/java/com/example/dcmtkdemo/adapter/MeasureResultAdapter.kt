package com.example.dcmtkdemo.adapter

import android.annotation.SuppressLint
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.ImageButton
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.example.dcmtkdemo.R

/**
 * 结果列表的一行。
 *
 * 五类对象（测量 / ROI / 种植体 / 神经管 / 标注）字段差异很大，
 * 但列表里只需要"叫什么、读数是什么、能不能看见、要不要删"，
 * 所以在这里收敛成同一行模型，数值文本全部由 Fragment 从 Native 回传的
 * 字段直接格式化，适配器不做任何计算。
 *
 * ownerKind 用 com.wangyao.cbctmeasure.model.OverlayOwner 的取值，
 * 点击行时把它和 id 一起交给叠加层做选中高亮（精确匹配，不靠文字猜）。
 */
data class MeasureRow(
    val ownerKind: Int,
    val id: Int,
    val title: String,
    val value: String,
    val sub: String,
    val color: Int,
    val visible: Boolean,
)

class MeasureResultAdapter(
    private var rows: List<MeasureRow>
) : RecyclerView.Adapter<MeasureResultAdapter.ViewHolder>() {

    private var clickListener: ((MeasureRow) -> Unit)? = null
    private var visibleListener: ((MeasureRow, Boolean) -> Unit)? = null
    private var deleteListener: ((MeasureRow) -> Unit)? = null

    fun setOnItemClickListener(listener: (MeasureRow) -> Unit) {
        clickListener = listener
    }

    fun setOnVisibleChangeListener(listener: (MeasureRow, Boolean) -> Unit) {
        visibleListener = listener
    }

    fun setOnDeleteClickListener(listener: (MeasureRow) -> Unit) {
        deleteListener = listener
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_measure_result, parent, false)
        return ViewHolder(view)
    }

    @SuppressLint("SetTextI18n")
    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val row = rows[position]
        holder.tvTitle.text = row.title
        holder.tvValue.text = row.value
        holder.tvSub.text = row.sub
        holder.tvSub.visibility = if (row.sub.isEmpty()) View.GONE else View.VISIBLE
        holder.swatch.setBackgroundColor(row.color)

        // 复用场景下必须先摘监听再 setChecked，否则 setChecked 会回调到上一条数据的监听。
        // 用 adapterPosition 而非 bindingAdapterPosition：本工程 classpath 上是 recyclerview 1.1.0，
        // 后者要 1.2.0+ 才有，升级依赖不在本次改动范围内。
        holder.cbVisible.setOnCheckedChangeListener(null)
        holder.cbVisible.isChecked = row.visible
        holder.cbVisible.setOnCheckedChangeListener { _, checked ->
            val pos = holder.adapterPosition
            if (pos == RecyclerView.NO_POSITION) return@setOnCheckedChangeListener
            visibleListener?.invoke(rows[pos], checked)
        }

        holder.itemView.setOnClickListener {
            val pos = holder.adapterPosition
            if (pos != RecyclerView.NO_POSITION) clickListener?.invoke(rows[pos])
        }
        holder.btnDelete.setOnClickListener {
            val pos = holder.adapterPosition
            if (pos != RecyclerView.NO_POSITION) deleteListener?.invoke(rows[pos])
        }
    }

    override fun getItemCount(): Int = rows.size

    @SuppressLint("NotifyDataSetChanged")
    fun updateData(newRows: List<MeasureRow>) {
        this.rows = newRows
        notifyDataSetChanged()
    }

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val swatch: View = view.findViewById(R.id.v_swatch)
        val tvTitle: TextView = view.findViewById(R.id.tv_row_title)
        val tvValue: TextView = view.findViewById(R.id.tv_row_value)
        val tvSub: TextView = view.findViewById(R.id.tv_row_sub)
        val cbVisible: CheckBox = view.findViewById(R.id.cb_visible)
        val btnDelete: ImageButton = view.findViewById(R.id.btn_delete)
    }
}
