package com.example.dcmtkdemo;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

import com.example.dcmtkdemo.databinding.ActivityMainBinding;

import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

public class MainActivity extends AppCompatActivity {
    private static final String TAG = "MainActivity";

    private ActivityMainBinding binding;
    private DcmtkJni dcmtkJni;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        dcmtkJni = new DcmtkJni();
        
        // 初始显示
        binding.sampleText.setText(dcmtkJni.stringFromJNI());

        // 设置按钮点击事件
        binding.btnLoadDicom.setOnClickListener(v -> {
            loadAndDisplayDicomInfo();
        });
    }

    private void loadAndDisplayDicomInfo() {
        TextView tv = binding.sampleText;
        tv.setText("Processing...");

        new Thread(() -> {
            try {
                // 1. 使用工具类拷贝必要文件
                String dictPath = FileUtil.copyAssetToInternalStorage(this, "dicom.dic");
                String dcmPath = FileUtil.copyAssetToInternalStorage(this, "CR2026060810120220260609162248FT17.dcm");

                // 2. 初始化字典
                DcmtkJni.initDcmtk(dictPath);

                // 3. 加载 DICOM 文件信息
                HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(dcmPath);

                // 4. 提取重要信息并展示
                StringBuilder sb = new StringBuilder();
                sb.append("--- Important DICOM Info ---\n\n");
                
                String[][] importantTags = {
                    {"(0010,0010)", "Patient Name"},
                    {"(0010,0020)", "Patient ID"},
                    {"(0010,0040)", "Patient Sex"},
                    {"(0010,0030)", "Patient BirthDate"},
                    {"(0008,0020)", "Study Date"},
                    {"(0008,0060)", "Modality"},
                    {"(0008,0070)", "Manufacturer"},
                    {"(0008,1030)", "Study Description"},
                    {"(0028,0010)", "Rows"},
                    {"(0028,0011)", "Columns"},
                    {"(0028,0100)", "Bits Allocated"}
                };

                for (String[] tagInfo : importantTags) {
                    String tag = tagInfo[0];
                    String label = tagInfo[1];
                    String value = info.get(tag);
                    if (value != null) {
                        sb.append(label).append(": ").append(value).append("\n");
                    }
                }

                if (sb.length() < 40) {
                    sb.append("No important tags found.\n\n--- All Tags ---\n");
                    for (Map.Entry<String, String> entry : info.entrySet()) {
                        sb.append(entry.getKey()).append(": ").append(entry.getValue()).append("\n");
                    }
                }

                String resultText = sb.toString();
                runOnUiThread(() -> tv.setText(resultText));

            } catch (IOException e) {
                Log.e(TAG, "Error handling files", e);
                runOnUiThread(() -> tv.setText("Error: " + e.getMessage()));
            }
        }).start();
    }
}
