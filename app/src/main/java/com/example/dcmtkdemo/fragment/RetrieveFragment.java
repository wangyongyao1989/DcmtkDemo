package com.example.dcmtkdemo.fragment;

import android.annotation.SuppressLint;
import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.LinearLayoutManager;

import com.example.dcmtkdemo.adapter.PatientAdapter;
import com.example.dcmtk.callback.ProgressCallback;
import com.example.dcmtkdemo.databinding.FragmentRetrieveBinding;
import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.model.PatientRecord;
import com.example.dcmtkdemo.viewmodel.PacsViewModel;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class RetrieveFragment extends Fragment {

    private FragmentRetrieveBinding binding;
    private PacsViewModel viewModel;
    private PatientAdapter adapter;

    private long lastBytes = 0;
    private long lastTime = 0;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container
            , @Nullable Bundle savedInstanceState) {
        binding = FragmentRetrieveBinding.inflate(inflater, container, false);
        return binding.getRoot();
    }

    @SuppressLint("SetTextI18n")
    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        viewModel = new ViewModelProvider(requireActivity()).get(PacsViewModel.class);

        setupRecyclerView();

        viewModel.queryResults.observe(getViewLifecycleOwner(), records -> {
            adapter.updateData(records);
            if (records == null || records.isEmpty()) {
                binding.tvMoveStatus.setText("No query results. Please go to Query tab first.");
            } else {
                binding.tvMoveStatus.setText("Found " + records.size()
                        + " items from Query. Select to retrieve.");
            }
        });

        binding.btnDownloadSelected.setOnClickListener(v -> {
            List<PatientRecord> selected = adapter.getSelectedRecords();
            if (selected.isEmpty()) {
                Toast.makeText(getContext(), "Please select at least one item", Toast.LENGTH_SHORT).show();
                return;
            }
            executeBatchDownload(selected);
        });
    }

    private void setupRecyclerView() {
        adapter = new PatientAdapter(new ArrayList<>(), record -> {
            record.setSelected(!record.isSelected());
            adapter.notifyDataSetChanged();
        });
        binding.rvPatients.setLayoutManager(new GridLayoutManager(getContext(), 5));
        binding.rvPatients.setAdapter(adapter);
    }

    @SuppressLint("SetTextI18n")
    private void executeBatchDownload(List<PatientRecord> selected) {
        File externalFilesDir = requireContext().getExternalFilesDir(null);
        if (externalFilesDir == null) return;
        File tempDir = new File(externalFilesDir.getParentFile(), "temp");
        if (!tempDir.exists()) tempDir.mkdirs();

        binding.progressBar.setVisibility(View.VISIBLE);
        binding.progressBar.setIndeterminate(false);
        binding.progressBar.setMax(selected.size());
        binding.progressBar.setProgress(0);
        binding.btnDownloadSelected.setEnabled(false);
        binding.tvDownloadStats.setVisibility(View.VISIBLE);
        binding.tvDownloadStats.setText("Speed: 0 KB/s | Progress: 0/" + selected.size());

        new Thread(() -> {
            int count = 0;
            for (PatientRecord record : selected) {
                final int currentCount = ++count;
                final String patId = record.getId();
                
                lastBytes = 0;
                lastTime = System.currentTimeMillis();

                if (getActivity() != null) {
                    getActivity().runOnUiThread(() -> {
                        binding.tvMoveStatus.setText("Downloading (" + currentCount
                                + "/" + selected.size() + "): " + patId);
                    });
                }

                @SuppressLint("DefaultLocale") ProgressCallback callback = (sent, total) -> {
                    long currentTime = System.currentTimeMillis();
                    long timeDiff = currentTime - lastTime;
                    if (timeDiff >= 1000) {
                        long bytesDiff = sent - lastBytes;
                        double speed = (bytesDiff / 1024.0) / (timeDiff / 1000.0); // KB/s
                        lastBytes = sent;
                        lastTime = currentTime;
                        if (getActivity() != null) {
                            getActivity().runOnUiThread(() -> {
                                binding.tvDownloadStats
                                        .setText(String.format("Speed: %.2f KB/s | Progress: %d/%d",
                                        speed, currentCount, selected.size()));
                            });
                        }
                    }
                };

                boolean success = DcmtkJni.cGet(
                        viewModel.host.getValue(),
                        viewModel.port.getValue(),
                        viewModel.localAet.getValue(),
                        viewModel.remoteAet.getValue(),
                        patId,
                        tempDir.getAbsolutePath(),
                        callback
                );

                if (success) {
                    record.setDownloaded(true);
                }

                if (getActivity() != null) {
                    getActivity().runOnUiThread(() -> {
                        binding.progressBar.setProgress(currentCount);
                        adapter.notifyDataSetChanged();
                    });
                }
            }

            if (getActivity() != null) {
                getActivity().runOnUiThread(() -> {
                    binding.progressBar.setVisibility(View.GONE);
                    binding.btnDownloadSelected.setEnabled(true);
                    binding.tvMoveStatus.setText("Batch download completed.");
                    Toast.makeText(getContext(), "Batch download finished", Toast.LENGTH_SHORT).show();
                });
            }
        }).start();
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
