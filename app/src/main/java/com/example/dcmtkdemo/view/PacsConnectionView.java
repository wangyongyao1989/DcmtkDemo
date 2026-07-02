package com.example.dcmtkdemo.view;

import android.content.Context;
import android.util.AttributeSet;
import android.view.LayoutInflater;
import android.widget.LinearLayout;
import android.widget.Toast;

import androidx.annotation.Nullable;

import com.example.dcmtk.PacsManager;
import com.example.dcmtkdemo.databinding.ViewPacsConnectionBinding;
import com.example.dcmtkdemo.utils.AppThreadPool;

public class PacsConnectionView extends LinearLayout {

    private ViewPacsConnectionBinding binding;
    private OnConnectionVerifiedListener listener;

    public interface OnConnectionVerifiedListener {
        void onVerified(String host, int port, String local, String remote);
        void onCancel();
    }

    public PacsConnectionView(Context context) {
        this(context, null);
    }

    public PacsConnectionView(Context context, @Nullable AttributeSet attrs) {
        this(context, attrs, 0);
    }

    public PacsConnectionView(Context context, @Nullable AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        setOrientation(VERTICAL);
        binding = ViewPacsConnectionBinding.inflate(LayoutInflater.from(context), this);
        init();
    }

    private void init() {
        binding.btnVerify.setOnClickListener(v -> {
            String host = getHost();
            int port = getPort();
            String local = getLocalAet();
            String remote = getRemoteAet();

            if (host.isEmpty() || port == 0 || local.isEmpty() || remote.isEmpty()) {
                showError("Please fill all fields");
                return;
            }

            binding.tvError.setVisibility(GONE);
            binding.btnVerify.setEnabled(false);
            binding.btnCancel.setEnabled(false);

            AppThreadPool.execute(() -> {
                // Optimization: Use safeCEchoSync (from PacsManager) which includes
                // network read timeout handling and automatic retry logic.
                boolean echoSuccess = PacsManager.safeCEchoSync(host, port, local, remote, 2);
                post(() -> {
                    if (echoSuccess) {
                        binding.tvError.setVisibility(GONE);
                        Toast.makeText(getContext(), "Connection & C-ECHO Verified"
                                , Toast.LENGTH_SHORT).show();
                        if (listener != null) {
                            listener.onVerified(host, port, local, remote);
                        }
                    } else {
                        showError("Verification Failed (Check Network or AETs)");
                    }
                    binding.btnVerify.setEnabled(true);
                    binding.btnCancel.setEnabled(true);
                });
            });
        });

        binding.btnCancel.setOnClickListener(v -> {
            if (listener != null) {
                listener.onCancel();
            }
        });
    }

    public void setOnConnectionVerifiedListener(OnConnectionVerifiedListener listener) {
        this.listener = listener;
    }

    private void showError(String message) {
        binding.tvError.setText(message);
        binding.tvError.setVisibility(VISIBLE);
    }

    public void setConnectionInfo(String host, int port, String local, String remote) {
        binding.etHost.setText(host);
        binding.etPort.setText(String.valueOf(port));
        binding.etLocalAet.setText(local);
        binding.etRemoteAet.setText(remote);
    }

    public String getHost() { return binding.etHost.getText().toString(); }
    public int getPort() {
        try {
            return Integer.parseInt(binding.etPort.getText().toString());
        } catch (Exception e) {
            return 0;
        }
    }
    public String getLocalAet() { return binding.etLocalAet.getText().toString(); }
    public String getRemoteAet() { return binding.etRemoteAet.getText().toString(); }
}
