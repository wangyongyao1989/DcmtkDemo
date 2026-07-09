package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.GridLayoutManager
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.DicomImageRecord
import com.example.dcmtk.utils.DicomTag
import com.example.dcmtkdemo.activity.DetailActivity
import com.example.dcmtkdemo.adapter.DcmImageAdapter
import com.example.dcmtkdemo.databinding.FragmentDcmShowBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.*

/**
 * 展示 ../temp 目录下通过 C-GET 下载的 DICOM 文件。
 * 1) 列出 temp 目录下的文件，若无则提示用户去 Retrieve 下载；
 * 2) 后台调用 native dcmToJpg 将所有文件转成 JPG（输出到 temp/jpg/）；
 * 3) 解析每个文件的 Patient Name/ID/Sex 等信息，以 RecyclerView 展示，item 仅含 JPG 缩略图 + name + id + sex；
 * 4) 点击 item 跳转到 DetailActivity 显示更多详情。
 */
class DcmShowFragment : Fragment() {

    private var binding: FragmentDcmShowBinding? = null
    private lateinit var adapter: DcmImageAdapter

    companion object {
        private const val TAG = "DcmShowFragment"
    }

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View? {
        binding = FragmentDcmShowBinding.inflate(inflater, container, false)
        return binding?.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        adapter = DcmImageAdapter(ArrayList()) { openDetail(it) }
        binding?.rvDcmImages?.layoutManager = GridLayoutManager(context, 5)
        binding?.rvDcmImages?.adapter = adapter

        loadAndConvert()
    }

    /** 解析 ../temp 目录 */
    private fun getTempDir(): File? {
        val externalFilesDir = requireContext().getExternalFilesDir(null) ?: return null
        val tempDir = File(externalFilesDir.parentFile, "temp")
        if (!tempDir.exists()) tempDir.mkdirs()
        return tempDir
    }

    @SuppressLint("SetTextI18n")
    private fun loadAndConvert() {
        val tempDir = getTempDir()
        if (tempDir == null) {
            showEmpty("External storage not available.")
            return
        }

        // 仅列出普通文件，排除 .jpg（转换产物）与 jpg/ 子目录
        val files = tempDir.listFiles { _, name ->
            !name.lowercase().endsWith(".jpg")
        }
        val dcmFiles = ArrayList<File>()
        files?.forEach { f ->
            if (f.isFile) dcmFiles.add(f)
        }

        if (dcmFiles.isEmpty()) {
            showEmpty("No files in temp/ folder.\nPlease go to the Retrieve tab to download DICOM files first.")
            return
        }

        showLoading("Converting ${dcmFiles.size} file(s) to JPG...")

        viewLifecycleOwner.lifecycleScope.launch {
            val (records, converted) = withContext(Dispatchers.IO) {
                // 1) 批量转换为 JPG
                val convertedCount = DcmtkJni.dcmToJpg(tempDir.absolutePath)
                Log.d(TAG, "dcmToJpg converted=$convertedCount")

                // 2) 解析每个文件的 DICOM 信息并构建记录
                val jpgDir = File(tempDir, "jpg")
                val recordsList = dcmFiles.map { f ->
                    val dcmPath = f.absolutePath
                    val jpgPath = File(jpgDir, "${f.name}.jpg").absolutePath

                    var name = "N/A"
                    var id = "N/A"
                    var sex = "N/A"
                    var studyDate = "N/A"
                    var studyDesc = "N/A"
                    try {
                        val info = DcmtkJni.loadDicomFileInfo(dcmPath)
                        if (info != null && info.isNotEmpty()) {
                            name = safeGet(info, DicomTag.PatientName.formattedTag)
                            id = safeGet(info, DicomTag.PatientID.formattedTag)
                            sex = safeGet(info, DicomTag.PatientSex.formattedTag)
                            studyDate = safeGet(info, DicomTag.StudyDate.formattedTag)
                            studyDesc = safeGet(info, DicomTag.StudyDescription.formattedTag)
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, "Error loading info for ${f.name}", e)
                    }
                    DicomImageRecord(name, id, sex, studyDate, studyDesc, dcmPath, jpgPath)
                }
                Pair(recordsList, convertedCount)
            }

            if (activity == null || binding == null) return@launch
            
            hideLoading()
            if (records.isEmpty()) {
                showEmpty("No valid DICOM files in temp/ folder.")
            } else {
                binding?.apply {
                    rvDcmImages.visibility = View.VISIBLE
                    tvDcmEmpty.visibility = View.GONE
                    tvDcmShowStatus.text = "Showing ${records.size} image(s). Converted $converted to JPG."
                    adapter.updateData(records)
                }
            }
        }
    }

    private fun openDetail(record: DicomImageRecord) {
        val intent = Intent(context, DetailActivity::class.java).apply {
            putExtra(DetailActivity.EXTRA_DCM_PATH, record.dcmPath)
            putExtra(DetailActivity.EXTRA_JPG_PATH, record.jpgPath)
            putExtra(DetailActivity.EXTRA_NAME, record.name)
            putExtra(DetailActivity.EXTRA_ID, record.id)
            putExtra(DetailActivity.EXTRA_SEX, record.sex)
            putExtra(DetailActivity.EXTRA_STUDY_DATE, record.studyDate)
            putExtra(DetailActivity.EXTRA_STUDY_DESC, record.studyDesc)
        }
        startActivity(intent)
    }

    private fun showLoading(status: String) {
        binding?.apply {
            progressBar.visibility = View.VISIBLE
            rvDcmImages.visibility = View.GONE
            tvDcmEmpty.visibility = View.GONE
            tvDcmShowStatus.text = status
        }
    }

    private fun hideLoading() {
        binding?.progressBar?.visibility = View.GONE
    }

    private fun showEmpty(message: String) {
        binding?.apply {
            progressBar.visibility = View.GONE
            rvDcmImages.visibility = View.GONE
            tvDcmEmpty.visibility = View.VISIBLE
            tvDcmEmpty.text = message
            tvDcmShowStatus.text = ""
        }
    }

    private fun safeGet(map: HashMap<String, String>, key: String): String {
        val valStr = map[key]
        return if (valStr != null && valStr.isNotEmpty()) valStr else "N/A"
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }
}
