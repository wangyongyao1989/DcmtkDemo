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
import com.example.dcmtk.viewmodel.PacsViewModel
import com.example.dcmtkdemo.activity.MainActivity
import com.example.dcmtkdemo.adapter.PatientAdapter
import com.example.dcmtkdemo.databinding.FragmentQueryBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.*

class QueryFragment : Fragment() {

    private var binding: FragmentQueryBinding? = null
    private lateinit var viewModel: PacsViewModel
    private lateinit var adapter: PatientAdapter

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View? {
        binding = FragmentQueryBinding.inflate(inflater, container, false)
        return binding?.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        viewModel = ViewModelProvider(requireActivity()).get(PacsViewModel::class.java)

        setupRecyclerView()

        binding?.btnQuery?.setOnClickListener {
            (requireActivity() as MainActivity).verifyConnection {
                val patName = binding?.etQueryPatName?.text.toString().trim()
                executeQuery(patName, 0)
            }
        }

        binding?.btnQueryAccession?.setOnClickListener {
            (requireActivity() as MainActivity).verifyConnection {
                val accession = binding?.etQueryAccession?.text.toString().trim()
                executeQuery(accession, 1)
            }
        }

        binding?.btnPacsConfig?.setOnClickListener {
            (requireActivity() as MainActivity).verifyConnection(null)
        }

        viewModel.queryResults.observe(viewLifecycleOwner) { records ->
            adapter.updateData(records)
        }
    }

    private fun setupRecyclerView() {
        adapter = PatientAdapter(ArrayList()) { _ ->
            // Optional click handling
        }
        binding?.rvQueryResults?.layoutManager = GridLayoutManager(context, 5)
        binding?.rvQueryResults?.adapter = adapter
    }

    @SuppressLint("SetTextI18n")
    private fun executeQuery(queryVal: String, queryType: Int) {
        val label = if (queryType == 1) "Accession" else "Name"

        Log.d("QueryFragment", "executeQuery: [START] Type=$label, Value=$queryVal")
        binding?.tvQueryResults?.text = "Querying $label: $queryVal..."
        binding?.progressBar?.visibility = View.VISIBLE
        setButtonsEnabled(false)

        viewLifecycleOwner.lifecycleScope.launch {
            val finalResults = withContext(Dispatchers.IO) {
                Log.d("QueryFragment", "executeQuery: Running on thread ${Thread.currentThread().name}")
                try {
                    if (queryType == 1) {
                        DcmtkJni.cFindByAccession(
                            viewModel.host.value!!,
                            viewModel.port.value!!,
                            viewModel.localAet.value!!,
                            viewModel.remoteAet.value!!,
                            queryVal
                        )
                    } else {
                        DcmtkJni.cFind(
                            viewModel.host.value!!,
                            viewModel.port.value!!,
                            viewModel.localAet.value!!,
                            viewModel.remoteAet.value!!,
                            queryVal
                        )
                    }
                } catch (e: Exception) {
                    Log.e("QueryFragment", "executeQuery: JNI call failed", e)
                    null
                }
            }

            if (activity == null || binding == null) return@launch

            binding?.progressBar?.visibility = View.GONE
            setButtonsEnabled(true)

            val records = ArrayList<PatientRecord>()
            if (finalResults != null) {
                Log.d("QueryFragment", "executeQuery: Received ${finalResults.size} raw records")
                for (res in finalResults) {
                    Log.d("QueryFragment", "Parsing raw result: $res")
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
                            p == "M" || p == "F" || p == "O" -> sex = p
                            p.length == 8 && p.matches("\\d+".toRegex()) -> birth = p
                        }
                    }
                    records.add(PatientRecord(name, id, sex, birth, acc, mod))
                }
            } else {
                Log.w("QueryFragment", "executeQuery: Results is null (possible network or association error)")
            }

            Log.d("QueryFragment", "executeQuery: [DONE] Parsed ${records.size} records")
            viewModel.queryResults.value = records

            if (records.isEmpty()) {
                binding?.tvQueryResults?.text = "No results found."
            } else {
                binding?.tvQueryResults?.text = "Found ${records.size} records. Details shown below."
            }
        }
    }

    private fun setButtonsEnabled(enabled: Boolean) {
        binding?.btnQuery?.isEnabled = enabled
        binding?.btnQueryAccession?.isEnabled = enabled
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }
}
