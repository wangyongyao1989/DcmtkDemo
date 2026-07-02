package com.example.dcmtkdemo.view;

import android.content.Context;
import android.view.ViewGroup;
import android.widget.PopupWindow;

import com.example.dcmtkdemo.R;

public class PacsConnectionPopupWindow extends PopupWindow {

    public PacsConnectionPopupWindow(Context context, String host, Integer port
            , String localAet, String remoteAet
            , PacsConnectionView.OnConnectionVerifiedListener listener) {
        super(context);

        PacsConnectionView connectionView = new PacsConnectionView(context);
        connectionView.setBackgroundResource(R.drawable.popup_bg);
        connectionView.setPadding(40, 40, 40, 40);

        setContentView(connectionView);

        // Use 75% of screen width
        int width = (int) (context.getResources().getDisplayMetrics().widthPixels * 0.75);
        setWidth(width);
        setHeight(ViewGroup.LayoutParams.WRAP_CONTENT);

        setFocusable(true);
        setOutsideTouchable(false);

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
