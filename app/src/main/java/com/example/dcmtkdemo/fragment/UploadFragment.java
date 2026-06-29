package com.example.dcmtkdemo.fragment;

import android.annotation.SuppressLint;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;
import androidx.recyclerview.widget.GridLayoutManager;

import com.example.dcmtkdemo.adapter.DcmUploadAdapter;
import com.example.dcmtkdemo.callback.ProgressCallback;
import com.example.dcmtkdemo.databinding.FragmentUploadBinding;
import com.example.dcmtkdemo.jni.DcmtkJni;
import com.example.dcmtkdemo.model.DicomImageRecord;
import com.example.dcmtkdemo.viewmodel.PacsViewModel;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

public class UploadFragment extends Fragment {

    private FragmentUploadBinding binding;
    private PacsViewModel viewModel;
    private DcmUploadAdapter adapter;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container
            , @Nullable Bundle savedInstanceState) {
        binding = FragmentUploadBinding.inflate(inflater, container, false);
        return binding.getRoot();
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        viewModel = new ViewModelProvider(requireActivity()).get(PacsViewModel.class);

        adapter = new DcmUploadAdapter(new ArrayList<>());
        binding.rvDcmFiles.setLayoutManager(new GridLayoutManager(getContext(), 5));
        binding.rvDcmFiles.setAdapter(adapter);

        refreshFileList();

        // 监听资产拷贝完成的信号，一旦完成就刷新列表
        viewModel.assetsReady.observe(getViewLifecycleOwner(), ready -> {
            if (ready) {
                refreshFileList();
            }
        });

        binding.btnUpload.setOnClickListener(v -> {
            List<DicomImageRecord> selectedRecords = adapter.getSelectedRecords();
            if (selectedRecords.isEmpty()) {
                Toast.makeText(getContext(), "No files selected", Toast.LENGTH_SHORT).show();
                return;
            }

            uploadDicomList(selectedRecords);
        });
    }

    @Override
    public void onResume() {
        super.onResume();
        refreshFileList();
    }

    @SuppressLint("SetTextI18n")
    private void refreshFileList() {
        File dir = requireContext().getExternalFilesDir(null);
        if (dir == null) return;

        // 仅列出普通文件，排除 .jpg 与 jpg/ 子目录
        File[] files = dir.listFiles((d, name) ->
                name.toLowerCase().endsWith(".dcm") && !name.toLowerCase().endsWith(".jpg"));

        if (files == null || files.length == 0) {
            binding.btnUpload.setEnabled(false);
            binding.tvUploadStatus.setText("No .dcm files found in " + dir.getAbsolutePath());
            adapter.updateData(new ArrayList<>());
            return;
        }

        binding.btnUpload.setEnabled(true);
        binding.tvUploadStatus.setText("Loading and converting " + files.length + " file(s)...");

        new Thread(() -> {
            // 1) 批量转换为 JPG
            DcmtkJni.dcmToJpg(dir.getAbsolutePath());

            // 2) 解析每个文件的 DICOM 信息
            File jpgDir = new File(dir, "jpg");
            List<DicomImageRecord> records = new ArrayList<>();
            for (File f : files) {
                String dcmPath = f.getAbsolutePath();
                String jpgPath = new File(jpgDir, f.getName() + ".jpg").getAbsolutePath();

                String name = "N/A", id = "N/A", sex = "N/A";
                try {
                    HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(dcmPath);
                    if (info != null && !info.isEmpty()) {
                        name = safeGet(info, "(0010,0010)");
                        id = safeGet(info, "(0010,0020)");
                        sex = safeGet(info, "(0010,0040)");
                    }
                } catch (Exception e) {
                    // ignore
                }
                records.add(new DicomImageRecord(name, id, sex, "N/A", "N/A"
                        , dcmPath, jpgPath));
            }

            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                adapter.updateData(records);
                binding.tvUploadStatus.setText("Found " + records.size() + " DICOM file(s).");
            });
        }).start();
    }

    private static String safeGet(HashMap<String, String> map, String key) {
        String val = map.get(key);
        return (val != null && !val.isEmpty()) ? val : "N/A";
    }

    @SuppressLint({"SetTextI18n", "DefaultLocale"})
    private void uploadDicomList(List<DicomImageRecord> records) {
        binding.progressBar.setMax(100);
        binding.progressBar.setProgress(0);
        binding.progressBar.setVisibility(View.VISIBLE);
        binding.btnUpload.setEnabled(false);

        new Thread(() -> {
            int totalFiles = records.size();
            int successCount = 0;

            for (int i = 0; i < totalFiles; i++) {
                DicomImageRecord record = records.get(i);
                final int currentIndex = i + 1;
                final String path = record.getDcmPath();

                // 进度回调更新当前文件的进度
                @SuppressLint("DefaultLocale") ProgressCallback callback = (sent, total) -> {
                    if (getActivity() == null) return;
                    getActivity().runOnUiThread(() -> {
                        int percent = total > 0 ? (int) (sent * 100 / total) : 0;
                        if (percent > 100) percent = 100;
                        binding.progressBar.setProgress(percent);
                        binding.tvUploadStatus.setText(
                                String.format("Uploading (%d/%d): %s\n%s / %s (%d%%)",
                                        currentIndex, totalFiles, new File(path).getName(),
                                        formatBytes(sent), formatBytes(total), percent)
                        );
                    });
                };

                boolean success = DcmtkJni.cStore(
                        viewModel.host.getValue(),
                        viewModel.port.getValue(),
                        viewModel.localAet.getValue(),
                        viewModel.remoteAet.getValue(),
                        path,
                        callback
                );

                if (success) successCount++;
            }

            final int finalSuccessCount = successCount;
            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                binding.progressBar.setVisibility(View.GONE);
                binding.btnUpload.setEnabled(true);
                binding.tvUploadStatus.setText(String.format("Upload Finished. Success: %d, Failed: %d",
                        finalSuccessCount, totalFiles - finalSuccessCount));
                Toast.makeText(getContext(), "Upload completed: " + finalSuccessCount + " success",
                        Toast.LENGTH_SHORT).show();
            });
        }).start();
    }

    @SuppressLint("DefaultLocale")
    private static String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format("%.1f KB", bytes / 1024.0);
        return String.format("%.2f MB", bytes / (1024.0 * 1024.0));
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
