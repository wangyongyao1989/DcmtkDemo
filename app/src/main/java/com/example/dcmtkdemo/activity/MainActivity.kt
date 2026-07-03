package com.example.dcmtkdemo.activity

import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.view.PacsConnectionDialog
import com.example.dcmtk.view.PacsConnectionView
import com.example.dcmtk.viewmodel.PacsViewModel
import com.example.dcmtkdemo.R
import com.example.dcmtkdemo.databinding.ActivityMainBinding
import com.example.dcmtkdemo.fragment.*
import com.example.dcmtkdemo.utils.FileUtil
import kotlinx.coroutines.launch
import java.io.IOException

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var viewModel: PacsViewModel

    companion object {
        private const val TAG = "MainActivity"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        viewModel = ViewModelProvider(this).get(PacsViewModel::class.java)

        // 初始化字典 (写入和读取都需要)
        lifecycleScope.launch {
            try {
                DcmtkJni.initDcmtk(this@MainActivity)
                // 将 assets 目录下的 .dcm 文件都拷贝到 getExternalFilesDir 下
                FileUtil.copyDcmAssetsToExternal(this@MainActivity)
                viewModel.assetsReady.postValue(true)
            } catch (e: IOException) {
                Log.e(TAG, "Failed to init dictionary or copy assets", e)
            }
        }

        setupNavigation()

        // Default fragment
        if (savedInstanceState == null) {
            switchFragment(UploadFragment())
        }
    }

    fun verifyConnection(onVerified: Runnable?) {
        val popupWindow = PacsConnectionDialog(
            this,
            viewModel.host.value,
            viewModel.port.value,
            viewModel.localAet.value,
            viewModel.remoteAet.value,
            object : PacsConnectionView.OnConnectionVerifiedListener {
                override fun onVerified(host: String, port: Int, local: String, remote: String) {
                    viewModel.host.postValue(host)
                    viewModel.port.postValue(port)
                    viewModel.localAet.postValue(local)
                    viewModel.remoteAet.postValue(remote)
                    onVerified?.run()
                }

                override fun onCancel() {}
            }
        )
        popupWindow.show()
    }

    private fun setupNavigation() {
        binding.bottomNavigation.setOnItemSelectedListener { item ->
            val fragment: Fragment? = when (item.itemId) {
                R.id.nav_upload -> UploadFragment()
                R.id.nav_query -> QueryFragment()
                R.id.nav_worklist -> WorklistQueryFragment()
                R.id.nav_retrieve -> RetrieveFragment()
                R.id.nav_show -> DcmShowFragment()
                else -> null
            }

            fragment?.let {
                switchFragment(it)
                true
            } ?: false
        }
    }

    private fun switchFragment(fragment: Fragment) {
        supportFragmentManager.beginTransaction()
            .replace(R.id.fragment_container, fragment)
            .commit()
    }

    private fun createWatcher(consumer: (String) -> Unit): TextWatcher {
        return object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable?) {
                consumer(s.toString())
            }
        }
    }
}
