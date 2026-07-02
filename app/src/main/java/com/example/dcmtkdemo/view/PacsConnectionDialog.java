package com.example.dcmtkdemo.view;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.view.ViewGroup;
import android.view.Window;

import androidx.annotation.NonNull;

import com.example.dcmtkdemo.R;

/**
 * Using Dialog to ensure it's truly modal and doesn't dismiss when clicking outside.
 */
public class PacsConnectionDialog extends Dialog {

    private final String host;
    private final Integer port;
    private final String localAet;
    private final String remoteAet;
    private final PacsConnectionView.OnConnectionVerifiedListener listener;

    public PacsConnectionDialog(@NonNull Context context, String host, Integer port
            , String localAet, String remoteAet
            , PacsConnectionView.OnConnectionVerifiedListener listener) {
        super(context);
        this.host = host;
        this.port = port;
        this.localAet = localAet;
        this.remoteAet = remoteAet;
        this.listener = listener;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // Remove title bar
        requestWindowFeature(Window.FEATURE_NO_TITLE);

        PacsConnectionView connectionView = new PacsConnectionView(getContext());
        connectionView.setBackgroundResource(R.drawable.popup_bg);
        connectionView.setPadding(40, 40, 40, 40);

        setContentView(connectionView);

        // Make it modal and non-dismissable on outside touch
        setCancelable(false);
        setCanceledOnTouchOutside(false);

        Window window = getWindow();
        if (window != null) {
            // Set background to transparent so we see the rounded corners of R.drawable.popup_bg
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            
            // Set width to 85% of screen
            int width = (int) (getContext().getResources().getDisplayMetrics().widthPixels * 0.85);
            window.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
        }

        connectionView.setConnectionInfo(host, port != null ? port : 0, localAet, remoteAet);

        connectionView.setOnConnectionVerifiedListener(new PacsConnectionView.OnConnectionVerifiedListener() {
            @Override
            public void onVerified(String host, int port, String local, String remote) {
                if (listener != null) {
                    listener.onVerified(host, port, local, remote);
                }
                dismiss();
            }

            @Override
            public void onCancel() {
                if (listener != null) {
                    listener.onCancel();
                }
                dismiss();
            }
        });
    }
}
