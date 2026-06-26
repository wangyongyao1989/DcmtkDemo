package com.example.dcmtkdemo.fragment;

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
import androidx.recyclerview.widget.LinearLayoutManager;

import com.example.dcmtkdemo.adapter.PatientAdapter;
import com.example.dcmtkdemo.callback.ProgressCallback;
import com.example.dcmtkdemo.databinding.FragmentRetrieveBinding;
import com.example.dcmtkdemo.jni.DcmtkJni;
import com.example.dcmtkdemo.viewmodel.PacsViewModel;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;

public class RetrieveFragment extends Fragment {

    private static final String TAG = "RetrieveFragment";
    private FragmentRetrieveBinding binding;
    private PacsViewModel viewModel;
    private PatientAdapter adapter;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container
            , @Nullable Bundle savedInstanceState) {
        binding = FragmentRetrieveBinding.inflate(inflater, container, false);
        return binding.getRoot();
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        viewModel = new ViewModelProvider(requireActivity()).get(PacsViewModel.class);

        setupRecyclerView();
        refreshTempFiles();

        viewModel.queryResults.observe(getViewLifecycleOwner(), records -> {
            adapter.updateData(records);
            if (records == null || records.isEmpty()) {
                binding.tvMoveStatus.setText("No query results. Please go to Query tab first.");
            } else {
                binding.tvMoveStatus.setText("Found " + records.size()
                        + " items from Query. Click item to retrieve.");
            }
        });
    }

    private void setupRecyclerView() {
        adapter = new PatientAdapter(new ArrayList<>(), record -> {
            executeDownload(record.getId());
        });
        binding.rvPatients.setLayoutManager(new LinearLayoutManager(getContext()));
        binding.rvPatients.setAdapter(adapter);
    }

    /**
     * 执行下载逻辑。
     * 原因分析：C-MOVE 失败通常是因为客户端缺少 Storage SCP (接收服务端推送的服务)。
     * 解决方案：改用 C-GET。C-GET 允许在同一个网络连接中直接下载文件，无需客户端启动服务。
     */
    private void executeDownload(String patId) {
        // 将 C-GET 后的 dcm 文件下载到 ../temp 目录下 (即 files 目录的平级 temp 目录)
        File externalFilesDir = requireContext().getExternalFilesDir(null);
        if (externalFilesDir == null) {
            Toast.makeText(getContext(), "External storage not available", Toast.LENGTH_SHORT).show();
            return;
        }
        File tempDir = new File(externalFilesDir.getParentFile(), "temp");
        if (!tempDir.exists()) tempDir.mkdirs();
        Log.d(TAG, "executeDownload tempDir: " + tempDir.getAbsolutePath());

        binding.tvMoveStatus.setText("Downloading PatientID: " + patId + "\nReceived: 0 B");
        binding.progressBar.setVisibility(View.VISIBLE);
        binding.rvPatients.setEnabled(false); // Disable interaction during download

        new Thread(() -> {
            ProgressCallback callback = (sent, total) -> {
                if (getActivity() == null) return;
                getActivity().runOnUiThread(() -> {
                    binding.tvMoveStatus.setText("Downloading PatientID: " + patId
                            + "\nReceived: " + formatBytes(sent));
                });
            };
            // 使用新实现的 C-GET 接口
            boolean success = DcmtkJni.cGet(
                    viewModel.host.getValue(),
                    viewModel.port.getValue(),
                    viewModel.localAet.getValue(),
                    viewModel.remoteAet.getValue(),
                    patId,
                    tempDir.getAbsolutePath(),
                    callback
            );
            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                binding.progressBar.setVisibility(View.GONE);
                binding.rvPatients.setEnabled(true);
                binding.tvMoveStatus.setText("Download Result: " + (success ? "Success" : "Failed"));
                if (success) {
                    Toast.makeText(getContext(), "Download successful", Toast.LENGTH_SHORT).show();
                    refreshTempFiles();
                } else {
                    Toast.makeText(getContext(), "Download failed. Check if PACS supports C-GET."
                            , Toast.LENGTH_LONG).show();
                }
            });
        }).start();
    }

    private static String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format("%.1f KB", bytes / 1024.0);
        return String.format("%.2f MB", bytes / (1024.0 * 1024.0));
    }

    private void refreshTempFiles() {
        File externalFilesDir = requireContext().getExternalFilesDir(null);
        if (externalFilesDir == null) return;
        File tempDir = new File(externalFilesDir.getParentFile(), "temp");
        if (!tempDir.exists()) tempDir.mkdirs();

        // DCMTK 的 DcmSCU 在 C-GET 存储到磁盘时，文件名格式为 "MODALITY.SOPInstanceUID"
        // (例如 "CT.1.2.3.4.5")，并不带 .dcm 扩展名。因此这里列出 temp 目录下的所有文件，
        // 而不是仅过滤 .dcm，以确保下载的文件都能被正确显示。
        File[] files = tempDir.listFiles(File::isFile);
        if (files == null || files.length == 0) {
            binding.tvRetrieveInfo.setText("No files in temp/ folder.");
            return;
        }

        StringBuilder sb = new StringBuilder();
        sb.append("Files in temp/ (").append(files.length).append("):\n");
        for (File f : files) {
            sb.append("========================================\n");
            sb.append("File: ").append(f.getName()).append("\n");
            sb.append("Size: ").append(f.length()).append(" bytes\n");
            try {
                HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(f.getAbsolutePath());
                if (info != null && !info.isEmpty()) {
                    // 显示 DICOM 重要信息
                    sb.append("------------ Patient Info ------------\n");
                    sb.append("  Patient Name  : ").append(safeGet(info, "(0010,0010)")).append("\n");
                    sb.append("  Patient ID    : ").append(safeGet(info, "(0010,0020)")).append("\n");
                    sb.append("  Patient Sex   : ").append(safeGet(info, "(0010,0040)")).append("\n");
                    sb.append("  Birth Date    : ").append(safeGet(info, "(0010,0030)")).append("\n");

                    sb.append("------------ Study Info --------------\n");
                    sb.append("  Study Date    : ").append(safeGet(info, "(0008,0020)")).append("\n");
                    sb.append("  Study Time    : ").append(safeGet(info, "(0008,0030)")).append("\n");
                    sb.append("  Study Desc    : ").append(safeGet(info, "(0008,1030)")).append("\n");
                    sb.append("  Accession No  : ").append(safeGet(info, "(0008,0050)")).append("\n");

                    sb.append("------------ Series Info -------------\n");
                    sb.append("  Modality      : ").append(safeGet(info, "(0008,0060)")).append("\n");
                    sb.append("  Series Desc   : ").append(safeGet(info, "(0008,103E)")).append("\n");
                    sb.append("  Series Number : ").append(safeGet(info, "(0020,0011)")).append("\n");

                    sb.append("------------ Image Info --------------\n");
                    sb.append("  Rows          : ").append(safeGet(info, "(0028,0010)")).append("\n");
                    sb.append("  Columns       : ").append(safeGet(info, "(0028,0011)")).append("\n");
                    sb.append("  Bits Allocated: ").append(safeGet(info, "(0028,0100)")).append("\n");
                    sb.append("  Bits Stored   : ").append(safeGet(info, "(0028,0101)")).append("\n");
                    sb.append("  Pixel Spacing : ").append(safeGet(info, "(0028,0030)")).append("\n");

                    sb.append("------------ UID Info ----------------\n");
                    sb.append("  SOP Class UID : ").append(safeGet(info, "(0008,0016)")).append("\n");
                    sb.append("  SOP Inst UID  : ").append(safeGet(info, "(0008,0018)")).append("\n");
                    sb.append("  Study Inst UID: ").append(safeGet(info, "(0020,000D)")).append("\n");
                    sb.append("  Series Inst UID:").append(safeGet(info, "(0020,000E)")).append("\n");
                } else {
                    sb.append("  [Not a valid DICOM file or empty]\n");
                }
            } catch (Exception e) {
                Log.e(TAG, "Error loading info for " + f.getName(), e);
                sb.append("  [Error reading file: ").append(e.getMessage()).append("]\n");
            }
        }
        binding.tvRetrieveInfo.setText(sb.toString());
    }

    /** 安全地从 HashMap 中获取值，若为 null 返回 "N/A" */
    private static String safeGet(HashMap<String, String> map, String key) {
        String val = map.get(key);
        return (val != null && !val.isEmpty()) ? val : "N/A";
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
