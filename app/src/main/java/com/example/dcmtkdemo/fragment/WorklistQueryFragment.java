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

import com.example.dcmtkdemo.adapter.PatientAdapter;
import com.example.dcmtkdemo.databinding.FragmentWorklistQueryBinding;
import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.model.PatientRecord;
import com.example.dcmtkdemo.utils.AppThreadPool;
import com.example.dcmtk.utils.MwlTemplateHelper;
import com.example.dcmtkdemo.viewmodel.PacsViewModel;
import com.example.dcmtkdemo.activity.MainActivity;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

public class WorklistQueryFragment extends Fragment {

    private static final String TAG = "WorklistQueryFragment";
    private FragmentWorklistQueryBinding binding;
    private PacsViewModel viewModel;
    private PatientAdapter adapter;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container
            , @Nullable Bundle savedInstanceState) {
        binding = FragmentWorklistQueryBinding.inflate(inflater, container, false);
        return binding.getRoot();
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        viewModel = new ViewModelProvider(requireActivity()).get(PacsViewModel.class);

        setupRecyclerView();

        binding.btnQueryMwl.setOnClickListener(v -> {
            ((MainActivity) requireActivity()).verifyConnection(() -> {
                executeMwlQuery("*");
            });
        });

        binding.btnQueryMwlTemplate.setOnClickListener(v -> {
            ((MainActivity) requireActivity()).verifyConnection(() -> {
                executeMwlQueryByTemplate("wlistqry1.wl");
            });
        });

        binding.btnPacsConfig.setOnClickListener(v -> {
            ((MainActivity) requireActivity()).verifyConnection(null);
        });

        viewModel.mwlResults.observe(getViewLifecycleOwner(), records -> {
            adapter.updateData(records);
        });
    }

    private void setupRecyclerView() {
        adapter = new PatientAdapter(new ArrayList<>(), record -> {
            // Optional: Click handling
        });
        binding.rvWorklistResults.setLayoutManager(new GridLayoutManager(getContext(), 5));
        binding.rvWorklistResults.setAdapter(adapter);
    }

    @SuppressLint("SetTextI18n")
    private void executeMwlQuery(String modality) {
        binding.tvWorklistResults.setText("Querying MWL for modality: " + modality + "...");
        binding.progressBar.setVisibility(View.VISIBLE);
        setButtonsEnabled(false);

        AppThreadPool.execute(() -> {
            String[] results;
            try {
                results = DcmtkJni.cFindMWL(
                        viewModel.host.getValue(),
                        viewModel.port.getValue(),
                        viewModel.localAet.getValue(),
                        viewModel.remoteAet.getValue(),
                        modality
                );
            } catch (Exception e) {
                Log.e(TAG, "MWL Query failed", e);
                results = null;
            }

            String[] finalResults = results;
            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                binding.progressBar.setVisibility(View.GONE);
                setButtonsEnabled(true);

                List<PatientRecord> records = new ArrayList<>();
                if (finalResults != null) {
                    for (String res : finalResults) {
                        String[] parts = res.split(" \\| ");
                        String name = "N/A", id = "N/A", sex = "N/A", birth = "N/A", acc = "", mod = "";
                        if (parts.length > 0) name = parts[0];
                        for (String p : parts) {
                            if (p.startsWith("ID:")) id = p.substring(3).trim();
                            else if (p.startsWith("Acc:")) acc = p.substring(4).trim();
                            else if (p.startsWith("Mod:")) mod = p.substring(4).trim();
                            else if (p.startsWith("Sex:")) sex = p.substring(4).trim();
                            else if (p.startsWith("Birth:")) birth = p.substring(6).trim();
                        }
                        records.add(new PatientRecord(name, id, sex, birth, acc, mod));
                    }
                }
                viewModel.mwlResults.setValue(records);
                binding.tvWorklistResults.setText("Found " + records.size() + " records via MWL C-FIND.");
            });
        });
    }

    @SuppressLint("SetTextI18n")
    private void executeMwlQueryByTemplate(String templateName) {
        binding.tvWorklistResults.setText("Executing MWL Query by Template: " + templateName + "...");
        binding.progressBar.setVisibility(View.VISIBLE);
        setButtonsEnabled(false);

        AppThreadPool.execute(() -> {
            MwlTemplateHelper.prepareTemplates(getContext());
            String[] exportedFiles = MwlTemplateHelper.executeMwlQuery(
                    getContext(),
                    viewModel.host.getValue(),
                    viewModel.port.getValue(),
                    viewModel.localAet.getValue(),
                    viewModel.remoteAet.getValue(),
                    templateName
            );

            if (getActivity() == null) return;
            getActivity().runOnUiThread(() -> {
                binding.progressBar.setVisibility(View.GONE);
                setButtonsEnabled(true);

                if (exportedFiles != null && exportedFiles.length > 0) {
                    List<PatientRecord> records = new ArrayList<>();
                    for (String path : exportedFiles) {
                        PatientRecord record = parseDicomFile(path);
                        if (record != null) {
                            records.add(record);
                        }
                    }
                    viewModel.mwlResults.setValue(records);
                    binding.tvWorklistResults.setText("MWL Query Complete. Parsed " + records.size() + " exported DCM files.");
                } else {
                    binding.tvWorklistResults.setText("MWL Query by Template failed or returned no results.");
                }
            });
        });
    }

    private PatientRecord parseDicomFile(String path) {
        try {
            HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(path);
            if (info != null && !info.isEmpty()) {
                String name = info.getOrDefault("(0010,0010)", "N/A");
                String id = info.getOrDefault("(0010,0020)", "N/A");
                String sex = info.getOrDefault("(0010,0040)", "N/A");
                String birth = info.getOrDefault("(0010,0030)", "N/A");
                String acc = info.getOrDefault("(0008,0050)", "");
                String mod = info.getOrDefault("(0008,0060)", "");
                
                return new PatientRecord(name, id, sex, birth, acc, mod);
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to parse DCM file: " + path, e);
        }
        return null;
    }

    private void setButtonsEnabled(boolean enabled) {
        binding.btnQueryMwl.setEnabled(enabled);
        binding.btnQueryMwlTemplate.setEnabled(enabled);
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
