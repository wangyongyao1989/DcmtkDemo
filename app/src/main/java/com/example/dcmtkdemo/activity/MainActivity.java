package com.example.dcmtkdemo.activity;

import androidx.appcompat.app.AppCompatActivity;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.PopupWindow;
import android.widget.Toast;

import com.example.dcmtkdemo.R;
import com.example.dcmtkdemo.databinding.ActivityMainBinding;
import com.example.dcmtkdemo.fragment.DcmShowFragment;
import com.example.dcmtkdemo.fragment.QueryFragment;
import com.example.dcmtkdemo.fragment.RetrieveFragment;
import com.example.dcmtkdemo.fragment.UploadFragment;
import com.example.dcmtkdemo.fragment.WorklistQueryFragment;
import com.example.dcmtk.jni.DcmtkJni;
import com.example.dcmtkdemo.utils.FileUtil;
import com.example.dcmtkdemo.view.PacsConnectionView;
import com.example.dcmtkdemo.viewmodel.PacsViewModel;

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

                // 将 assets 目录下的 .dcm 文件都拷贝到 getExternalFilesDir 下
                FileUtil.copyDcmAssetsToExternal(this);
                viewModel.assetsReady.postValue(true);
            } catch (IOException e) {
                Log.e(TAG, "Failed to init dictionary or copy assets", e);
            }
        }).start();

        // setupConnectionBar();
        setupNavigation();

        // Default fragment
        if (savedInstanceState == null) {
            switchFragment(new UploadFragment());
        }
    }

    public void verifyConnection(Runnable onVerified) {
        PacsConnectionView connectionView = new PacsConnectionView(this);
        connectionView.setBackgroundResource(R.drawable.popup_bg);
        connectionView.setPadding(40, 40, 40, 40);

        // Use 85% of screen width
        int width = (int) (getResources().getDisplayMetrics().widthPixels * 0.75);
        PopupWindow popupWindow = new PopupWindow(connectionView, 
                width, 
                ViewGroup.LayoutParams.WRAP_CONTENT, true);

        connectionView.setConnectionInfo(
                viewModel.host.getValue(),
                viewModel.port.getValue(),
                viewModel.localAet.getValue(),
                viewModel.remoteAet.getValue()
        );

        connectionView.setOnConnectionVerifiedListener((host, port, local, remote) -> {
            viewModel.host.postValue(host);
            viewModel.port.postValue(port);
            viewModel.localAet.postValue(local);
            viewModel.remoteAet.postValue(remote);
            
            popupWindow.dismiss();
            if (onVerified != null) onVerified.run();
        });

        popupWindow.showAtLocation(binding.getRoot(), Gravity.CENTER, 0, 0);
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
