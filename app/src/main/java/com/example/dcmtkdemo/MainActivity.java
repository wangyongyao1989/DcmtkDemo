package com.example.dcmtkdemo;

import androidx.appcompat.app.AppCompatActivity;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.widget.Toast;

import com.example.dcmtkdemo.databinding.ActivityMainBinding;

import java.io.IOException;

public class MainActivity extends AppCompatActivity {
    private static final String TAG = "MainActivity";

    private ActivityMainBinding binding;
    private PacsViewModel viewModel;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        viewModel = new ViewModelProvider(this).get(PacsViewModel.class);

        // 初始化字典 (写入和读取都需要)
        new Thread(() -> {
            try {
                String dictPath = FileUtil.copyAssetToInternalStorage(this
                        , "dicom.dic");
                DcmtkJni.initDcmtk(dictPath);
                // Also copy sample files if they don't exist in external storage for testing
                FileUtil.copyAssetToExternalStorage(this
                        , "CR2026060810120220260609162248FT17.dcm"
                        , "sample.dcm");
            } catch (IOException e) {
                Log.e(TAG, "Failed to init dictionary", e);
            }
        }).start();

        setupConnectionBar();
        setupNavigation();

        // Default fragment
        if (savedInstanceState == null) {
            switchFragment(new UploadFragment());
        }
    }

    private void setupConnectionBar() {
        binding.etHost.setText(viewModel.host.getValue());
        binding.etPort.setText(String.valueOf(viewModel.port.getValue()));
        binding.etLocalAet.setText(viewModel.localAet.getValue());
        binding.etRemoteAet.setText(viewModel.remoteAet.getValue());

        binding.etHost.addTextChangedListener(createWatcher(s -> viewModel.host.setValue(s)));
        binding.etPort.addTextChangedListener(createWatcher(s -> {
            try {
                viewModel.port.setValue(Integer.parseInt(s));
            } catch (NumberFormatException ignored) {}
        }));
        binding.etLocalAet.addTextChangedListener(createWatcher(s
                -> viewModel.localAet.setValue(s)));
        binding.etRemoteAet.addTextChangedListener(createWatcher(s
                -> viewModel.remoteAet.setValue(s)));

        binding.btnConnectPacs.setOnClickListener(v -> {
            new Thread(() -> {
                boolean success = DcmtkJni.connectPACS(
                        viewModel.host.getValue(),
                        viewModel.port.getValue(),
                        viewModel.localAet.getValue(),
                        viewModel.remoteAet.getValue()
                );
                runOnUiThread(() -> Toast.makeText(this, "PACS Connection: "
                        + (success ? "Success" : "Failed"), Toast.LENGTH_SHORT).show());
            }).start();
        });

        binding.btnCecho.setOnClickListener(v -> {
            new Thread(() -> {
                boolean success = DcmtkJni.cEcho(
                        viewModel.host.getValue(),
                        viewModel.port.getValue(),
                        viewModel.localAet.getValue(),
                        viewModel.remoteAet.getValue()
                );
                runOnUiThread(() -> Toast.makeText(this
                        , "C-ECHO: " + (success ? "Success" : "Failed")
                        , Toast.LENGTH_SHORT).show());
            }).start();
        });
    }

    private void setupNavigation() {
        binding.bottomNavigation.setOnItemSelectedListener(item -> {
            Fragment fragment = null;
            int itemId = item.getItemId();
            if (itemId == R.id.nav_upload) {
                fragment = new UploadFragment();
            } else if (itemId == R.id.nav_query) {
                fragment = new QueryFragment();
            } else if (itemId == R.id.nav_retrieve) {
                fragment = new RetrieveFragment();
            }

            if (fragment != null) {
                switchFragment(fragment);
                return true;
            }
            return false;
        });
    }

    private void switchFragment(Fragment fragment) {
        getSupportFragmentManager().beginTransaction()
                .replace(R.id.fragment_container, fragment)
                .commit();
    }

    private TextWatcher createWatcher(java.util.function.Consumer<String> consumer) {
        return new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) {
                consumer.accept(s.toString());
            }
        };
    }
}
