package com.example.dcmtkdemo.activity;

import android.os.Bundle;
import android.util.Log;
import android.view.MenuItem;
import android.view.View;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;

import com.bumptech.glide.Glide;
import com.example.dcmtkdemo.R;
import com.example.dcmtkdemo.databinding.ActivityDetailBinding;
import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.utils.AppThreadPool;

import java.io.File;

/**
 * 详情页：展示某个 DICOM 文件转换后的 JPG 大图，以及更完整的病人/检查信息。
 * 通过 Intent extras 接收 DcmShowFragment 传递的路径与字段；
 * 若 JPG 缩略图不存在则后台触发批量转换以保证可用。
 */
public class DetailActivity extends AppCompatActivity {

    public static final String EXTRA_DCM_PATH = "extra_dcm_path";
    public static final String EXTRA_JPG_PATH = "extra_jpg_path";
    public static final String EXTRA_NAME = "extra_name";
    public static final String EXTRA_ID = "extra_id";
    public static final String EXTRA_SEX = "extra_sex";
    public static final String EXTRA_STUDY_DATE = "extra_study_date";
    public static final String EXTRA_STUDY_DESC = "extra_study_desc";

    private static final String TAG = "DetailActivity";

    private ActivityDetailBinding binding;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        binding = ActivityDetailBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
            getSupportActionBar().setTitle("DICOM Detail");
        }

        // 直接使用 extras 填充文本字段（已在列表页解析过）
        binding.tvDetailName.setText("Patient Name: " + safeExtra(EXTRA_NAME));
        binding.tvDetailId.setText("Patient ID: " + safeExtra(EXTRA_ID));
        binding.tvDetailSex.setText("Patient Sex: " + safeExtra(EXTRA_SEX));
        binding.tvDetailStudyDate.setText("Study Date: " + safeExtra(EXTRA_STUDY_DATE));
        binding.tvDetailStudyDesc.setText("Study Desc: " + safeExtra(EXTRA_STUDY_DESC));

        loadImage();
    }

    /** 加载转换后的 JPG；若不存在则在后台重新触发转换。 */
    private void loadImage() {
        String jpgPath = getIntent().getStringExtra(EXTRA_JPG_PATH);
        String dcmPath = getIntent().getStringExtra(EXTRA_DCM_PATH);

        if (jpgPath != null && new File(jpgPath).exists()) {
            Glide.with(this).load(new File(jpgPath)).into(binding.ivDetailImage);
            return;
        }

        // JPG 不存在，后台重新转换（转换是耗时操作，显示 loading）
        binding.progressBar.setVisibility(View.VISIBLE);
        AppThreadPool.execute(() -> {
            if (dcmPath != null) {
                File dcmFile = new File(dcmPath);
                File tempDir = dcmFile.getParentFile();
                if (tempDir != null) {
                    DcmtkJni.dcmToJpg(tempDir.getAbsolutePath());
                }
            }
            String finalJpg = jpgPath;
            if (finalJpg == null && dcmPath != null) {
                File dcmFile = new File(dcmPath);
                File tempDir = dcmFile.getParentFile();
                if (tempDir != null) {
                    finalJpg = new File(new File(tempDir, "jpg"),
                            dcmFile.getName() + ".jpg").getAbsolutePath();
                }
            }
            final String pathToLoad = finalJpg;
            runOnUiThread(() -> {
                if (isFinishing() || binding == null) return;
                binding.progressBar.setVisibility(View.GONE);
                if (pathToLoad != null && new File(pathToLoad).exists()) {
                    Glide.with(this).load(new File(pathToLoad)).into(binding.ivDetailImage);
                } else {
                    Log.e(TAG, "loadImage: JPG not available at " + pathToLoad);
                }
            });
        });
    }

    private String safeExtra(String key) {
        String v = getIntent().getStringExtra(key);
        return (v != null && !v.isEmpty()) ? v : "N/A";
    }

    @Override
    public boolean onOptionsItemSelected(@Nullable MenuItem item) {
        if (item != null && item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        binding = null;
    }
}
