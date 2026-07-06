package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
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
import com.example.dcmtk.PacsManager
import com.example.dcmtk.callback.ProgressCallback
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.model.PatientRecord
import com.example.dcmtk.view.PacsConnectionDialog
import com.example.dcmtk.view.PacsConnectionView
import com.example.dcmtk.viewmodel.PacsViewModel
import com.example.dcmtkdemo.adapter.PatientAdapter
import com.example.dcmtkdemo.databinding.FragmentRetrieveBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class RetrieveFragment : Fragment() {

    private var binding: FragmentRetrieveBinding? = null
    private lateinit var viewModel: PacsViewModel
    private lateinit var adapter: PatientAdapter

    private var lastBytes = 0L
    private var lastTime = 0L

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View? {
        binding = FragmentRetrieveBinding.inflate(inflater, container, false)
        return binding?.root
    }

    @SuppressLint("SetTextI18n")
    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        viewModel = ViewModelProvider(requireActivity()).get(PacsViewModel::class.java)

        setupRecyclerView()

        viewModel.queryResults.observe(viewLifecycleOwner) { records ->
            adapter.updateData(records)
            if (records == null || records.isEmpty()) {
                binding?.tvMoveStatus?.text = "No query results. Please go to Query tab first."
            } else {
                binding?.tvMoveStatus?.text = "Found ${records.size} items from Query. Select to retrieve."
            }
        }

        binding?.btnDownloadSelected?.setOnClickListener {
            verifyConnection {
                val selected = adapter.selectedRecords
                if (selected.isEmpty()) {
                    Toast.makeText(context, "Please select at least one item", Toast.LENGTH_SHORT).show()
                    return@verifyConnection
                }
                executeBatchDownload(selected)
            }
        }

        binding?.btnPacsConfig?.setOnClickListener {
            verifyConnection(null)
        }
    }

    @SuppressLint("NotifyDataSetChanged")
    private fun setupRecyclerView() {
        adapter = PatientAdapter(ArrayList()) { record ->
            record.isSelected = !record.isSelected
            adapter.notifyDataSetChanged()
        }
        binding?.rvPatients?.layoutManager = GridLayoutManager(context, 5)
        binding?.rvPatients?.adapter = adapter
    }

    @SuppressLint("SetTextI18n", "DefaultLocale")
    private fun executeBatchDownload(selected: List<PatientRecord>) {
        val externalFilesDir = requireContext().getExternalFilesDir(null) ?: return
        val tempDir = File(externalFilesDir.parentFile, "temp")
        if (!tempDir.exists()) tempDir.mkdirs()

        binding?.apply {
            progressBar.visibility = View.VISIBLE
            progressBar.isIndeterminate = false
            progressBar.max = selected.size
            progressBar.progress = 0
            btnDownloadSelected.isEnabled = false
            tvDownloadStats.visibility = View.VISIBLE
            tvDownloadStats.text = "Speed: 0 KB/s | Progress: 0/${selected.size}"
        }

        viewLifecycleOwner.lifecycleScope.launch {
            val config = viewModel.pacsConfig.value ?: return@launch
            var count = 0
            selected.forEach { record ->
                val currentCount = ++count
                val patId = record.id

                lastBytes = 0L
                lastTime = System.currentTimeMillis()

                binding?.tvMoveStatus?.text = "Downloading ($currentCount/${selected.size}): $patId"

                val success = withContext(Dispatchers.IO) {
                    PacsManager.cGet(
                        config,
                        patId,
                        tempDir.absolutePath,
                        object : ProgressCallback {
                            override fun onProgress(sent: Long, total: Long) {
                                val currentTime = System.currentTimeMillis()
                                val timeDiff = currentTime - lastTime
                                if (timeDiff >= 1000) {
                                    val bytesDiff = sent - lastBytes
                                    val speed = (bytesDiff / 1024.0) / (timeDiff / 1000.0) // KB/s
                                    lastBytes = sent
                                    lastTime = currentTime
                                    
                                    lifecycleScope.launch(Dispatchers.Main) {
                                        binding?.tvDownloadStats?.text = String.format(
                                            "Speed: %.2f KB/s | Progress: %d/%d",
                                            speed, currentCount, selected.size
                                        )
                                    }
                                }
                            }
                        }
                    )
                }

                if (success) {
                    record.isDownloaded = true
                }

                binding?.progressBar?.progress = currentCount
                adapter.notifyDataSetChanged()
            }

            binding?.apply {
                progressBar.visibility = View.GONE
                btnDownloadSelected.isEnabled = true
                tvMoveStatus.text = "Batch download completed."
                selected.forEach { it.isSelected = false }
                adapter.notifyDataSetChanged()
                Toast.makeText(context, "Batch download finished", Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun verifyConnection(onVerified: (() -> Unit)?) {
        val popupWindow = PacsConnectionDialog(
            requireContext(),
            viewModel.pacsConfig.value,
            object : PacsConnectionView.OnConnectionVerifiedListener {
                override fun onVerified(config: PacsConfig) {
                    viewModel.pacsConfig.postValue(config)
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
