package com.example.dcmtkdemo.view;

import android.content.Context;
import android.util.AttributeSet;
import android.view.LayoutInflater;
import android.widget.LinearLayout;
import android.widget.Toast;

import androidx.annotation.Nullable;

import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.databinding.ViewPacsConnectionBinding;

public class PacsConnectionView extends LinearLayout {

    private ViewPacsConnectionBinding binding;
    private OnConnectionVerifiedListener listener;

    public interface OnConnectionVerifiedListener {
        void onVerified(String host, int port, String local, String remote);
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
        binding.btnTestConnect.setOnClickListener(v -> {
            String host = getHost();
            int port = getPort();
            String local = getLocalAet();
            String remote = getRemoteAet();

            new Thread(() -> {
                boolean success = DcmtkJni.connectPACS(host, port, local, remote);
                post(() -> Toast.makeText(getContext(), "Connection: "
                        + (success ? "OK" : "Failed"), Toast.LENGTH_SHORT).show());
            }).start();
        });

        binding.btnCecho.setOnClickListener(v -> {
            String host = getHost();
            int port = getPort();
            String local = getLocalAet();
            String remote = getRemoteAet();

            new Thread(() -> {
                boolean success = DcmtkJni.cEcho(host, port, local, remote);
                post(() -> {
                    Toast.makeText(getContext(), "C-ECHO: "
                            + (success ? "Success" : "Failed"), Toast.LENGTH_SHORT).show();
                    if (success && listener != null) {
                        listener.onVerified(host, port, local, remote);
                    }
                });
            }).start();
        });
    }

    public void setOnConnectionVerifiedListener(OnConnectionVerifiedListener listener) {
        this.listener = listener;
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
