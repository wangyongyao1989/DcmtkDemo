package com.example.dcmtkdemo.fragment;

import android.annotation.SuppressLint;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.LinearLayoutManager;

import com.example.dcmtkdemo.adapter.PatientAdapter;
import com.example.dcmtkdemo.databinding.FragmentQueryBinding;
import com.example.dcmtkdemo.jni.DcmtkJni;
import com.example.dcmtkdemo.model.PatientRecord;
import com.example.dcmtkdemo.viewmodel.PacsViewModel;

import java.util.ArrayList;
import java.util.List;

public class QueryFragment extends Fragment {

    private FragmentQueryBinding binding;
    private PacsViewModel viewModel;
    private PatientAdapter adapter;

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

        setupRecyclerView();

        binding.btnQuery.setOnClickListener(v -> {
            String patName = binding.etQueryPatName.getText().toString().trim();
            executeQuery(patName, 0);
        });

        binding.btnQueryAccession.setOnClickListener(v -> {
            String accession = binding.etQueryAccession.getText().toString().trim();
            executeQuery(accession, 1);
        });

        binding.btnQueryMwl.setOnClickListener(v -> {
            // Use modality from input or default to '*'
            executeQuery("*", 2);
        });

        viewModel.queryResults.observe(getViewLifecycleOwner(), records -> {
            adapter.updateData(records);
        });
    }

    private void setupRecyclerView() {
        adapter = new PatientAdapter(new ArrayList<>(), record -> {
            // Optional: Click to do something in Query tab? 
            // The requirement didn't specify, but Retrieve tab handles retrieval.
        });
        binding.rvQueryResults.setLayoutManager(new GridLayoutManager(getContext(), 5));
        binding.rvQueryResults.setAdapter(adapter);
    }

    @SuppressLint("SetTextI18n")
    private void executeQuery(String queryVal, int queryType) {
        String label = "Query";
        if (queryType == 0) label = "Name";
        else if (queryType == 1) label = "Accession";
        else if (queryType == 2) label = "MWL";

        binding.tvQueryResults.setText("Querying " + label + ": " + queryVal + "...");
        binding.progressBar.setVisibility(View.VISIBLE);
        setButtonsEnabled(false);

        new Thread(() -> {
            String[] results;
            switch (queryType) {
                case 1:
                    results = DcmtkJni.cFindByAccession(
                            viewModel.host.getValue(),
                            viewModel.port.getValue(),
                            viewModel.localAet.getValue(),
                            viewModel.remoteAet.getValue(),
                            queryVal
                    );
                    break;
                case 2:
                    results = DcmtkJni.cFindMWL(
                            viewModel.host.getValue(),
                            viewModel.port.getValue(),
                            viewModel.localAet.getValue(),
                            viewModel.remoteAet.getValue(),
                            queryVal // Modality
                    );
                    break;
                case 0:
                default:
                    results = DcmtkJni.cFind(
                            viewModel.host.getValue(),
                            viewModel.port.getValue(),
                            viewModel.localAet.getValue(),
                            viewModel.remoteAet.getValue(),
                            queryVal
                    );
                    break;
            }

            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                binding.progressBar.setVisibility(View.GONE);
                setButtonsEnabled(true);

                List<PatientRecord> records = new ArrayList<>();
                if (results != null) {
                    for (String res : results) {
                        // Basic parsing logic: Split by " | " and look for prefixes
                        String[] parts = res.split(" \\| ");
                        String name = "N/A", id = "N/A", sex = "N/A", birth = "N/A", acc = "", mod = "";

                        if (parts.length > 0) name = parts[0];
                        for (String p : parts) {
                            if (p.startsWith("ID:")) id = p.substring(3);
                            else if (p.startsWith("Acc:")) acc = p.substring(4);
                            else if (p.startsWith("Mod:")) mod = p.substring(4);
                            else if (p.equals("M") || p.equals("F") || p.equals("O")) sex = p;
                            else if (p.length() == 8 && p.matches("\\d+")) birth = p; // Simple date check
                        }
                        records.add(new PatientRecord(name, id, sex, birth, acc, mod));
                    }
                }
                viewModel.queryResults.setValue(records);

                if (records.isEmpty()) {
                    binding.tvQueryResults.setText("No results found.");
                } else {
                    binding.tvQueryResults.setText("Found " + records.size()
                            + " records. Details shown below.");
                }
            });
        }).start();
    }

    private void setButtonsEnabled(boolean enabled) {
        binding.btnQuery.setEnabled(enabled);
        binding.btnQueryAccession.setEnabled(enabled);
        binding.btnQueryMwl.setEnabled(enabled);
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
