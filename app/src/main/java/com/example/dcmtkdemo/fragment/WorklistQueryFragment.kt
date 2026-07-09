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
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.model.PatientRecord
import com.example.dcmtk.utils.DicomTag
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
import kotlin.math.log

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
            val config = viewModel.worklistConfig.value ?: return@launch
            val finalResults = withContext(Dispatchers.IO) {
                PacsManager.cFindMWL(config, modality)
            }

            if (activity == null || binding == null) return@launch
            
            binding?.progressBar?.visibility = View.GONE
            setButtonsEnabled(true)

            val records = ArrayList<PatientRecord>()
            if (finalResults != null) {
                finalResults.forEachIndexed { index, map ->
                    Log.d(TAG, "MWL Result #$index:")
                    map.forEach { (tag, value) ->
                        Log.d(TAG, "  $tag -> $value")
                    }
                }
                for (map in finalResults) {
                    val name = map[DicomTag.PatientName.formattedTag] ?: "N/A"
                    val id = map[DicomTag.PatientID.formattedTag] ?: "N/A"
                    val acc = map[DicomTag.AccessionNumber.formattedTag] ?: ""
                    val sex = map[DicomTag.PatientSex.formattedTag] ?: "N/A"
                    val birth = map[DicomTag.PatientBirthDate.formattedTag] ?: "N/A"
                    val mod = map[DicomTag.Modality.formattedTag] ?: ""

                    records.add(PatientRecord(name, id, sex, birth, acc, mod))
                }
            }
            viewModel.mwlResults.value = records
            binding?.tvWorklistResults?.text = "Found ${records.size} records via MWL C-FIND."
        }
    }

    @SuppressLint("SetTextI18n")
    private fun executeMwlQueryByTemplate(templateName: String) {
        binding?.tvWorklistResults?.text = "Executing MWL Query by Template: $templateName..."
        binding?.progressBar?.visibility = View.VISIBLE
        setButtonsEnabled(false)

        viewLifecycleOwner.lifecycleScope.launch {
            val config = viewModel.worklistConfig.value ?: return@launch
            val exportedFiles = withContext(Dispatchers.IO) {
                MwlTemplateHelper.prepareTemplates(requireContext())
                MwlTemplateHelper.executeMwlQuery(
                    requireContext(),
                    config,
                    templateName
                )
            }

            if (activity == null || binding == null) return@launch
            
            binding?.progressBar?.visibility = View.GONE
            setButtonsEnabled(true)

            if (exportedFiles.isNotEmpty()) {
                val records = ArrayList<PatientRecord>()
                for (path in exportedFiles) {
                    Log.e(TAG, "path: "+path)
                    val record = withContext(Dispatchers.IO) { parseDicomFile(path) }
                    if (record != null) {
                        records.add(record)
                    }
                }
                viewModel.mwlResults.value = records
                binding?.tvWorklistResults?.text = "MWL Query Complete. Parsed ${records.size} exported DCM files."
            } else {
                binding?.tvWorklistResults?.text = "MWL Query by Template failed or returned no results."
            }
        }
    }

    private fun parseDicomFile(path: String): PatientRecord? {
        return try {
            val info = DcmtkJni.loadDicomFileInfo(path)
            if (info != null && info.isNotEmpty()) {
                val name = info.getOrDefault(DicomTag.PatientName.formattedTag, "N/A")
                val id = info.getOrDefault(DicomTag.PatientID.formattedTag, "N/A")
                val sex = info.getOrDefault(DicomTag.PatientSex.formattedTag, "N/A")
                val birth = info.getOrDefault(DicomTag.PatientBirthDate.formattedTag, "N/A")
                val acc = info.getOrDefault(DicomTag.AccessionNumber.formattedTag, "")
                val mod = info.getOrDefault(DicomTag.Modality.formattedTag, "")
                PatientRecord(name, id, sex, birth, acc, mod)
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
