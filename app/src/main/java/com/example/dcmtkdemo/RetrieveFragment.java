package com.example.dcmtkdemo;

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

import com.example.dcmtkdemo.databinding.FragmentRetrieveBinding;

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
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
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
        File tempDir = new File(requireContext().getExternalFilesDir(null), "temp");
        if (!tempDir.exists()) tempDir.mkdirs();
        Log.e(TAG, "executeDownload tempDir: "+tempDir.getAbsolutePath());

        binding.tvMoveStatus.setText("Downloading PatientID: " + patId + "...");
        new Thread(() -> {
            // 使用新实现的 C-GET 接口
            boolean success = DcmtkJni.cGet(
                    viewModel.host.getValue(),
                    viewModel.port.getValue(),
                    viewModel.localAet.getValue(),
                    viewModel.remoteAet.getValue(),
                    patId,
                    tempDir.getAbsolutePath()
            );
            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
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

    private void refreshTempFiles() {
        File tempDir = new File(requireContext().getExternalFilesDir(null), "temp");
        if (!tempDir.exists()) tempDir.mkdirs();

        File[] files = tempDir.listFiles((dir, name) -> name.toLowerCase().endsWith(".dcm"));
        if (files == null || files.length == 0) {
            binding.tvRetrieveInfo.setText("No files in temp/ folder.");
            return;
        }

        StringBuilder sb = new StringBuilder("Files in temp/:\n");
        for (File f : files) {
            sb.append("- ").append(f.getName()).append("\n");
            try {
                HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(f.getAbsolutePath());
                if (info != null) {
                    sb.append("  PatName: ").append(info.get("(0010,0010)")).append("\n");
                    sb.append("  Modality: ").append(info.get("(0008,0060)")).append("\n");
                }
            } catch (Exception e) {
                Log.e(TAG, "Error loading info for " + f.getName(), e);
            }
        }
        binding.tvRetrieveInfo.setText(sb.toString());
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
