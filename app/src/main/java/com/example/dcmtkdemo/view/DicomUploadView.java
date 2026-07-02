package com.example.dcmtkdemo.view;

import android.annotation.SuppressLint;
import android.content.Context;
import android.util.AttributeSet;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.LinearLayout;

import androidx.annotation.Nullable;

import com.example.dcmtk.PacsManager;
import com.example.dcmtk.callback.MultiProgressCallback;
import com.example.dcmtkdemo.databinding.ViewDicomUploadBinding;
import com.example.dcmtkdemo.model.DicomImageRecord;
import com.example.dcmtkdemo.utils.AppThreadPool;

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

    public void startUploadProcess() {
        if (uploadRecords == null || uploadRecords.isEmpty()) {
            showError("No records to upload");
            return;
        }

        String host = binding.connectionView.getHost();
        int port = binding.connectionView.getPort();
        String local = binding.connectionView.getLocalAet();
        String remote = binding.connectionView.getRemoteAet();

        if (host.isEmpty() || port == 0 || local.isEmpty() || remote.isEmpty()) {
            showError("Please fill all fields");
            return;
        }

        binding.tvError.setVisibility(GONE);
        isUploading = true;
        binding.connectionView.setVisibility(View.GONE);
        binding.layoutUploadProgress.setVisibility(View.VISIBLE);
        binding.btnAction.setVisibility(View.GONE);
        binding.tvTitle.setText("Uploading DICOM Files");

        AppThreadPool.execute(() -> {
            // Optimization: Use safeCEchoSync (from PacsManager) which includes
            // network read timeout handling and automatic retry logic.
            boolean echoOk = PacsManager.safeCEchoSync(host, port, local, remote, 1);
            if (!echoOk) {
                showError("PACS Verification Failed (Check Network/AETs)");
                return;
            }

            int totalFiles = uploadRecords.size();
            String[] paths = new String[totalFiles];
            for (int i = 0; i < totalFiles; i++) {
                paths[i] = uploadRecords.get(i).getDcmPath();
            }

            // Optimization: Use safeCStoreMultiSync which handles connection reuse
            // and provides automatic retries for unstable mobile networks.
            int successCount = PacsManager.safeCStoreMultiSync(host, port, local, remote
                    , paths, (index, sent, total) -> {
                if (isCancelled.get()) return false;
                post(() -> {
                    int percent = total > 0 ? (int) (sent * 100 / total) : 0;
                    if (percent > 100) percent = 100;
                    binding.progressBar.setProgress(percent);
                    binding.tvStatus.setText(
                            String.format(java.util.Locale.getDefault(), "Uploading (%d/%d): %s\n%s / %s (%d%%)",
                                    index + 1, totalFiles, new File(paths[index]).getName(),
                                    formatBytes(sent), formatBytes(total), percent)
                    );
                });
                return true;
            });

            post(() -> {
                if (listener != null) {
                    listener.onFinished(successCount, totalFiles);
                }
            });
        });
    }

    private void showError(String message) {
        post(() -> {
            binding.tvError.setText(message);
            binding.tvError.setVisibility(VISIBLE);
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
