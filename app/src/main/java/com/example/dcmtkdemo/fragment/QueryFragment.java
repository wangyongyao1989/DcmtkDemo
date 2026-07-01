package com.example.dcmtkdemo.fragment;

import android.annotation.SuppressLint;
import android.os.Bundle;
import android.util.Log;
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
import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.model.PatientRecord;
import com.example.dcmtkdemo.utils.MwlTemplateHelper;
import com.example.dcmtkdemo.viewmodel.PacsViewModel;

import java.io.File;
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
            ((com.example.dcmtkdemo.activity.MainActivity) requireActivity()).verifyConnection(() -> {
                String patName = binding.etQueryPatName.getText().toString().trim();
                executeQuery(patName, 0);
            });
        });

        binding.btnQueryAccession.setOnClickListener(v -> {
            ((com.example.dcmtkdemo.activity.MainActivity) requireActivity()).verifyConnection(() -> {
                String accession = binding.etQueryAccession.getText().toString().trim();
                executeQuery(accession, 1);
            });
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

        Log.d("QueryFragment", "executeQuery: [START] Type=" + label
                + ", Value=" + queryVal);
        binding.tvQueryResults.setText("Querying " + label + ": " + queryVal + "...");
        binding.progressBar.setVisibility(View.VISIBLE);
        setButtonsEnabled(false);

        new Thread(() -> {
            Log.d("QueryFragment", "executeQuery: Running on thread "
                    + Thread.currentThread().getName());
            String[] results;
            try {
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
            } catch (Exception e) {
                Log.e("QueryFragment", "executeQuery: JNI call failed", e);
                results = null;
            }

            if (getActivity() == null) {
                Log.w("QueryFragment", "executeQuery: Activity is null, aborting UI update");
                return;
            }

            String[] finalResults = results;
            getActivity().runOnUiThread(() -> {
                binding.progressBar.setVisibility(View.GONE);
                setButtonsEnabled(true);

                List<PatientRecord> records = new ArrayList<>();
                if (finalResults != null) {
                    Log.d("QueryFragment", "executeQuery: Received " 
                            + finalResults.length + " raw records");
                    for (String res : finalResults) {
                        Log.d("QueryFragment", "Parsing raw result: " + res);
                        // Basic parsing logic: Split by " | " and look for prefixes
                        String[] parts = res.split(" \\| ");
                        String name = "N/A", id = "N/A", sex = "N/A", birth = "N/A", acc = "", mod = "";

                        if (parts.length > 0) name = parts[0];
                        for (String p : parts) {
                            if (p.startsWith("ID:")) id = p.substring(3).trim();
                            else if (p.startsWith("Acc:")) acc = p.substring(4).trim();
                            else if (p.startsWith("Mod:")) mod = p.substring(4).trim();
                            else if (p.startsWith("Sex:")) sex = p.substring(4).trim();
                            else if (p.startsWith("Birth:")) birth = p.substring(6).trim();
                            // Fallback for old format or simple queries
                            else if (p.equals("M") || p.equals("F") || p.equals("O")) sex = p;
                            else if (p.length() == 8 && p.matches("\\d+")) birth = p;
                        }
                        records.add(new PatientRecord(name, id, sex, birth, acc, mod));
                    }
                } else {
                    Log.w("QueryFragment"
                            , "executeQuery: Results is null (possible network or association error)");
                }
                
                Log.d("QueryFragment", "executeQuery: [DONE] Parsed " + records.size() + " records");
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
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
