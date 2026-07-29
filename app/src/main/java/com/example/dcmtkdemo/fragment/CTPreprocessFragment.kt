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
import com.example.rawpixeldeal.MedicalCTPreprocess
import com.example.rawpixeldeal.MedicalCTPreprocess.Op
import com.example.rawpixeldeal.MedicalCTPreprocess.PreprocessStep
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * CTPreprocessFragment - Simplified Version
 * Only keeps the Optimal Adjustment flow.
 */
class CTPreprocessFragment : Fragment() {

    private var _binding: FragmentCtPreprocessBinding? = null
    private val binding get() = _binding!!

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentCtPreprocessBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        // Only keep the Optimal Adjustment button
        binding.btnOptimalAdjustment.setOnClickListener {
            runOptimalAdjustment()
        }

        setupAssetSpinner()
    }

    /**
     * 执行“最优的调节”一键流水线 (Requirement 1 & 2)
     * 流程：HU校正 -> 双边去噪(5) -> CLAHE增强(3) -> 图片裁剪 -> 特征锐化(6) -> Invert LUTs
     * 算法：Peak Area (6)
     */
    @SuppressLint("SetTextI18n")
    private fun runOptimalAdjustment() {
        // 1) 同步 UI 状态（实现一键式勾选与参数填充）
        binding.cbHu.isChecked = true
        binding.cbBilateral.isChecked = true
        binding.etBilateralD.setText("5")
        binding.cbClahe.isChecked = true
        binding.etClaheClip.setText("3.0")
        binding.cbTailor.isChecked = true
        binding.cbSharpen.isChecked = true
        binding.etSharpenStrength.setText("6.0")
        binding.cbInvert.isChecked = true
        
        // 选中 Peak Area 调窗算法 (假设 rbWinPeak 还在布局中)
        binding.rbWinPeak.isChecked = true

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
        steps.add(PreprocessStep(Op.HU_CONVERT, listOf(slope, intercept)))
        // 2. 双边去噪 (d=5)
        steps.add(PreprocessStep(Op.BILATERAL, listOf(5.0, 75.0, 75.0)))
        // 3. CLAHE 增强 (clip=3)
        steps.add(PreprocessStep(Op.CLAHE, listOf(3.0, 8.0, 8.0)))
        // 4. 图片裁剪
        steps.add(PreprocessStep(Op.TAILOR, listOf(40000.0, 1.0, 25.0, 10.0)))
        // 5. 特征锐化 (strength=6)
        steps.add(PreprocessStep(Op.FEATURE_SHARPEN, listOf(1.5, 6.0)))
        // 6. Invert LUTs
        steps.add(PreprocessStep(Op.INVERT_LUT))

        // 自动排序校验 (简单保留逻辑)
        validateAndSortSteps(steps)

        binding.btnOptimalAdjustment.isEnabled = false
        binding.tvInfo.text = "执行最优调节流水线 (Peak Area)..."

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
                        windowMethods = listOf(-1, 6) // -1: None (min-max), 6: Peak Area
                    )
                }
                if (_binding == null) return@launch
                binding.ivBefore.setImageBitmap(results[0].bitmap)
                binding.ivAfter.setImageBitmap(results[1].bitmap)
                binding.tvInfo.text = "最优调节完成. 算法: Peak Area Auto"
                binding.tvSummary.text = generateSummary(steps, true, "Peak Area Auto")
            } catch (e: Exception) {
                Log.e(TAG, "Optimal adjustment failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                binding.btnOptimalAdjustment.isEnabled = true
            }
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
