package com.example.dcmtkdemo.fragment;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.GridLayoutManager;

import com.example.dcmtkdemo.activity.DetailActivity;
import com.example.dcmtkdemo.adapter.DcmImageAdapter;
import com.example.dcmtkdemo.databinding.FragmentDcmShowBinding;
import com.example.dcmtkdemo.jni.DcmtkJni;
import com.example.dcmtkdemo.model.DicomImageRecord;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

/**
 * 展示 ../temp 目录下通过 C-GET 下载的 DICOM 文件。
 * 1) 列出 temp 目录下的文件，若无则提示用户去 Retrieve 下载；
 * 2) 后台调用 native dcmToJpg 将所有文件转成 JPG（输出到 temp/jpg/）；
 * 3) 解析每个文件的 Patient Name/ID/Sex 等信息，以 RecyclerView 展示，item 仅含 JPG 缩略图 + name + id + sex；
 * 4) 点击 item 跳转到 DetailActivity 显示更多详情。
 */
public class DcmShowFragment extends Fragment {

    private static final String TAG = "DcmShowFragment";

    private FragmentDcmShowBinding binding;
    private DcmImageAdapter adapter;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        binding = FragmentDcmShowBinding.inflate(inflater, container, false);
        return binding.getRoot();
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        adapter = new DcmImageAdapter(new ArrayList<>(), this::openDetail);
        binding.rvDcmImages.setLayoutManager(new GridLayoutManager(getContext(), 5));
        binding.rvDcmImages.setAdapter(adapter);

        loadAndConvert();
    }

    /** 解析 ../temp 目录 */
    @Nullable
    private File getTempDir() {
        File externalFilesDir = requireContext().getExternalFilesDir(null);
        if (externalFilesDir == null) return null;
        File tempDir = new File(externalFilesDir.getParentFile(), "temp");
        if (!tempDir.exists()) tempDir.mkdirs();
        return tempDir;
    }

    private void loadAndConvert() {
        File tempDir = getTempDir();
        if (tempDir == null) {
            showEmpty("External storage not available.");
            return;
        }

        // 仅列出普通文件，排除 .jpg（转换产物）与 jpg/ 子目录
        File[] files = tempDir.listFiles((dir, name) ->
                !name.toLowerCase().endsWith(".jpg"));
        List<File> dcmFiles = new ArrayList<>();
        if (files != null) {
            for (File f : files) {
                if (f.isFile()) dcmFiles.add(f);
            }
        }

        if (dcmFiles.isEmpty()) {
            showEmpty("No files in temp/ folder.\nPlease go to the Retrieve tab to download DICOM files first.");
            return;
        }

        showLoading("Converting " + dcmFiles.size() + " file(s) to JPG...");

        new Thread(() -> {
            // 1) 批量转换为 JPG
            int converted = DcmtkJni.dcmToJpg(tempDir.getAbsolutePath());
            Log.d(TAG, "dcmToJpg converted=" + converted);

            // 2) 解析每个文件的 DICOM 信息并构建记录
            File jpgDir = new File(tempDir, "jpg");
            List<DicomImageRecord> records = new ArrayList<>();
            for (File f : dcmFiles) {
                String dcmPath = f.getAbsolutePath();
                String jpgPath = new File(jpgDir, f.getName() + ".jpg").getAbsolutePath();

                String name = "N/A", id = "N/A", sex = "N/A";
                String studyDate = "N/A", studyDesc = "N/A";
                try {
                    HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(dcmPath);
                    if (info != null && !info.isEmpty()) {
                        name = safeGet(info, "(0010,0010)");
                        id = safeGet(info, "(0010,0020)");
                        sex = safeGet(info, "(0010,0040)");
                        studyDate = safeGet(info, "(0008,0020)");
                        studyDesc = safeGet(info, "(0008,1030)");
                    }
                } catch (Exception e) {
                    Log.e(TAG, "Error loading info for " + f.getName(), e);
                }
                records.add(new DicomImageRecord(name, id, sex, studyDate, studyDesc,
                        dcmPath, jpgPath));
            }

            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                hideLoading();
                if (records.isEmpty()) {
                    showEmpty("No valid DICOM files in temp/ folder.");
                } else {
                    binding.rvDcmImages.setVisibility(View.VISIBLE);
                    binding.tvDcmEmpty.setVisibility(View.GONE);
                    binding.tvDcmShowStatus.setText("Showing " + records.size()
                            + " image(s). Converted " + converted + " to JPG.");
                    adapter.updateData(records);
                }
            });
        }).start();
    }

    private void openDetail(DicomImageRecord record) {
        Intent intent = new Intent(getContext(), DetailActivity.class);
        intent.putExtra(DetailActivity.EXTRA_DCM_PATH, record.getDcmPath());
        intent.putExtra(DetailActivity.EXTRA_JPG_PATH, record.getJpgPath());
        intent.putExtra(DetailActivity.EXTRA_NAME, record.getName());
        intent.putExtra(DetailActivity.EXTRA_ID, record.getId());
        intent.putExtra(DetailActivity.EXTRA_SEX, record.getSex());
        intent.putExtra(DetailActivity.EXTRA_STUDY_DATE, record.getStudyDate());
        intent.putExtra(DetailActivity.EXTRA_STUDY_DESC, record.getStudyDesc());
        startActivity(intent);
    }

    private void showLoading(String status) {
        binding.progressBar.setVisibility(View.VISIBLE);
        binding.rvDcmImages.setVisibility(View.GONE);
        binding.tvDcmEmpty.setVisibility(View.GONE);
        binding.tvDcmShowStatus.setText(status);
    }

    private void hideLoading() {
        binding.progressBar.setVisibility(View.GONE);
    }

    private void showEmpty(String message) {
        binding.progressBar.setVisibility(View.GONE);
        binding.rvDcmImages.setVisibility(View.GONE);
        binding.tvDcmEmpty.setVisibility(View.VISIBLE);
        binding.tvDcmEmpty.setText(message);
        binding.tvDcmShowStatus.setText("");
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
