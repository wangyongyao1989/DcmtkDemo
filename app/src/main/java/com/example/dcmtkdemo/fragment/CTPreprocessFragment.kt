package com.example.dcmtkdemo.fragment

import android.content.Context
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.TextView
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtk.utils.LogUtil
import com.example.dcmtkdemo.databinding.FragmentCtPreprocessBinding
import com.example.rawpixeldeal.MedicalCTPreprocess
import com.example.rawpixeldeal.MedicalCTPreprocess.Op
import com.example.rawpixeldeal.MedicalCTPreprocess.PreprocessStep
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * CTPreprocessFragment (Requirement 3 & 4 & 5)
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

        binding.btnRun.setOnClickListener {
            runPreprocessChain()
        }

        binding.btnFullPipeline.setOnClickListener {
            runFullPipeline()
        }

        setupKeyboardDismiss()
    }

    /**
     * 执行标准完整流水线
     */
    private fun runFullPipeline() {
        val ctx = context ?: return
        val assetName = if (binding.rbData610.isChecked) "Data610.bin" else "Data622.bin"
        val w = binding.etWidth.text.toString().toIntOrNull() ?: 1112
        val h = binding.etHeight.text.toString().toIntOrNull() ?: 1740
        val slope = binding.etSlope.text.toString().toFloatOrNull() ?: 1.0f
        val intercept = binding.etIntercept.text.toString().toFloatOrNull() ?: -1024.0f

        // 标准流程通常重采样到 512x512
        val tw = binding.etResW.text.toString().toIntOrNull() ?: 512
        val th = binding.etResH.text.toString().toIntOrNull() ?: 512

        binding.btnFullPipeline.isEnabled = false
        binding.tvInfo.text = "Running Standard Pipeline..."

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    MedicalCTPreprocess.processFullPipeline(
                        context = ctx,
                        assetName = assetName,
                        width = w,
                        height = h,
                        tarW = tw,
                        tarH = th,
                        slope = slope,
                        intercept = intercept
                    )
                }
                if (_binding == null) return@launch
                binding.ivImage.setImageBitmap(result.bitmap)
                binding.tvInfo.text = buildString {
                    append("Standard Pipeline Done.\n")
                    append("Source: $assetName (${w}x${h})\n")
                    append("Output: ${result.outWidth}x${result.outHeight}\n")
                    append("Flow: Raw -> HU -> Bilateral -> Resample -> Stretch -> CLAHE")
                }
                binding.tvSummary.text = "【标准流水线总结】\n执行了官方标准流程：" +
                        "1. HU值校正；2. 双边滤波降噪；3. 线性重采样；4. 对比度拉伸；" +
                        "5. CLAHE局部增强。该流程是医学图像处理的基准，兼顾了边缘保留与对比度提升。"
            } catch (e: Exception) {
                Log.e(TAG, "Full pipeline failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                _binding?.btnFullPipeline?.isEnabled = true
            }
        }
    }

    /**
     * 让所有 EditText 在按下软键盘上的"完成"后能收起键盘；
     * 同时让根 NestedScrollView 在触屏模式下可获焦 + 可点击，
     * 点击 EditText 之外的区域时自动让 EditText 失焦，从而隐藏软键盘。
     */
    private fun setupKeyboardDismiss() {
        val editorListener = TextView.OnEditorActionListener { v, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                v.clearFocus()
                hideKeyboard()
                true
            } else {
                false
            }
        }
        val allEditTexts = listOf(
            binding.etWidth,
            binding.etHeight,
            binding.etSlope,
            binding.etIntercept,
            binding.etGaussK,
            binding.etMedianK,
            binding.etBilateralD,
            binding.etFftR,
            binding.etResW,
            binding.etResH,
            binding.etClaheClip
        )
        allEditTexts.forEach { it.setOnEditorActionListener(editorListener) }

        // 触摸 EditText 之外的区域 -> 根 NestedScrollView 抢焦点 -> EditText 失焦 -> 软键盘收起
        binding.scrollRoot.setOnClickListener {
            LogUtil.e("scrollRoot.setOnClickListener")
            hideKeyboard()
            binding.scrollRoot.requestFocus()
        }
    }

    private fun hideKeyboard() {
        val ctx = context ?: return
        val imm = ctx.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
            ?: return
        val tokenOwner = activity?.currentFocus ?: binding.root
        imm.hideSoftInputFromWindow(tokenOwner.windowToken, 0)
    }

    private fun runPreprocessChain() {
        val ctx = context ?: return

        // 1. 获取基础参数
        val assetName = if (binding.rbData610.isChecked) "Data610.bin" else "Data622.bin"
        val w = binding.etWidth.text.toString().toIntOrNull() ?: 1112
        val h = binding.etHeight.text.toString().toIntOrNull() ?: 1740
        val bitDepth = if (binding.rb16bit.isChecked) 16 else 8
        val slope = binding.etSlope.text.toString().toDoubleOrNull() ?: 1.0
        val intercept = binding.etIntercept.text.toString().toDoubleOrNull() ?: -1024.0

        // 2. 构造处理链 (注意顺序：通常是 HU校正 -> 去噪 -> 重采样 -> 增强)
        val steps = mutableListOf<PreprocessStep>()

        if (binding.cbHu.isChecked) {
            steps.add(PreprocessStep(Op.HU_CONVERT, listOf(slope, intercept)))
        }
        if (binding.cbGaussian.isChecked) {
            val k = binding.etGaussK.text.toString().toDoubleOrNull() ?: 5.0
            steps.add(PreprocessStep(Op.GAUSSIAN, listOf(k, 0.0)))
        }
        if (binding.cbMedian.isChecked) {
            val k = binding.etMedianK.text.toString().toDoubleOrNull() ?: 3.0
            steps.add(PreprocessStep(Op.MEDIAN, listOf(k)))
        }
        if (binding.cbBilateral.isChecked) {
            val d = binding.etBilateralD.text.toString().toDoubleOrNull() ?: 5.0
            steps.add(PreprocessStep(Op.BILATERAL, listOf(d, 50.0, 50.0)))
        }
        if (binding.cbFft.isChecked) {
            val r = binding.etFftR.text.toString().toDoubleOrNull() ?: 30.0
            steps.add(PreprocessStep(Op.FFT, listOf(r)))
        }
        if (binding.cbResample.isChecked) {
            val tw = binding.etResW.text.toString().toDoubleOrNull() ?: 512.0
            val th = binding.etResH.text.toString().toDoubleOrNull() ?: 512.0
            steps.add(PreprocessStep(Op.RESAMPLE_SIZE, listOf(tw, th, 0.0)))
        }
        if (binding.cbStretch.isChecked) {
            steps.add(PreprocessStep(Op.CONTRAST_STRETCH))
        }
        if (binding.cbEqualize.isChecked) {
            steps.add(PreprocessStep(Op.GLOBAL_EQUALIZE))
        }
        if (binding.cbClahe.isChecked) {
            val clip = binding.etClaheClip.text.toString().toDoubleOrNull() ?: 2.0
            steps.add(PreprocessStep(Op.CLAHE, listOf(clip, 8.0, 8.0)))
        }

        if (steps.isEmpty()) {
            // Requirement: If no steps selected, show original image
            Log.i(TAG, "No preprocess steps selected, loading original image.")
        }

        binding.btnRun.isEnabled = false
        binding.tvInfo.text = "Processing..."
        binding.ivImage.setImageDrawable(null)

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    MedicalCTPreprocess.process(
                        context = ctx,
                        assetName = assetName,
                        width = w,
                        height = h,
                        bitDepth = bitDepth,
                        steps = steps
                    )
                }

                if (_binding == null) return@launch
                binding.ivImage.setImageBitmap(result.bitmap)
                binding.tvInfo.text = buildString {
                    append("Source: $assetName (${w}x${h}@${bitDepth}bit)\n")
                    append("Output: ${result.outWidth}x${result.outHeight}\n")
                    append("Range: [${result.minVal}, ${result.maxVal}]\n")
                    val stepsStr = if (steps.isEmpty()) "None (Original)" else steps.joinToString { it.op.displayName }
                    append("Steps: $stepsStr")
                }
                
                // Requirement 5: Summary
                binding.tvSummary.text = generateSummary(steps)

            } catch (e: Exception) {
                Log.e(TAG, "Preprocess failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                _binding?.btnRun?.isEnabled = true
            }
        }
    }

    private fun generateSummary(steps: List<PreprocessStep>): String = buildString {
        appendLine("【预处理总结与效果】")
        if (steps.isEmpty()) {
            appendLine("- 未选择预处理方法：当前展示为原始图像。图像仅经过了大/小端转换及基本的 8-bit 线性映射，用于基准对比。")
            return@buildString
        }
        steps.forEach { step ->
            when (step.op) {
                Op.HU_CONVERT -> appendLine("- HU校正：将原始像素值转换为物理HU值，使图像具有临床诊断意义。")
                Op.GAUSSIAN -> appendLine("- 高斯去噪：平滑图像，抑制高斯噪声，适用于Sinogram预处理。")
                Op.MEDIAN -> appendLine("- 中值去噪：有效去除椒盐噪声，保留边缘效果优于均值滤波。")
                Op.BILATERAL -> appendLine("- 双边去噪：在平滑噪声的同时保留组织边缘，是CT影像降噪的首选。")
                Op.FFT -> appendLine("- 频域去噪：通过傅里叶变换滤除高频周期性噪声（如环形伪影）。")
                Op.RESAMPLE_SIZE -> appendLine("- 重采样：统一空间分辨率，消除因采集参数差异导致的几何失真。")
                Op.GLOBAL_EQUALIZE -> appendLine("- 全局均衡化：提升整体灰度分布均匀度，增强弱对比度区域。")
                Op.CLAHE -> appendLine("- CLAHE：局部自适应增强对比度，抑制噪声放大，突出细节结构。")
                Op.CONTRAST_STRETCH -> appendLine("- 对比度拉伸：将灰度区间映射到0-255，提升视觉可读性。")
                else -> {}
            }
        }
        appendLine("\n达到效果：通过上述组合处理，图像消除了采集噪声，统一了分辨率，并针对关键特征进行了对比度增强，为后续辅助诊断提供了高质量数据基础。")
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    companion object {
        private const val TAG = "CTPreprocessFragment"
    }
}
