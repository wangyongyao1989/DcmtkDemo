package com.example.dcmtkdemo;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import com.example.dcmtkdemo.databinding.FragmentUploadBinding;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class UploadFragment extends Fragment {

    private FragmentUploadBinding binding;
    private PacsViewModel viewModel;

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

        refreshFileList();

        // 监听资产拷贝完成的信号，一旦完成就刷新列表
        viewModel.assetsReady.observe(getViewLifecycleOwner(), ready -> {
            if (ready) {
                refreshFileList();
            }
        });

        binding.btnUpload.setOnClickListener(v -> {
            String selectedFile = (String) binding.spinnerDcmFiles.getSelectedItem();
            if (selectedFile == null) {
                Toast.makeText(getContext(), "No file selected", Toast.LENGTH_SHORT).show();
                return;
            }

            File file = new File(requireContext().getExternalFilesDir(null), selectedFile);
            uploadDicom(file.getAbsolutePath());
        });
    }

    @Override
    public void onResume() {
        super.onResume();
        refreshFileList();
    }

    private void refreshFileList() {
        File dir = requireContext().getExternalFilesDir(null);
        File[] files = dir.listFiles((d, name) -> name.toLowerCase().endsWith(".dcm"));
        List<String> fileNames = new ArrayList<>();
        if (files != null) {
            for (File f : files) fileNames.add(f.getName());
        }

        if (fileNames.isEmpty()) {
            fileNames.add("No .dcm files found");
            binding.btnUpload.setEnabled(false);
        } else {
            binding.btnUpload.setEnabled(true);
        }

        ArrayAdapter<String> adapter = new ArrayAdapter<>(requireContext()
                , android.R.layout.simple_spinner_item, fileNames);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        binding.spinnerDcmFiles.setAdapter(adapter);
    }

    private void uploadDicom(String path) {
        File file = new File(path);
        long totalBytes = file.length();
        binding.tvUploadStatus.setText("Uploading " + path + "\n" + formatBytes(0) + " / "
                + formatBytes(totalBytes) + " (0%)");
        binding.progressBar.setMax(100);
        binding.progressBar.setProgress(0);
        binding.progressBar.setVisibility(View.VISIBLE);
        binding.btnUpload.setEnabled(false);
        new Thread(() -> {
            ProgressCallback callback = (sent, total) -> {
                if (getActivity() == null) return;
                getActivity().runOnUiThread(() -> {
                    int percent = total > 0 ? (int) (sent * 100 / total) : 0;
                    if (percent > 100) percent = 100;
                    binding.progressBar.setProgress(percent);
                    binding.tvUploadStatus.setText("Uploading " + path + "\n"
                            + formatBytes(sent) + " / " + formatBytes(total)
                            + " (" + percent + "%)");
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
            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                binding.progressBar.setVisibility(View.GONE);
                binding.btnUpload.setEnabled(true);
                binding.tvUploadStatus.setText("C-STORE Result: " + (success ? "Success" : "Failed"));
                if (success) {
                    Toast.makeText(getContext(), "Upload successful", Toast.LENGTH_SHORT).show();
                } else {
                    Toast.makeText(getContext(), "Upload failed", Toast.LENGTH_SHORT).show();
                }
            });
        }).start();
    }

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
