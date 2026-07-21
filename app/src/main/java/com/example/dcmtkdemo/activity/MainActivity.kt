package com.example.dcmtkdemo.activity

import android.annotation.SuppressLint
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.ActionBarDrawerToggle
import androidx.core.view.GravityCompat
import androidx.drawerlayout.widget.DrawerLayout
import com.google.android.material.navigation.NavigationView
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import com.example.dcmtk.jni.DcmtkJni
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
            binding.navView.setCheckedItem(R.id.nav_upload)
        }
    }

    private fun setupNavigation() {
        setSupportActionBar(binding.toolbar)

        val toggle = ActionBarDrawerToggle(
            this, binding.drawerLayout, binding.toolbar,
            R.string.navigation_drawer_open, R.string.navigation_drawer_close
        )
        binding.drawerLayout.addDrawerListener(toggle)
        toggle.syncState()

        binding.navView.setNavigationItemSelectedListener { item ->
            val fragment: Fragment? = when (item.itemId) {
                R.id.nav_upload -> UploadFragment()
                R.id.nav_query -> QueryFragment()
                R.id.nav_worklist -> WorklistQueryFragment()
                R.id.nav_retrieve -> RetrieveFragment()
                R.id.nav_show -> DcmShowFragment()
                R.id.nav_compare -> FileCompareFragment()
                R.id.nav_raw_pixel -> RawPixelDealFragment()
                R.id.nav_ct_preprocess -> CTPreprocessFragment()
                else -> null
            }

            fragment?.let {
                switchFragment(it)
                binding.drawerLayout.closeDrawer(GravityCompat.START)
                true
            } ?: false
        }
    }

    @Deprecated("Deprecated in Java")
    @SuppressLint("GestureBackNavigation")
    override fun onBackPressed() {
        if (binding.drawerLayout.isDrawerOpen(GravityCompat.START)) {
            binding.drawerLayout.closeDrawer(GravityCompat.START)
        } else {
            super.onBackPressed()
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
