package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.GridLayoutManager
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.DicomImageRecord
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.view.DicomUploadDialog
import com.example.dcmtk.view.PacsConnectionDialog
import com.example.dcmtk.view.PacsConnectionView
import com.example.dcmtk.viewmodel.PacsViewModel
import com.example.dcmtkdemo.activity.DetailActivity
import com.example.dcmtkdemo.adapter.DcmUploadAdapter
import com.example.dcmtkdemo.databinding.FragmentUploadBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.*

class UploadFragment : Fragment() {

    private var binding: FragmentUploadBinding? = null
    private lateinit var viewModel: PacsViewModel
    private lateinit var adapter: DcmUploadAdapter

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View? {
        binding = FragmentUploadBinding.inflate(inflater, container, false)
        return binding?.root
    }

    @SuppressLint("NotifyDataSetChanged")
    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        viewModel = ViewModelProvider(requireActivity()).get(PacsViewModel::class.java)

        adapter = DcmUploadAdapter(ArrayList())
        binding?.rvDcmFiles?.layoutManager = GridLayoutManager(context, 5)
        binding?.rvDcmFiles?.adapter = adapter

        adapter.setOnItemClickListener { record ->
            val intent = Intent(context, DetailActivity::class.java)
            intent.putExtra(DetailActivity.EXTRA_DCM_PATH, record.dcmPath)
            intent.putExtra(DetailActivity.EXTRA_JPG_PATH, record.jpgPath)
            intent.putExtra(DetailActivity.EXTRA_NAME, record.name)
            intent.putExtra(DetailActivity.EXTRA_ID, record.id)
            intent.putExtra(DetailActivity.EXTRA_SEX, record.sex)
            intent.putExtra(DetailActivity.EXTRA_STUDY_DATE, record.studyDate)
            intent.putExtra(DetailActivity.EXTRA_STUDY_DESC, record.studyDesc)
            startActivity(intent)
        }

        binding?.btnToggleMode?.setOnClickListener {
            val currentMode = adapter.isUploadMode
            val newMode = !currentMode
            adapter.isUploadMode = newMode
            updateUiMode(newMode)
        }

        // Initialize UI mode
        updateUiMode(false)

        refreshFileList()

        // 监听资产拷贝完成的信号，一旦完成就刷新列表
        viewModel.assetsReady.observe(viewLifecycleOwner) { ready ->
            if (ready) {
                refreshFileList()
            }
        }

        binding?.btnUpload?.setOnClickListener {
            val selectedRecords = adapter.selectedRecords
            if (selectedRecords.isEmpty()) {
                Toast.makeText(context, "No files selected", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }

            verifyConnection {
                val dialog = DicomUploadDialog.newInstance(selectedRecords.toTypedArray(), true
                    , viewModel.pacsConfig.value)
                dialog.setOnUploadFinishedListener(object :
                    DicomUploadDialog.OnUploadFinishedListener {
                    override fun onFinished(successCount: Int, totalCount: Int) {
                        adapter.notifyDataSetChanged()
                        binding?.tvUploadStatus?.text = String.format(
                            Locale.getDefault(),
                            "Last Upload: %d/%d success", successCount, totalCount
                        )
                    }
                })
                dialog.show(parentFragmentManager, "DicomUploadDialog")
            }
        }
    }

    private fun updateUiMode(uploadMode: Boolean) {
        binding?.btnToggleMode?.text =
            if (uploadMode) "Exit Upload Mode" else "Switch to Upload Mode"
        binding?.btnUpload?.visibility = if (uploadMode) View.VISIBLE else View.GONE
        binding?.tvUploadStatus?.visibility = View.VISIBLE // Always show status
        if (!uploadMode) {
            binding?.progressBar?.visibility = View.GONE
        }
    }

    override fun onResume() {
        super.onResume()
        refreshFileList()
    }

    @SuppressLint("SetTextI18n")
    private fun refreshFileList() {
        val dir = requireContext().getExternalFilesDir(null) ?: return

        // 仅列出普通文件，排除 .jpg 与 jpg/ 子目录
        val files = dir.listFiles { _, name ->
            name.lowercase().endsWith(".dcm") && !name.lowercase().endsWith(".jpg")
        }

        if (files == null || files.isEmpty()) {
            binding?.btnUpload?.isEnabled = false
            binding?.tvUploadStatus?.text = "No .dcm files found in ${dir.absolutePath}"
            adapter.updateData(ArrayList())
            return
        }

        binding?.btnUpload?.isEnabled = true
        binding?.tvUploadStatus?.text = "Loading and converting ${files.size} file(s)..."

        viewLifecycleOwner.lifecycleScope.launch {
            val records = withContext(Dispatchers.IO) {
                // 1) 批量转换为 JPG
                DcmtkJni.dcmToJpg(dir.absolutePath)

                // 2) 解析每个文件的 DICOM 信息
                val jpgDir = File(dir, "jpg")
                files.map { f ->
                    val dcmPath = f.absolutePath
                    val jpgPath = File(jpgDir, "${f.name}.jpg").absolutePath

                    var name = "N/A"
                    var id = "N/A"
                    var sex = "N/A"
                    var date = "N/A"
                    var desc = "N/A"
                    try {
                        val info = DcmtkJni.loadDicomFileInfo(dcmPath)
                        if (info != null && info.isNotEmpty()) {
                            name = safeGet(info, "(0010,0010)")
                            id = safeGet(info, "(0010,0020)")
                            sex = safeGet(info, "(0010,0040)")
                            date = safeGet(info, "(0008,0020)")
                            desc = safeGet(info, "(0008,1030)")
                        }
                    } catch (e: Exception) {
                        // ignore
                    }
                    DicomImageRecord(name, id, sex, date, desc, dcmPath, jpgPath)
                }
            }

            binding?.let {
                adapter.updateData(records)
                it.tvUploadStatus.text = "Found ${records.size} DICOM file(s)."
            }
        }
    }

    private fun safeGet(map: HashMap<String, String>, key: String): String {
        val valStr = map[key]
        return if (valStr != null && valStr.isNotEmpty()) valStr else "N/A"
    }

    private fun verifyConnection(onSuccess: (() -> Unit)? = null) {
        PacsConnectionDialog(
            requireContext(),
            viewModel.pacsConfig.value,
            object : PacsConnectionView.OnConnectionVerifiedListener {
                override fun onVerified(config: PacsConfig) {
                    // 更新 ViewModel 中的连接信息。使用 .value 直接同步更新，
                    // 确保后续操作（如弹出上传对话框）能立即读取到最新配置。
                    viewModel.pacsConfig.value = config
                    onSuccess?.invoke()
                }

                override fun onCancel() {
                    // 用户取消验证，不执行后续逻辑
                }
            }
        ).show()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }
}
