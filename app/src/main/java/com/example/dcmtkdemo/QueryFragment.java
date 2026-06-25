package com.example.dcmtkdemo;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import com.example.dcmtkdemo.databinding.FragmentQueryBinding;

import java.util.ArrayList;
import java.util.List;

public class QueryFragment extends Fragment {

    private FragmentQueryBinding binding;
    private PacsViewModel viewModel;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container
            , @Nullable Bundle savedInstanceState) {
        binding = FragmentQueryBinding.inflate(inflater, container, false);
        return binding.getRoot();
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        viewModel = new ViewModelProvider(requireActivity()).get(PacsViewModel.class);

        binding.btnQuery.setOnClickListener(v -> {
            String patName = binding.etQueryPatName.getText().toString().trim();
            executeQuery(patName);
        });
    }

    private void executeQuery(String patName) {
        binding.tvQueryResults.setText("Querying for: " + patName + "...");
        new Thread(() -> {
            String[] results = DcmtkJni.cFind(
                    viewModel.host.getValue(),
                    viewModel.port.getValue(),
                    viewModel.localAet.getValue(),
                    viewModel.remoteAet.getValue(),
                    patName
            );
            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                List<PatientRecord> records = new ArrayList<>();
                if (results != null) {
                    for (String res : results) {
                        // JNI Format: "name | ID:id | sex | birth"
                        String[] parts = res.split(" \\| ");
                        String name = parts.length > 0 ? parts[0] : "N/A";
                        String id = parts.length > 1 ? parts[1].replace("ID:", "") : "N/A";
                        String sex = parts.length > 2 ? parts[2] : "N/A";
                        String birth = parts.length > 3 ? parts[3] : "N/A";
                        records.add(new PatientRecord(name, id, sex, birth));
                    }
                }
                viewModel.queryResults.setValue(records);

                if (records.isEmpty()) {
                    binding.tvQueryResults.setText("No results found.");
                } else {
                    binding.tvQueryResults.setText("Found " + records.size()
                            + " records. Switch to Retrieve tab to download.");
                }
            });
        }).start();
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
