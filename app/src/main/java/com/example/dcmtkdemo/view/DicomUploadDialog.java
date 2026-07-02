package com.example.dcmtkdemo.view;

import android.app.Dialog;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.DialogFragment;
import androidx.lifecycle.ViewModelProvider;

import com.example.dcmtkdemo.R;
import com.example.dcmtkdemo.model.DicomImageRecord;
import com.example.dcmtkdemo.viewmodel.PacsViewModel;

import java.util.List;

public class DicomUploadDialog extends DialogFragment {

    private DicomUploadView uploadView;
    private PacsViewModel viewModel;
    private List<DicomImageRecord> uploadRecords;
    private OnUploadFinishedListener listener;
    private boolean autoStart = false;

    public interface OnUploadFinishedListener {
        void onFinished(int successCount, int totalCount);
    }

    public static DicomUploadDialog newInstance(List<DicomImageRecord> records) {
        return newInstance(records, false);
    }

    public static DicomUploadDialog newInstance(List<DicomImageRecord> records, boolean autoStart) {
        DicomUploadDialog dialog = new DicomUploadDialog();
        dialog.uploadRecords = records;
        dialog.autoStart = autoStart;
        return dialog;
    }

    public void setOnUploadFinishedListener(OnUploadFinishedListener listener) {
        this.listener = listener;
    }

    @Override
    public void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setCancelable(false);
    }

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container
            , @Nullable Bundle savedInstanceState) {
        uploadView = new DicomUploadView(requireContext());
        uploadView.setBackgroundResource(R.drawable.popup_bg);
        return uploadView;
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        viewModel = new ViewModelProvider(requireActivity()).get(PacsViewModel.class);

        uploadView.setConnectionInfo(
                viewModel.host.getValue(),
                viewModel.port.getValue(),
                viewModel.localAet.getValue(),
                viewModel.remoteAet.getValue()
        );
        uploadView.setUploadRecords(uploadRecords);
        uploadView.setOnUploadEventListener(new DicomUploadView.OnUploadEventListener() {
            @Override
            public void onFinished(int successCount, int totalCount) {
                if (listener != null) listener.onFinished(successCount, totalCount);
                dismiss();
            }

            @Override
            public void onCancel() {
                dismiss();
            }
        });

        if (autoStart) {
            uploadView.startUploadProcess();
        }
    }

    @Override
    public void onStart() {
        super.onStart();
        Dialog dialog = getDialog();
        if (dialog != null) {
            dialog.setCancelable(false);
            dialog.setCanceledOnTouchOutside(false);
            Window window = dialog.getWindow();
            if (window != null) {
                int width = (int) (getResources().getDisplayMetrics().widthPixels * 0.90);
                window.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
                window.setBackgroundDrawableResource(android.R.color.transparent);
            }
        }
    }
}
