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
import com.example.dcmtkdemo.activity.DetailActivity;
import com.example.dcmtkdemo.databinding.FragmentUploadBinding;
import android.content.Intent;
import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.dialog.DicomUploadDialog;
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

    @SuppressLint("NotifyDataSetChanged")
    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        viewModel = new ViewModelProvider(requireActivity()).get(PacsViewModel.class);

        adapter = new DcmUploadAdapter(new ArrayList<>());
        binding.rvDcmFiles.setLayoutManager(new GridLayoutManager(getContext(), 5));
        binding.rvDcmFiles.setAdapter(adapter);

        adapter.setOnItemClickListener(record -> {
            Intent intent = new Intent(getContext(), DetailActivity.class);
            intent.putExtra(DetailActivity.EXTRA_DCM_PATH, record.getDcmPath());
            intent.putExtra(DetailActivity.EXTRA_JPG_PATH, record.getJpgPath());
            intent.putExtra(DetailActivity.EXTRA_NAME, record.getName());
            intent.putExtra(DetailActivity.EXTRA_ID, record.getId());
            intent.putExtra(DetailActivity.EXTRA_SEX, record.getSex());
            intent.putExtra(DetailActivity.EXTRA_STUDY_DATE, record.getStudyDate());
            intent.putExtra(DetailActivity.EXTRA_STUDY_DESC, record.getStudyDesc());
            startActivity(intent);
        });

        binding.btnToggleMode.setOnClickListener(v -> {
            boolean currentMode = adapter.isUploadMode();
            boolean newMode = !currentMode;
            adapter.setUploadMode(newMode);
            updateUiMode(newMode);
        });

        // Initialize UI mode
        updateUiMode(false);

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

            DicomUploadDialog dialog = DicomUploadDialog.newInstance(selectedRecords);
            dialog.setOnUploadFinishedListener((successCount, totalCount) -> {
                for (DicomImageRecord record : selectedRecords) {
                    record.setSelected(false);
                }
                adapter.notifyDataSetChanged();
                binding.tvUploadStatus.setText(String.format(java.util.Locale.getDefault()
                        , "Last Upload: %d/%d success", successCount, totalCount));
            });
            dialog.show(getParentFragmentManager(), "DicomUploadDialog");
        });
    }

    private void updateUiMode(boolean uploadMode) {
        binding.btnToggleMode.setText(uploadMode ? "Exit Upload Mode" : "Switch to Upload Mode");
        binding.btnUpload.setVisibility(uploadMode ? View.VISIBLE : View.GONE);
        binding.tvUploadStatus.setVisibility(View.VISIBLE); // Always show status
        if (!uploadMode) {
            binding.progressBar.setVisibility(View.GONE);
        }
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

                String name = "N/A", id = "N/A", sex = "N/A", date = "N/A", desc = "N/A";
                try {
                    HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(dcmPath);
                    if (info != null && !info.isEmpty()) {
                        name = safeGet(info, "(0010,0010)");
                        id = safeGet(info, "(0010,0020)");
                        sex = safeGet(info, "(0010,0040)");
                        date = safeGet(info, "(0008,0020)");
                        desc = safeGet(info, "(0008,1030)");
                    }
                } catch (Exception e) {
                    // ignore
                }
                records.add(new DicomImageRecord(name, id, sex, date, desc
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

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
