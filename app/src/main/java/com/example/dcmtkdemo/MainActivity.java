package com.example.dcmtkdemo;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

import com.example.dcmtkdemo.databinding.ActivityMainBinding;

import java.io.File;
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

        // 初始化字典 (写入和读取都需要)
        new Thread(() -> {
            try {
                String dictPath = FileUtil.copyAssetToInternalStorage(this, "dicom.dic");
                DcmtkJni.initDcmtk(dictPath);
            } catch (IOException e) {
                Log.e(TAG, "Failed to init dictionary", e);
            }
        }).start();

        // 初始显示
        binding.sampleText.setText(dcmtkJni.stringFromJNI());

        // 设置按钮点击事件
        binding.btnLoadDicom.setOnClickListener(v -> {
            // 默认加载生成的，如果没有则尝试加载资产里的
            File outFile = new File(getExternalFilesDir(null), "generated.dcm");
            if (outFile.exists()) {
                loadAndDisplayDicomInfo(outFile.getAbsolutePath());
            } else {
                loadAndDisplayDicomInfo(null);
            }
        });

        binding.btnWriteDicom.setOnClickListener(v -> {
            writeRawToDicom();
        });

        binding.btnConnectPacs.setOnClickListener(v -> {
            connectToPacs();
        });

        binding.btnCecho.setOnClickListener(v -> {
            executePacsCommand("C-ECHO", () -> {
                String host = binding.etHost.getText().toString().trim();
                int port = Integer.parseInt(binding.etPort.getText().toString().trim());
                String localAet = binding.etLocalAet.getText().toString().trim();
                String remoteAet = binding.etRemoteAet.getText().toString().trim();
                return DcmtkJni.cEcho(host, port, localAet, remoteAet) ? "Success" : "Failed";
            });
        });

        binding.btnCstore.setOnClickListener(v -> {
            executePacsCommand("C-STORE", () -> {
                String host = binding.etHost.getText().toString().trim();
                int port = Integer.parseInt(binding.etPort.getText().toString().trim());
                String localAet = binding.etLocalAet.getText().toString().trim();
                String remoteAet = binding.etRemoteAet.getText().toString().trim();
//                File outFile = new File(getExternalFilesDir(null), "generated.dcm");
                String outFileString = FileUtil.copyAssetToInternalStorage(this,
                        "CR2026060810120220260609162248FT17.dcm");
                File outFile = new File(outFileString);
                if (!outFile.exists())
                    return "Error: generated.dcm not found. Please click 'Write DICOM' first.";
                return DcmtkJni.cStore(host, port, localAet, remoteAet, outFile.getAbsolutePath()) ? "Success" : "Failed";
            });
        });

        binding.btnCfind.setOnClickListener(v -> {
            executePacsCommand("C-FIND", () -> {
                String host = binding.etHost.getText().toString().trim();
                int port = Integer.parseInt(binding.etPort.getText().toString().trim());
                String localAet = binding.etLocalAet.getText().toString().trim();
                String remoteAet = binding.etRemoteAet.getText().toString().trim();
                String patName = binding.etQueryPatName.getText().toString().trim();
                String[] results = DcmtkJni.cFind(host, port, localAet, remoteAet, patName);
                if (results == null || results.length == 0) return "No results found.";
                StringBuilder sb = new StringBuilder("Found ").append(results.length).append(" records:\n");
                for (String res : results) sb.append("- ").append(res).append("\n");
                return sb.toString();
            });
        });

        binding.btnCmove.setOnClickListener(v -> {
            executePacsCommand("C-MOVE", () -> {
                String host = binding.etHost.getText().toString().trim();
                int port = Integer.parseInt(binding.etPort.getText().toString().trim());
                String localAet = binding.etLocalAet.getText().toString().trim();
                String remoteAet = binding.etRemoteAet.getText().toString().trim();
                String patId = binding.etMovePatId.getText().toString().trim();
                String destAet = binding.etDestAet.getText().toString().trim();
                return DcmtkJni.cMove(host, port, localAet, remoteAet, patId, destAet) ? "Success" : "Failed";
            });
        });
    }

    private interface PacsCommand {
        String run() throws IOException;
    }

    private void executePacsCommand(String name, PacsCommand command) {
        binding.sampleText.setText("Executing " + name + "...");
        new Thread(() -> {
            try {
                String result = command.run();
                runOnUiThread(() -> binding.sampleText.setText(name + " Result:\n" + result));
            } catch (Exception e) {
                runOnUiThread(() -> binding.sampleText.setText(name + " Error:\n" + e.getMessage()));
            }
        }).start();
    }

    private void connectToPacs() {
        String host = binding.etHost.getText().toString().trim();
        String portStr = binding.etPort.getText().toString().trim();
        String localAet = binding.etLocalAet.getText().toString().trim();
        String remoteAet = binding.etRemoteAet.getText().toString().trim();

        if (host.isEmpty() || portStr.isEmpty() || localAet.isEmpty() || remoteAet.isEmpty()) {
            binding.sampleText.setText("Please fill in all PACS connection fields.");
            return;
        }

        int port = Integer.parseInt(portStr);
        binding.sampleText.setText("Connecting to PACS: " + host + ":" + port + "...");

        new Thread(() -> {
            boolean success = DcmtkJni.connectPACS(host, port, localAet, remoteAet);
            runOnUiThread(() -> {
                if (success) {
                    binding.sampleText.setText("PACS Connection/Association successful!");
                } else {
                    binding.sampleText.setText("PACS Connection/Association failed.\nCheck Logcat (DcmtkJni) for details.");
                }
            });
        }).start();
    }

    private void writeRawToDicom() {
        TextView tv = binding.sampleText;
        tv.setText("Writing DICOM...");

        new Thread(() -> {
            try {
                // 1. 准备输入输出路径
                String rawPath = FileUtil.copyAssetToInternalStorage(this, "Data610.bin");
                File outFile = new File(getExternalFilesDir(null), "generated.dcm");
                String dcmPath = outFile.getAbsolutePath();

                // 2. 调用 JNI 写入 (根据 Data610.bin 大小 3,870,000 字节推测)
                // 1935 * 1000 * 2 = 3,870,000
                boolean success = DcmtkJni.writeDicomFile(rawPath, dcmPath, 1935, 1000);

                runOnUiThread(() -> {
                    if (success) {
                        tv.setText("Successfully wrote DICOM to:\n" + dcmPath +
                                "\n\nYou can now click 'Load DICOM' to view its info.");
                        // 自动加载显示刚才生成的
                        loadAndDisplayDicomInfo(dcmPath);
                    } else {
                        tv.setText("Failed to write DICOM file.");
                    }
                });
            } catch (IOException e) {
                Log.e(TAG, "Error writing DICOM", e);
                runOnUiThread(() -> tv.setText("Error: " + e.getMessage()));
            }
        }).start();
    }

    private void loadAndDisplayDicomInfo(String specificPath) {
        TextView tv = binding.sampleText;
        tv.setText("Processing...");

        new Thread(() -> {
            try {
                // 1. 获取 DICOM 文件路径
                String dcmPath;
                if (specificPath != null) {
                    dcmPath = specificPath;
                } else {
                    dcmPath = FileUtil.copyAssetToInternalStorage(this, "CR2026060810120220260609162248FT17.dcm");
                }

                // 2. 加载 DICOM 文件信息
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
                        {"(0028,0100)", "Bits Allocated"},
                        {"(0028,1050)", "Window Center (Level)"},
                        {"(0028,1051)", "Window Width"},
                        {"(0028,1052)", "Rescale Intercept"},
                        {"(0028,1053)", "Rescale Slope"}
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
