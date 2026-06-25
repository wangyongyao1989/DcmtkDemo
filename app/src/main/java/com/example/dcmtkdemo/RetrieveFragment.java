package com.example.dcmtkdemo;

import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import com.example.dcmtkdemo.databinding.FragmentRetrieveBinding;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

public class RetrieveFragment extends Fragment {

    private static final String TAG = "RetrieveFragment";
    private FragmentRetrieveBinding binding;
    private PacsViewModel viewModel;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container
            , @Nullable Bundle savedInstanceState) {
        binding = FragmentRetrieveBinding.inflate(inflater, container, false);
        return binding.getRoot();
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        viewModel = new ViewModelProvider(requireActivity()).get(PacsViewModel.class);

        refreshTempFiles();

        binding.btnRetrieve.setOnClickListener(v -> {
            String patId = binding.etMovePatId.getText().toString().trim();
            String destAet = binding.etDestAet.getText().toString().trim();
            executeMove(patId, destAet);
        });
    }

    private void executeMove(String patId, String destAet) {
        binding.tvRetrieveInfo.setText("Moving PatientID: " + patId + " to " + destAet + "...");
        new Thread(() -> {
            boolean success = DcmtkJni.cMove(
                    viewModel.host.getValue(),
                    viewModel.port.getValue(),
                    viewModel.localAet.getValue(),
                    viewModel.remoteAet.getValue(),
                    patId,
                    destAet
            );
            getActivity().runOnUiThread(() -> {
                binding.tvRetrieveInfo.setText("C-MOVE Result: " + (success ? "Success" : "Failed"));
                if (success) {
                    refreshTempFiles();
                }
            });
        }).start();
    }

    private void refreshTempFiles() {
        File tempDir = new File(requireContext().getExternalFilesDir(null), "temp");
        if (!tempDir.exists()) tempDir.mkdirs();

        File[] files = tempDir.listFiles((dir, name) -> name.toLowerCase().endsWith(".dcm"));
        if (files == null || files.length == 0) {
            binding.tvRetrieveInfo.append("\n\nNo files in temp/ folder.");
            return;
        }

        StringBuilder sb = new StringBuilder("\nFiles in temp/:\n");
        for (File f : files) {
            sb.append("- ").append(f.getName()).append("\n");
            // Optional: load some info
            try {
                HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(f.getAbsolutePath());
                sb.append("  PatName: ").append(info.get("(0010,0010)")).append("\n");
                sb.append("  Modality: ").append(info.get("(0008,0060)")).append("\n");
            } catch (Exception e) {
                Log.e(TAG, "Error loading info for " + f.getName(), e);
            }
        }
        binding.tvRetrieveInfo.setText(sb.toString());
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
