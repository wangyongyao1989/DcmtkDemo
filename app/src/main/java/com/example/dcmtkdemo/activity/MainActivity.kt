package com.example.dcmtkdemo.activity;

import androidx.appcompat.app.AppCompatActivity;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;

import com.example.dcmtkdemo.R;
import com.example.dcmtkdemo.databinding.ActivityMainBinding;
import com.example.dcmtkdemo.fragment.DcmShowFragment;
import com.example.dcmtkdemo.fragment.QueryFragment;
import com.example.dcmtkdemo.fragment.RetrieveFragment;
import com.example.dcmtkdemo.fragment.UploadFragment;
import com.example.dcmtkdemo.fragment.WorklistQueryFragment;
import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.utils.AppThreadPool;
import com.example.dcmtkdemo.utils.FileUtil;
import com.example.dcmtk.view.PacsConnectionDialog;
import com.example.dcmtk.view.PacsConnectionView;
import com.example.dcmtk.viewmodel.PacsViewModel;

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
        AppThreadPool.execute(() -> {
            try {
                DcmtkJni.initDcmtk(this);

                // 将 assets 目录下的 .dcm 文件都拷贝到 getExternalFilesDir 下
                FileUtil.copyDcmAssetsToExternal(this);
                viewModel.assetsReady.postValue(true);
            } catch (IOException e) {
                Log.e(TAG, "Failed to init dictionary or copy assets", e);
            }
        });

        // setupConnectionBar();
        setupNavigation();

        // Default fragment
        if (savedInstanceState == null) {
            switchFragment(new UploadFragment());
        }
    }

    public void verifyConnection(Runnable onVerified) {
        PacsConnectionDialog popupWindow = new PacsConnectionDialog(
                this,
                viewModel.host.getValue(),
                viewModel.port.getValue(),
                viewModel.localAet.getValue(),
                viewModel.remoteAet.getValue(),
                new PacsConnectionView.OnConnectionVerifiedListener() {
                    @Override
                    public void onVerified(@androidx.annotation.NonNull String host, int port, @androidx.annotation.NonNull String local, @androidx.annotation.NonNull String remote) {
                        viewModel.host.postValue(host);
                        viewModel.port.postValue(port);
                        viewModel.localAet.postValue(local);
                        viewModel.remoteAet.postValue(remote);
                        if (onVerified != null) onVerified.run();
                    }

                    @Override
                    public void onCancel() {
                    }
                }
        );

        popupWindow.show();
    }

    private void setupNavigation() {
        binding.bottomNavigation.setOnItemSelectedListener(item -> {
            Fragment fragment = null;
            int itemId = item.getItemId();
            if (itemId == R.id.nav_upload) {
                fragment = new UploadFragment();
            } else if (itemId == R.id.nav_query) {
                fragment = new QueryFragment();
            } else if (itemId == R.id.nav_worklist) {
                fragment = new WorklistQueryFragment();
            } else if (itemId == R.id.nav_retrieve) {
                fragment = new RetrieveFragment();
            } else if (itemId == R.id.nav_show) {
                fragment = new DcmShowFragment();
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
