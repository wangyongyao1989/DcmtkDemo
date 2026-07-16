package com.example.dcmtkdemo.fragment

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import com.example.rawpixeldeal.RawPixelDealVerify
import com.example.dcmtkdemo.databinding.FragmentRawPixelDealBinding

class RawPixelDealFragment : Fragment() {

    private var _binding: FragmentRawPixelDealBinding? = null
    private val binding get() = _binding!!

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentRawPixelDealBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        binding.btnVerify.setOnClickListener {
            runVerify()
        }
    }

    private fun runVerify() {
        try {
            val report = RawPixelDealVerify.verifyChain()
            val resultText = buildString {
                appendLine("OpenCV Version: ${report.opencvVersion}")
                appendLine("Native Message: ${report.nativeMessage}")
                appendLine("Image Size: ${report.width}x${report.height}")
                appendLine("Pixel p00 (After): ${report.p00}")
                appendLine("Pixel Sum: ${report.pixelSum}")
                appendLine("Raw Before Head: ${report.rawBeforeHead.joinToString()}")
                appendLine("Raw After Head: ${report.rawAfterHead.joinToString()}")
                appendLine("Changed Any Pixel: ${report.changedAnyPixel}")
                appendLine("\nSUCCESS: Native -> C++ -> OpenCV -> Kotlin chain verified!")
            }
            binding.tvResult.text = resultText
        } catch (e: Exception) {
            binding.tvResult.text = "ERROR: ${e.message}"
            e.printStackTrace()
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
