package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtkdemo.databinding.FragmentCtPreprocessBinding
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.PixelDataNew
import com.example.dcmtk.model.ScanRecord
import com.example.rawpixeldeal.MedicalCTPreprocess
import com.example.rawpixeldeal.MedicalCTPreprocess.Op
import com.example.rawpixeldeal.MedicalCTPreprocess.PreprocessStep
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * CTPreprocessFragment - Optimized Version
 * Supports configurable pipeline steps and Write DICOM.
 */
class CTPreprocessFragment : Fragment() {

    private var _binding: FragmentCtPreprocessBinding? = null
    private val binding get() = _binding!!

    private var lastResult: MedicalCTPreprocess.PreprocessResult? = null

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentCtPreprocessBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        binding.btnOptimalAdjustment.setOnClickListener {
            runOptimalAdjustment()
        }

        binding.btnWriteDcm.setOnClickListener {
            runSaveDcmFile()
        }

        setupAssetSpinner()
    }

    /**
     * 根据 UI 勾选动态执行预处理流水线
     */
    @SuppressLint("SetTextI18n")
    private fun runOptimalAdjustment() {
        val ctx = context ?: return
        val assetName = binding.spinnerAsset.selectedItem?.toString() ?: return
        val w = binding.etWidth.text.toString().toIntOrNull() ?: 1112
        val h = binding.etHeight.text.toString().toIntOrNull() ?: 1740
        val bitDepth = if (binding.rb16bit.isChecked) 16 else 8
        val isBigEndian = binding.rbBigEndian.isChecked
        val slope = binding.etSlope.text.toString().toDoubleOrNull() ?: 1.0
        val intercept = binding.etIntercept.text.toString().toDoubleOrNull() ?: -1024.0

        val steps = mutableListOf<PreprocessStep>()
        
        // 1. HU 校正
        if (binding.cbHu.isChecked) {
            steps.add(PreprocessStep(Op.HU_CONVERT, listOf(slope, intercept)))
        }
        
        // 2. 双边去噪
        if (binding.cbBilateral.isChecked) {
            val d = binding.etBilateralD.text.toString().toDoubleOrNull() ?: 5.0
            steps.add(PreprocessStep(Op.BILATERAL, listOf(d, 75.0, 75.0)))
        }
        
        // 3. CLAHE 增强
        if (binding.cbClahe.isChecked) {
            val clip = binding.etClaheClip.text.toString().toDoubleOrNull() ?: 3.0
            steps.add(PreprocessStep(Op.CLAHE, listOf(clip, 8.0, 8.0)))
        }
        
        // 4. 图片裁剪
        if (binding.cbTailor.isChecked) {
            steps.add(PreprocessStep(Op.TAILOR, listOf(40000.0, 1.0, 25.0, 10.0)))
        }
        
        // 5. 特征锐化
        if (binding.cbSharpen.isChecked) {
            val strength = binding.etSharpenStrength.text.toString().toDoubleOrNull() ?: 6.0
            steps.add(PreprocessStep(Op.FEATURE_SHARPEN, listOf(1.5, strength)))
        }
        
        // 6. Invert LUTs
        if (binding.cbInvert.isChecked) {
            steps.add(PreprocessStep(Op.INVERT_LUT))
        }

        validateAndSortSteps(steps)

        binding.btnOptimalAdjustment.isEnabled = false
        binding.btnWriteDcm.isEnabled = false
        binding.tvInfo.text = "正在执行预处理流水线..."

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val bytes = withContext(Dispatchers.IO) {
                    ctx.assets.open(assetName).use { it.readBytes() }
                }

                val results = withContext(Dispatchers.IO) {
                    MedicalCTPreprocess.processCompareWindows(
                        rawBuffer = bytes,
                        width = w,
                        height = h,
                        bitDepth = bitDepth,
                        bigEndian = isBigEndian,
                        isUint16 = true,
                        steps = steps,
                        windowMethods = listOf(-1, 6)
                    )
                }
                if (_binding == null) return@launch
                
                lastResult = results[1] // 保存 Peak Area 结果用于写 DCM
                
                binding.ivBefore.setImageBitmap(results[0].bitmap)
                binding.ivAfter.setImageBitmap(results[1].bitmap)
                binding.tvInfo.text = "处理完成. 算法: Peak Area Auto"
                binding.tvSummary.text = generateSummary(steps, true, "Peak Area Auto")
                binding.btnWriteDcm.isEnabled = true
            } catch (e: Exception) {
                Log.e(TAG, "Pipeline failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                binding.btnOptimalAdjustment.isEnabled = true
            }
        }
    }

    /**
     * 将处理后的数据保存为 DICOM 文件
     */
    private fun runSaveDcmFile() {
        val result = lastResult ?: return
        val ctx = context ?: return
        
        binding.btnWriteDcm.isEnabled = false
        binding.tvInfo.text = "正在写入 DICOM 文件..."

        viewLifecycleOwner.lifecycleScope.launch {
            val success = withContext(Dispatchers.IO) {
                try {
                    val outDir = File(ctx.getExternalFilesDir(null), "dcm_out")
                    if (!outDir.exists()) outDir.mkdirs()
                    val outFile = File(outDir, "processed_${System.currentTimeMillis()}.dcm")

                    val record = ScanRecord(
                        examineNo = System.currentTimeMillis(),
                        patientName = "CT_PREPROCESS_TEST",
                        patientAge = "030Y",
                        patientSex = "M",
                        toothPosition = "FULL_BODY"
                    )

                    // 注意：DCMTK writeDcmFile 需要原始像素数据。
                    // 这里简化逻辑：如果是演示性质，通常需要将处理后的 HU 数据回填。
                    // 实际项目中，PixelDataNew 的 data 应该与 result 关联。
                    val px = PixelDataNew(
                        rows = result.outHeight,
                        columns = result.outWidth,
                        data = null, // 这里暂时传 null 或需要重新提取像素
                        largestImagePixelValue = result.maxVal,
                        win_center = (result.minVal + result.maxVal) / 2,
                        win_width = result.maxVal - result.minVal,
                        exposure_leve = 0,
                        standardDeviation = 0.0
                    )

                    DcmtkJni.writeDcmFile(record, px, outFile.absolutePath)
                } catch (e: Exception) {
                    Log.e(TAG, "Save DCM failed", e)
                    false
                }
            }

            if (_binding == null) return@launch
            binding.tvInfo.text = if (success) "DICOM 写入成功" else "DICOM 写入失败"
            binding.btnWriteDcm.isEnabled = true
        }
    }

    private fun setupAssetSpinner() {
        val ctx = context ?: return
        val assets = ctx.assets.list("") ?: emptyArray()
        val fileList = assets.filter { it.endsWith(".bin") || it.endsWith(".raw") }
        if (fileList.isNotEmpty()) {
            val adapter = ArrayAdapter(ctx, android.R.layout.simple_spinner_item, fileList)
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            binding.spinnerAsset.adapter = adapter
        }
    }

    private fun validateAndSortSteps(steps: MutableList<PreprocessStep>) {
        val huIdx = steps.indexOfFirst { it.op == Op.HU_CONVERT }
        val invertIdx = steps.indexOfFirst { it.op == Op.INVERT_LUT }

        val newHuIdx = steps.indexOfFirst { it.op == Op.HU_CONVERT }
        val newInvertIdx = steps.indexOfFirst { it.op == Op.INVERT_LUT }
        if (newHuIdx >= 0 && newInvertIdx >= 0 && newHuIdx > newInvertIdx) {
            val huStep = steps.removeAt(newHuIdx)
            steps.add(newInvertIdx, huStep)
        }
    }

    private fun generateSummary(
        steps: List<PreprocessStep>,
        isWin: Boolean,
        method: String
    ): String = buildString {
        appendLine(if (isWin) "【调窗前后类比分析】" else "【预处理操作总结】")
        steps.forEach { step ->
            val p = step.params
            when (step.op) {
                Op.TAILOR -> appendLine("- 图片裁剪：自动定位主体并旋转，去除无效背景。")
                Op.INVERT_LUT -> appendLine("- Invert LUTs：色度反转，改变图像极性。")
                Op.HU_CONVERT -> appendLine("- HU校正：还原物理密度值。")
                Op.BILATERAL -> appendLine("- 双边去噪：保边平滑，提升信噪比。")
                Op.CLAHE -> appendLine("- CLAHE：局部对比度增强。")
                Op.FEATURE_SHARPEN -> appendLine("- 特征锐化：针对骨皮质和骨小梁的USM增强。")
                else -> appendLine("- ${step.op.displayName}")
            }
        }
        if (isWin) {
            appendLine("- 调窗算法 ($method)：左图为基础线性映射，右图为应用算法后的诊断增强效果。")
        }
    }

    override fun onDestroyView() {
        super.onDestroyView(); _binding = null
    }

    companion object {
        private const val TAG = "CTPreprocessFragment"
    }
}
