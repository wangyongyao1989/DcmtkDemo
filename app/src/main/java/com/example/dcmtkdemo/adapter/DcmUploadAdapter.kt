package com.example.dcmtkdemo.adapter;

import android.annotation.SuppressLint;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.bumptech.glide.Glide;
import com.example.dcmtkdemo.R;
import com.example.dcmtk.model.DicomImageRecord;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class DcmUploadAdapter extends RecyclerView.Adapter<DcmUploadAdapter.ViewHolder> {

    private List<DicomImageRecord> records;
    private boolean uploadMode = false;
    private OnItemClickListener listener;

    public interface OnItemClickListener {
        void onItemClick(DicomImageRecord record);
    }

    public DcmUploadAdapter(List<DicomImageRecord> records) {
        this.records = records;
    }

    public void setOnItemClickListener(OnItemClickListener listener) {
        this.listener = listener;
    }

    @SuppressLint("NotifyDataSetChanged")
    public void setUploadMode(boolean uploadMode) {
        this.uploadMode = uploadMode;
        notifyDataSetChanged();
    }

    public boolean isUploadMode() {
        return uploadMode;
    }

    @NonNull
    @Override
    public ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_dcm_upload, parent, false);
        return new ViewHolder(view);
    }

    @SuppressLint("SetTextI18n")
    @Override
    public void onBindViewHolder(@NonNull ViewHolder holder, int position) {
        DicomImageRecord record = records.get(position);
        holder.tvName.setText(record.getName());
        holder.tvId.setText("ID: " + record.getId());
        holder.tvSex.setText("Sex: " + record.getSex());
        
        holder.cbSelect.setVisibility(uploadMode ? View.VISIBLE : View.GONE);
        holder.cbSelect.setChecked(record.isSelected());

        Glide.with(holder.itemView.getContext())
                .load(record.getJpgPath() != null ? new File(record.getJpgPath()) : null)
                .centerCrop()
                .placeholder(android.R.color.darker_gray)
                .error(android.R.drawable.ic_menu_gallery)
                .into(holder.ivThumb);

        holder.itemView.setOnClickListener(v -> {
            if (uploadMode) {
                record.setSelected(!record.isSelected());
                notifyItemChanged(position);
            } else {
                if (listener != null) {
                    listener.onItemClick(record);
                }
            }
        });

        holder.cbSelect.setOnClickListener(v -> {
            record.setSelected(!record.isSelected());
            notifyItemChanged(position);
        });

    }

    @Override
    public int getItemCount() {
        return records.size();
    }

    @SuppressLint("NotifyDataSetChanged")
    public void updateData(List<DicomImageRecord> newRecords) {
        this.records = newRecords;
        notifyDataSetChanged();
    }

    public List<DicomImageRecord> getSelectedRecords() {
        List<DicomImageRecord> selected = new ArrayList<>();
        for (DicomImageRecord r : records) {
            if (r.isSelected()) selected.add(r);
        }
        return selected;
    }

    static class ViewHolder extends RecyclerView.ViewHolder {
        ImageView ivThumb;
        TextView tvName, tvId, tvSex;
        CheckBox cbSelect;

        ViewHolder(View view) {
            super(view);
            ivThumb = view.findViewById(R.id.iv_dcm_thumb);
            tvName = view.findViewById(R.id.tv_dcm_name);
            tvId = view.findViewById(R.id.tv_dcm_id);
            tvSex = view.findViewById(R.id.tv_dcm_sex);
            cbSelect = view.findViewById(R.id.cb_select);
        }
    }
}
