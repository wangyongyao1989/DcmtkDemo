package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.GridLayoutManager
import com.example.dcmtk.PacsManager
import com.example.dcmtk.db.MwlSyncConfig
import com.example.dcmtk.db.MwlSyncRepository
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.model.PatientRecord
import com.example.dcmtk.model.ScanRecord
import com.example.dcmtk.model.WorklistItemMapper
import com.example.dcmtk.utils.MwlTemplateHelper
import com.example.dcmtk.view.WorkListConnectionDialog
import com.example.dcmtk.view.WorkListConnectionView
import com.example.dcmtk.viewmodel.PacsViewModel
import com.example.dcmtkdemo.adapter.PatientAdapter
import com.example.dcmtkdemo.databinding.FragmentWorklistQueryBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.*

class WorklistQueryFragment : Fragment() {

    private var binding: FragmentWorklistQueryBinding? = null
    private lateinit var viewModel: PacsViewModel
    private lateinit var adapter: PatientAdapter

    companion object {
        private const val TAG = "WorklistQueryFragment"
    }

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View? {
        binding = FragmentWorklistQueryBinding.inflate(inflater, container, false)
        return binding?.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        viewModel = ViewModelProvider(requireActivity()).get(PacsViewModel::class.java)

        setupRecyclerView()

        // 进入页面即初始化模板和目录
        viewLifecycleOwner.lifecycleScope.launch {
            MwlTemplateHelper.prepareTemplates(requireContext())
        }

        binding?.btnQueryMwl?.setOnClickListener {
            verifyConnection {
                executeMwlQuery("*")
            }
        }

        binding?.btnQueryMwlTemplate?.setOnClickListener {
            verifyConnection {
                executeMwlQueryByTemplate("wlistqry1.wl")
            }
        }

        binding?.btnPacsConfig?.setOnClickListener {
            verifyConnection(null)
        }

        viewModel.mwlResults.observe(viewLifecycleOwner) { records ->
            adapter.updateData(records)
        }
    }

    private fun setupRecyclerView() {
        adapter = PatientAdapter(ArrayList()) { _ ->
            // Optional click handling
        }
        binding?.rvWorklistResults?.layoutManager = GridLayoutManager(context, 5)
        binding?.rvWorklistResults?.adapter = adapter
    }

    @SuppressLint("SetTextI18n")
    private fun executeMwlQuery(modality: String) {
        binding?.tvWorklistResults?.text = "Querying MWL for modality: $modality..."
        binding?.progressBar?.visibility = View.VISIBLE
        setButtonsEnabled(false)

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val config = viewModel.worklistConfig.value
                if (config == null) {
                    binding?.tvWorklistResults?.text = "Worklist configuration not set. Please verify connection first."
                    return@launch
                }

                // 1. C-FIND 查询 MWL
                val finalResults = withContext(Dispatchers.IO) {
                    PacsManager.cFindMWL(config, modality)
                }

                if (activity == null || binding == null) return@launch

                // 2. 映射为 WorklistItem 列表（用于数据库同步）
                val worklistItems = if (finalResults != null) {
                    WorklistItemMapper.fromMapList(finalResults)
                } else emptyList()

                worklistItems.forEachIndexed { index, item ->
                    Log.d(
                        TAG, "MWL Result #$index: ${item.patientName} (${item.patientID}), " +
                                "acc=${item.accessionNumber}, modality=${item.modality}, " +
                                "studyUID=${item.studyInstanceUID}"
                    )
                }

                // 3. 同步到数据库 — "查询-匹配-存在则修改-不存在则插入"
                val syncConfig = buildMwlSyncConfig()
                val syncResult = withContext(Dispatchers.IO) {
                    MwlSyncRepository.getInstance(requireContext())
                        .syncWorklistItems(worklistItems, syncConfig)
                }
                Log.i(TAG, "MWL DB sync result: $syncResult")

                // 4. 转换为 PatientRecord 供 UI 显示
                val records = finalResults?.map { PatientRecord.fromMap(it) } ?: emptyList()
                records.forEachIndexed { index, record ->
                    Log.d(TAG, "UI PatientRecord #$index: $record")
                }

                // 5. 转换为 ScanRecord 列表（DICOM 写入/业务使用）
                val scanRecords = records.map { pr ->
                    ScanRecord(
                        examineNo = pr.examineNo,
                        patientName = pr.patientName,
                        patientSex = pr.patientSex,
                        patientAge = pr.patientAge,
                        patientHeight = pr.patientHeight,
                        patientWeight = pr.patientWeight,
                        patientTelephoneNumbers = pr.patientTelephoneNumbers,
                        patientNote = pr.patientNote,
                        sendDoctorNo = pr.sendDoctorNo,
                        sendDoctorName = pr.sendDoctorName,
                        sendCheckDate = pr.sendCheckDate,
                        toothPosition = pr.toothPosition,
                        checkDoctorNo = pr.checkDoctorNo,
                        checkDoctorName = pr.checkDoctorName,
                        checkDate = pr.checkDate,
                        birthDate = pr.birthDate,
                        examNumber = pr.examNumber,
                        dcmPath = pr.dcmPath,
                        imagePath = pr.imagePath,
                        modifiedImagePath = pr.modifiedImagePath,
                        deviceName = pr.deviceName,
                        deviceIp = pr.deviceIp,
                        checked = pr.checked
                    )
                }
                scanRecords.forEachIndexed { index, scan ->
                    Log.d(TAG, "Business ScanRecord #$index: $scan")
                }

                viewModel.mwlResults.value = records
                binding?.tvWorklistResults?.text = buildString {
                    append("Found ${records.size} records via MWL C-FIND.\n")
                    append("DB Sync: patient[ins=${syncResult.patientInserted}, upd=${syncResult.patientUpdated}], ")
                    append("study[ins=${syncResult.studyInserted}, upd=${syncResult.studyUpdated}]")
                    if (syncResult.errors.isNotEmpty()) {
                        append(", errors=${syncResult.errors.size}")
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "executeMwlQuery error", e)
                binding?.tvWorklistResults?.text = "Query error: ${e.message}"
            } finally {
                binding?.progressBar?.visibility = View.GONE
                setButtonsEnabled(true)
            }
        }
    }

    /**
     * 构建 MWL 同步配置（对应 C# CodeMaster Code 47/48/49）。
     * 后续可改为从 SharedPreferences 或服务端配置读取。
     */
    private fun buildMwlSyncConfig(): MwlSyncConfig {
        return MwlSyncConfig(
            checkStudyInstanceUIDEnable = true,
            checkAccessionNumberEnable = true,
            checkModalityEnable = false,
            fallbackUserId = "admin"
        )
    }

    @SuppressLint("SetTextI18n")
    private fun executeMwlQueryByTemplate(templateName: String) {
        binding?.tvWorklistResults?.text = "Executing MWL Query by Template: $templateName..."
        binding?.progressBar?.visibility = View.VISIBLE
        setButtonsEnabled(false)

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val config = viewModel.worklistConfig.value
                if (config == null) {
                    binding?.tvWorklistResults?.text = "Worklist configuration not set. Please verify connection first."
                    return@launch
                }
                val exportedFiles = withContext(Dispatchers.IO) {
                    MwlTemplateHelper.prepareTemplates(requireContext())
                    MwlTemplateHelper.executeMwlQuery(
                        requireContext(),
                        config,
                        templateName
                    )
                }

                if (activity == null || binding == null) return@launch

                if (exportedFiles.isNotEmpty()) {
                    val records = ArrayList<PatientRecord>()
                    for (path in exportedFiles) {
                        Log.e(TAG, "path: " + path)
                        val record = withContext(Dispatchers.IO) { parseDicomFile(path) }
                        if (record != null) {
                            records.add(record)
                        }
                    }
                    viewModel.mwlResults.value = records
                    binding?.tvWorklistResults?.text =
                        "MWL Query Complete. Parsed ${records.size} exported DCM files."
                } else {
                    binding?.tvWorklistResults?.text =
                        "MWL Query by Template failed or returned no results."
                }
            } catch (e: Exception) {
                Log.e(TAG, "executeMwlQueryByTemplate error", e)
                binding?.tvWorklistResults?.text = "Query error: ${e.message}"
            } finally {
                binding?.progressBar?.visibility = View.GONE
                setButtonsEnabled(true)
            }
        }
    }

    private fun parseDicomFile(path: String): PatientRecord? {
        return try {
            val info = DcmtkJni.loadDicomFileInfo(path)
            if (info != null && info.isNotEmpty()) {
                PatientRecord.fromMap(info)
            } else null
        } catch (e: Exception) {
            Log.e(TAG, "Failed to parse DCM file: $path", e)
            null
        }
    }

    private fun setButtonsEnabled(enabled: Boolean) {
        binding?.btnQueryMwl?.isEnabled = enabled
        binding?.btnQueryMwlTemplate?.isEnabled = enabled
    }

    private fun verifyConnection(onVerified: (() -> Unit)?) {
        val popupWindow = WorkListConnectionDialog(
            requireContext(),
            viewModel.worklistConfig.value,
            object : WorkListConnectionView.OnConnectionVerifiedListener {
                override fun onVerified(config: PacsConfig) {
                    viewModel.worklistConfig.postValue(config)
                    onVerified?.invoke()
                }

                override fun onCancel() {
                    // 用户取消验证，不执行后续逻辑
                }
            }
        )
        popupWindow.show()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }
}
