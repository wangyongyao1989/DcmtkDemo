package com.example.dcmtkdemo.view;

import android.annotation.SuppressLint;
import android.content.Context;
import android.util.AttributeSet;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.Toast;

import androidx.annotation.Nullable;

import com.example.dcmtk.callback.ProgressCallback;
import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.databinding.ViewDicomUploadBinding;
import com.example.dcmtkdemo.model.DicomImageRecord;

import java.io.File;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

public class DicomUploadView extends LinearLayout {

    private ViewDicomUploadBinding binding;
    private List<DicomImageRecord> uploadRecords;
    private final AtomicBoolean isCancelled = new AtomicBoolean(false);
    private boolean isUploading = false;
    private OnUploadEventListener listener;

    public interface OnUploadEventListener {
        void onFinished(int successCount, int totalCount);
        void onCancel();
    }

    public DicomUploadView(Context context) {
        this(context, null);
    }

    public DicomUploadView(Context context, @Nullable AttributeSet attrs) {
        this(context, attrs, 0);
    }

    public DicomUploadView(Context context, @Nullable AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        setOrientation(VERTICAL);
        binding = ViewDicomUploadBinding.inflate(LayoutInflater.from(context), this, true);
        init();
    }

    private void init() {
        binding.btnAction.setOnClickListener(v -> {
            if (!isUploading) {
                startUploadProcess();
            }
        });

        binding.btnCancel.setOnClickListener(v -> {
            if (isUploading) {
                isCancelled.set(true);
                binding.tvStatus.setText("Cancelling...");
                binding.btnCancel.setEnabled(false);
            } else if (listener != null) {
                listener.onCancel();
            }
        });
    }

    public void setUploadRecords(List<DicomImageRecord> records) {
        this.uploadRecords = records;
    }

    public void setOnUploadEventListener(OnUploadEventListener listener) {
        this.listener = listener;
    }

    public void setConnectionInfo(String host, int port, String local, String remote) {
        binding.connectionView.setConnectionInfo(host, port, local, remote);
    }

    private void startUploadProcess() {
        if (uploadRecords == null || uploadRecords.isEmpty()) {
            Toast.makeText(getContext(), "No records to upload", Toast.LENGTH_SHORT).show();
            return;
        }

        String host = binding.connectionView.getHost();
        int port = binding.connectionView.getPort();
        String local = binding.connectionView.getLocalAet();
        String remote = binding.connectionView.getRemoteAet();

        if (host.isEmpty() || port == 0 || local.isEmpty() || remote.isEmpty()) {
            Toast.makeText(getContext(), "Please fill all fields", Toast.LENGTH_SHORT).show();
            return;
        }

        isUploading = true;
        binding.connectionView.setVisibility(View.GONE);
        binding.layoutUploadProgress.setVisibility(View.VISIBLE);
        binding.btnAction.setVisibility(View.GONE);
        binding.tvTitle.setText("Uploading DICOM Files");

        new Thread(() -> {
            boolean connected = DcmtkJni.connectPACS(host, port, local, remote);
            if (!connected) {
                showError("Connection failed");
                return;
            }

            boolean echoOk = DcmtkJni.cEcho(host, port, local, remote);
            if (!echoOk) {
                showError("C-ECHO failed");
                return;
            }

            int totalFiles = uploadRecords.size();
            int successCount = 0;

            for (int i = 0; i < totalFiles; i++) {
                if (isCancelled.get()) break;

                DicomImageRecord record = uploadRecords.get(i);
                final int currentIndex = i + 1;
                final String path = record.getDcmPath();

                ProgressCallback callback = (sent, total) -> {
                    if (isCancelled.get()) return;
                    post(() -> {
                        int percent = total > 0 ? (int) (sent * 100 / total) : 0;
                        if (percent > 100) percent = 100;
                        binding.progressBar.setProgress(percent);
                        binding.tvStatus.setText(
                                String.format(java.util.Locale.getDefault(), "Uploading (%d/%d): %s\n%s / %s (%d%%)",
                                        currentIndex, totalFiles, new File(path).getName(),
                                        formatBytes(sent), formatBytes(total), percent)
                        );
                    });
                };

                boolean success = DcmtkJni.cStore(host, port, local, remote, path, callback);
                if (success) successCount++;
            }

            final int finalSuccessCount = successCount;
            post(() -> {
                if (listener != null) {
                    listener.onFinished(finalSuccessCount, totalFiles);
                }
            });
        }).start();
    }

    private void showError(String message) {
        post(() -> {
            Toast.makeText(getContext(), message, Toast.LENGTH_LONG).show();
            isUploading = false;
            binding.connectionView.setVisibility(View.VISIBLE);
            binding.layoutUploadProgress.setVisibility(View.GONE);
            binding.btnAction.setVisibility(View.VISIBLE);
            binding.tvTitle.setText("DICOM Upload");
        });
    }

    @SuppressLint("DefaultLocale")
    private static String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format("%.1f KB", bytes / 1024.0);
        return String.format("%.2f MB", bytes / (1024.0 * 1024.0));
    }
}
