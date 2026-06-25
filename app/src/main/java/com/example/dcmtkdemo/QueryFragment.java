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
            getActivity().runOnUiThread(() -> {
                if (results == null || results.length == 0) {
                    binding.tvQueryResults.setText("No results found.");
                } else {
                    StringBuilder sb = new StringBuilder("Found ")
                            .append(results.length).append(" records:\n\n");
                    for (String res : results) {
                        sb.append(res).append("\n---\n");
                    }
                    binding.tvQueryResults.setText(sb.toString());
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
