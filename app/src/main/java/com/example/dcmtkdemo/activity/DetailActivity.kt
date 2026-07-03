package com.example.dcmtkdemo.activity

import android.os.Bundle
import android.util.Log
import android.view.MenuItem
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.bumptech.glide.Glide
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtkdemo.databinding.ActivityDetailBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 详情页：展示某个 DICOM 文件转换后的 JPG 大图，以及更完整的病人/检查信息。
 * 通过 Intent extras 接收 DcmShowFragment 传递的路径与字段；
 * 若 JPG 缩略图不存在则后台触发批量转换以保证可用。
 */
class DetailActivity : AppCompatActivity() {

    private var binding: ActivityDetailBinding? = null

    companion object {
        const val EXTRA_DCM_PATH = "extra_dcm_path"
        const val EXTRA_JPG_PATH = "extra_jpg_path"
        const val EXTRA_NAME = "extra_name"
        const val EXTRA_ID = "extra_id"
        const val EXTRA_SEX = "extra_sex"
        const val EXTRA_STUDY_DATE = "extra_study_date"
        const val EXTRA_STUDY_DESC = "extra_study_desc"
        private const val TAG = "DetailActivity"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityDetailBinding.inflate(layoutInflater)
        setContentView(binding!!.root)

        supportActionBar?.apply {
            setDisplayHomeAsUpEnabled(true)
            title = "DICOM Detail"
        }

        // 直接使用 extras 填充文本字段（已在列表页解析过）
        binding!!.tvDetailName.text = "Patient Name: ${safeExtra(EXTRA_NAME)}"
        binding!!.tvDetailId.text = "Patient ID: ${safeExtra(EXTRA_ID)}"
        binding!!.tvDetailSex.text = "Patient Sex: ${safeExtra(EXTRA_SEX)}"
        binding!!.tvDetailStudyDate.text = "Study Date: ${safeExtra(EXTRA_STUDY_DATE)}"
        binding!!.tvDetailStudyDesc.text = "Study Desc: ${safeExtra(EXTRA_STUDY_DESC)}"

        loadImage()
    }

    /** 加载转换后的 JPG；若不存在则在后台重新触发转换。 */
    private fun loadImage() {
        val jpgPath = intent.getStringExtra(EXTRA_JPG_PATH)
        val dcmPath = intent.getStringExtra(EXTRA_DCM_PATH)

        if (jpgPath != null && File(jpgPath).exists()) {
            Glide.with(this).load(File(jpgPath)).into(binding!!.ivDetailImage)
            return
        }

        // JPG 不存在，后台重新转换（转换是耗时操作，显示 loading）
        binding!!.progressBar.visibility = View.VISIBLE
        
        lifecycleScope.launch {
            val pathToLoad = withContext(Dispatchers.IO) {
                if (dcmPath != null) {
                    val dcmFile = File(dcmPath)
                    val tempDir = dcmFile.parentFile
                    tempDir?.let {
                        DcmtkJni.dcmToJpg(it.absolutePath)
                    }
                }
                
                var finalJpg = jpgPath
                if (finalJpg == null && dcmPath != null) {
                    val dcmFile = File(dcmPath)
                    val tempDir = dcmFile.parentFile
                    if (tempDir != null) {
                        finalJpg = File(File(tempDir, "jpg"),
                            "${dcmFile.name}.jpg").absolutePath
                    }
                }
                finalJpg
            }
            
            if (isFinishing || binding == null) return@launch
            binding!!.progressBar.visibility = View.GONE
            if (pathToLoad != null && File(pathToLoad).exists()) {
                Glide.with(this@DetailActivity).load(File(pathToLoad)).into(binding!!.ivDetailImage)
            } else {
                Log.e(TAG, "loadImage: JPG not available at $pathToLoad")
            }
        }
    }

    private fun safeExtra(key: String): String {
        val v = intent.getStringExtra(key)
        return if (v != null && v.isNotEmpty()) v else "N/A"
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == android.R.id.home) {
            finish()
            return true
        }
        return super.onOptionsItemSelected(item)
    }

    override fun onDestroy() {
        super.onDestroy()
        binding = null
    }
}
