package com.example.dcmtkdemo.adapter;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.example.dcmtkdemo.R;
import com.example.dcmtkdemo.model.PatientRecord;

import java.util.List;

public class PatientAdapter extends RecyclerView.Adapter<PatientAdapter.ViewHolder> {

    private List<PatientRecord> records;
    private OnItemClickListener listener;

    public interface OnItemClickListener {
        void onItemClick(PatientRecord record);
    }

    public PatientAdapter(List<PatientRecord> records, OnItemClickListener listener) {
        this.records = records;
        this.listener = listener;
    }

    @NonNull
    @Override
    public ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_patient, parent, false);
        return new ViewHolder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull ViewHolder holder, int position) {
        PatientRecord record = records.get(position);
        holder.tvName.setText(record.getName());
        holder.tvId.setText("ID: " + record.getId());
        holder.tvSex.setText("Sex: " + record.getSex());
        holder.tvBirth.setText("Birth: " + record.getBirthDate());
        holder.itemView.setOnClickListener(v -> listener.onItemClick(record));
    }

    @Override
    public int getItemCount() {
        return records.size();
    }

    public void updateData(List<PatientRecord> newRecords) {
        this.records = newRecords;
        notifyDataSetChanged();
    }

    static class ViewHolder extends RecyclerView.ViewHolder {
        TextView tvName, tvId, tvSex, tvBirth;

        ViewHolder(View view) {
            super(view);
            tvName = view.findViewById(R.id.tv_patient_name);
            tvId = view.findViewById(R.id.tv_patient_id);
            tvSex = view.findViewById(R.id.tv_patient_sex);
            tvBirth = view.findViewById(R.id.tv_patient_birth);
        }
    }
}
