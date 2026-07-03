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
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.PatientRecord
import com.example.dcmtk.utils.MwlTemplateHelper
import com.example.dcmtk.viewmodel.PacsViewModel
import com.example.dcmtkdemo.activity.MainActivity
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

        binding?.btnQueryMwl?.setOnClickListener {
            (requireActivity() as MainActivity).verifyConnection {
                executeMwlQuery("*")
            }
        }

        binding?.btnQueryMwlTemplate?.setOnClickListener {
            (requireActivity() as MainActivity).verifyConnection {
                executeMwlQueryByTemplate("wlistqry1.wl")
            }
        }

        binding?.btnPacsConfig?.setOnClickListener {
            (requireActivity() as MainActivity).verifyConnection(null)
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
            val finalResults = withContext(Dispatchers.IO) {
                try {
                    DcmtkJni.cFindMWL(
                        viewModel.host.value!!,
                        viewModel.port.value!!,
                        viewModel.localAet.value!!,
                        viewModel.remoteAet.value!!,
                        modality
                    )
                } catch (e: Exception) {
                    Log.e(TAG, "MWL Query failed", e)
                    null
                }
            }

            if (activity == null || binding == null) return@launch
            
            binding?.progressBar?.visibility = View.GONE
            setButtonsEnabled(true)

            val records = ArrayList<PatientRecord>()
            if (finalResults != null) {
                for (res in finalResults) {
                    val parts = res.split(" | ".toRegex()).toTypedArray()
                    var name = "N/A"
                    var id = "N/A"
                    var sex = "N/A"
                    var birth = "N/A"
                    var acc = ""
                    var mod = ""
                    if (parts.isNotEmpty()) name = parts[0]
                    for (p in parts) {
                        when {
                            p.startsWith("ID:") -> id = p.substring(3).trim { it <= ' ' }
                            p.startsWith("Acc:") -> acc = p.substring(4).trim { it <= ' ' }
                            p.startsWith("Mod:") -> mod = p.substring(4).trim { it <= ' ' }
                            p.startsWith("Sex:") -> sex = p.substring(4).trim { it <= ' ' }
                            p.startsWith("Birth:") -> birth = p.substring(6).trim { it <= ' ' }
                        }
                    }
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
            val exportedFiles = withContext(Dispatchers.IO) {
                MwlTemplateHelper.prepareTemplates(requireContext())
                MwlTemplateHelper.executeMwlQuery(
                    requireContext(),
                    viewModel.host.value!!,
                    viewModel.port.value!!,
                    viewModel.localAet.value!!,
                    viewModel.remoteAet.value!!,
                    templateName
                )
            }

            if (activity == null || binding == null) return@launch
            
            binding?.progressBar?.visibility = View.GONE
            setButtonsEnabled(true)

            if (exportedFiles.isNotEmpty()) {
                val records = ArrayList<PatientRecord>()
                for (path in exportedFiles) {
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
                val name = info.getOrDefault("(0010,0010)", "N/A")
                val id = info.getOrDefault("(0010,0020)", "N/A")
                val sex = info.getOrDefault("(0010,0040)", "N/A")
                val birth = info.getOrDefault("(0010,0030)", "N/A")
                val acc = info.getOrDefault("(0008,0050)", "")
                val mod = info.getOrDefault("(0008,0060)", "")
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

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }
}
