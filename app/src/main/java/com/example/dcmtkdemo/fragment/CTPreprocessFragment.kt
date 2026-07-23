package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.content.Context
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtk.utils.LogUtil
import com.example.dcmtkdemo.databinding.FragmentCtPreprocessBinding
import com.example.rawpixeldeal.MedicalCTPreprocess
import com.example.rawpixeldeal.MedicalCTPreprocess.Op
import com.example.rawpixeldeal.MedicalCTPreprocess.PreprocessStep
import com.example.rawpixeldeal.jni.RawPixelDealJni
import com.example.rawpixeldeal.xray.WindowMethod
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * CTPreprocessFragment - Optimized Version
 * Reorganized by operation categories and added side-by-side windowing comparison.
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

        // 3) Preprocess Operations - Load & Display
        binding.btnRun.setOnClickListener {
            runPreprocessChain(isWindowing = false)
        }

        // 4) One-Click Pipelines
        binding.btnFullPipeline.setOnClickListener {
            runFullPipeline()
        }
        binding.btnTailorPipeline.setOnClickListener {
            runTailorPipeline()
        }

        // 5) Chromatic Inversion
        binding.btnInvertLut.setOnClickListener {
            runInstantInvertLut()
        }

        // 6) Windowing
        binding.btnWindowing.setOnClickListener {
            runPreprocessChain(isWindowing = true)
        }

        setupWindowMethodRadioLogic()
        setupKeyboardDismiss()
    }

    private fun setupWindowMethodRadioLogic() {
        val radioButtons = listOf(
            binding.rbWinNone,
            binding.rbWinDefault,
            binding.rbWin72,
            binding.rbWinBimodal,
            binding.rbWinAdaptive,
            binding.rbWinHistType,
            binding.rbWinMinMax
        )
        radioButtons.forEach { rb ->
            rb.setOnClickListener {
                val wasChecked = rb.tag as? Boolean ?: false
                if (wasChecked) {
                    rb.isChecked = false
                    rb.tag = false
                } else {
                    radioButtons.forEach {
                        it.isChecked = false
                        it.tag = false
                    }
                    rb.isChecked = true
                    rb.tag = true
                }
            }
        }
    }

    /**
     * 5) 独立色度反转逻辑 (针对当前勾选的操作执行瞬时反转)
     */
    @SuppressLint("SetTextI18n")
    private fun runInstantInvertLut() {
        // 强制勾选 Invert LUTs 并执行加载显示
        binding.cbInvert.isChecked = true
        runPreprocessChain(isWindowing = false)
    }

    /**
     * 执行裁剪后标准流程 (One-Click)
     */
    @SuppressLint("SetTextI18n")
    private fun runTailorPipeline() {
        // 模拟一键勾选：裁剪 + HU + 去噪 + 增强
        binding.cbTailor.isChecked = true
        binding.cbHu.isChecked = true
        binding.cbBilateral.isChecked = true
        binding.cbClahe.isChecked = true
        binding.cbInvert.isChecked = false
        runPreprocessChain(isWindowing = false)
    }

    /**
     * 执行标准完整流水线 (One-Click)
     */
    @SuppressLint("SetTextI18n")
    private fun runFullPipeline() {
        val ctx = context ?: return
        val assetName = if (binding.rbData610.isChecked) "Data610.bin" else "Data622.raw"
        val w = binding.etWidth.text.toString().toIntOrNull() ?: 1112
        val h = binding.etHeight.text.toString().toIntOrNull() ?: 1740
        val slope = binding.etSlope.text.toString().toFloatOrNull() ?: 1.0f
        val intercept = binding.etIntercept.text.toString().toFloatOrNull() ?: -1024.0f
        val isBigEndian = binding.rbBigEndian.isChecked
        val tw = binding.etResW.text.toString().toIntOrNull() ?: 512
        val th = binding.etResH.text.toString().toIntOrNull() ?: 512

        binding.btnFullPipeline.isEnabled = false
        binding.tvInfo.text = "Running Standard Full Pipeline..."

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    MedicalCTPreprocess.processFullPipeline(
                        context = ctx, assetName = assetName,
                        width = w, height = h, tarW = tw, tarH = th,
                        slope = slope, intercept = intercept, bigEndian = isBigEndian
                    )
                }
                if (_binding == null) return@launch
                binding.ivBefore.setImageBitmap(result.bitmap)
                binding.ivAfter.setImageDrawable(null)
                binding.tvInfo.text = "Standard Pipeline Done. (Output: ${result.outWidth}x${result.outHeight})"
                binding.tvSummary.text = "【标准流水线一键操作】\n执行了官方标准流程：HU校正、双边降噪、重采样、CLAHE增强。该流程是医学图像处理的基准。"
            } catch (e: Exception) {
                Log.e(TAG, "Full pipeline failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                binding.btnFullPipeline.isEnabled = true
            }
        }
    }

    /**
     * 核心处理链逻辑 - 支持对比显示
     */
    @SuppressLint("SetTextI18n")
    private fun runPreprocessChain(isWindowing: Boolean) {
        val ctx = context ?: return

        val assetName = if (binding.rbData610.isChecked) "Data610.bin" else "Data622.raw"
        val w = binding.etWidth.text.toString().toIntOrNull() ?: 1112
        val h = binding.etHeight.text.toString().toIntOrNull() ?: 1740
        val bitDepth = if (binding.rb16bit.isChecked) 16 else 8
        val isBigEndian = binding.rbBigEndian.isChecked
        val slope = binding.etSlope.text.toString().toDoubleOrNull() ?: 1.0
        val intercept = binding.etIntercept.text.toString().toDoubleOrNull() ?: -1024.0

        val steps = mutableListOf<PreprocessStep>()
        if (binding.cbTailor.isChecked) steps.add(PreprocessStep(Op.TAILOR, listOf(40000.0, 1.0, 25.0, 10.0)))
        if (binding.cbInvert.isChecked) steps.add(PreprocessStep(Op.INVERT_LUT))
        if (binding.cbHu.isChecked) steps.add(PreprocessStep(Op.HU_CONVERT, listOf(slope, intercept)))
        if (binding.cbGaussian.isChecked) steps.add(PreprocessStep(Op.GAUSSIAN, listOf(binding.etGaussK.text.toString().toDoubleOrNull() ?: 5.0, 0.0)))
        if (binding.cbMedian.isChecked) steps.add(PreprocessStep(Op.MEDIAN, listOf(binding.etMedianK.text.toString().toDoubleOrNull() ?: 3.0)))
        if (binding.cbBilateral.isChecked) steps.add(PreprocessStep(Op.BILATERAL, listOf(binding.etBilateralD.text.toString().toDoubleOrNull() ?: 5.0, 50.0, 50.0)))
        if (binding.cbFft.isChecked) steps.add(PreprocessStep(Op.FFT, listOf(binding.etFftR.text.toString().toDoubleOrNull() ?: 300.0)))
        if (binding.cbResample.isChecked) steps.add(PreprocessStep(Op.RESAMPLE_SIZE, listOf(binding.etResW.text.toString().toDoubleOrNull() ?: 1112.0, binding.etResH.text.toString().toDoubleOrNull() ?: 1740.0, 0.0)))
        if (binding.cbEqualize.isChecked) steps.add(PreprocessStep(Op.GLOBAL_EQUALIZE))
        if (binding.cbClahe.isChecked) steps.add(PreprocessStep(Op.CLAHE, listOf(binding.etClaheClip.text.toString().toDoubleOrNull() ?: 2.0, 8.0, 8.0)))
        if (binding.cbStretch.isChecked) steps.add(PreprocessStep(Op.CONTRAST_STRETCH))

        val btn = if (isWindowing) binding.btnWindowing else binding.btnRun
        btn.isEnabled = false
        binding.tvInfo.text = if (isWindowing) "Computing Windowing Comparison..." else "Loading & Displaying Preprocess..."

        val windowMethodIndex = if (isWindowing) {
            when {
                binding.rbWinNone.isChecked -> -1
                binding.rbWinDefault.isChecked -> 0
                binding.rbWin72.isChecked -> 1
                binding.rbWinBimodal.isChecked -> 2
                binding.rbWinAdaptive.isChecked -> 3
                binding.rbWinHistType.isChecked -> 4
                binding.rbWinMinMax.isChecked -> 5
                else -> -1
            }
        } else -1

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                if (isWindowing) {
                    // 1. 获取调窗前 (None/Min-Max)
                    val resBefore = withContext(Dispatchers.IO) {
                        MedicalCTPreprocess.process(ctx, assetName, w, h, bitDepth, isBigEndian, false, steps, -1)
                    }
                    // 2. 获取调窗后
                    val resAfter = withContext(Dispatchers.IO) {
                        MedicalCTPreprocess.process(ctx, assetName, w, h, bitDepth, isBigEndian, false, steps, windowMethodIndex)
                    }
                    if (_binding == null) return@launch
                    binding.ivBefore.setImageBitmap(resBefore.bitmap)
                    binding.ivAfter.setImageBitmap(resAfter.bitmap)
                    val methodName = if (windowMethodIndex == -1) "None" else WindowMethod.values()[windowMethodIndex].displayName
                    binding.tvInfo.text = "Comparison Ready. Method: $methodName\nRange: [${resAfter.minVal}, ${resAfter.maxVal}]"
                    binding.tvSummary.text = generateSummary(steps, true, methodName)
                } else {
                    // 仅预处理显示 - 固定在左侧 (Left/Before)
                    val result = withContext(Dispatchers.IO) {
                        MedicalCTPreprocess.process(ctx, assetName, w, h, bitDepth, isBigEndian, false, steps, -1)
                    }
                    if (_binding == null) return@launch
                    binding.ivBefore.setImageBitmap(result.bitmap)
                    binding.ivAfter.setImageDrawable(null)
                    binding.tvInfo.text = "Preprocess Only. Range: [${result.minVal}, ${result.maxVal}]"
                    binding.tvSummary.text = generateSummary(steps, false, "")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Process chain failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                btn.isEnabled = true
            }
        }
    }

    private fun generateSummary(steps: List<PreprocessStep>, isWin: Boolean, method: String): String = buildString {
        appendLine(if (isWin) "【调窗前后类比分析】" else "【预处理操作总结】")
        if (steps.isEmpty()) appendLine("- 基础映射：展示原始或最简处理后的图像。")
        steps.forEach { step ->
            when (step.op) {
                Op.TAILOR -> appendLine("- 图片裁剪：自动定位主体并旋转，去除无效背景。")
                Op.INVERT_LUT -> appendLine("- Invert LUTs：色度反转，改变图像极性。")
                Op.HU_CONVERT -> appendLine("- HU校正：还原物理密度值。")
                Op.BILATERAL -> appendLine("- 双边去噪：保边平滑，提升信噪比。")
                Op.CLAHE -> appendLine("- CLAHE：局部对比度增强。")
                else -> appendLine("- ${step.op.displayName}")
            }
        }
        if (isWin) appendLine("- 调窗算法 ($method)：左图为基础线性映射，右图为应用算法后的诊断增强效果。")
    }

    private fun setupKeyboardDismiss() {
        val editorListener = TextView.OnEditorActionListener { v, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) { v.clearFocus(); hideKeyboard(); true } else false
        }
        listOf(binding.etWidth, binding.etHeight, binding.etSlope, binding.etIntercept, binding.etGaussK,
            binding.etMedianK, binding.etBilateralD, binding.etFftR, binding.etResW, binding.etResH, binding.etClaheClip)
            .forEach { it.setOnEditorActionListener(editorListener) }
        binding.scrollRoot.setOnClickListener { hideKeyboard(); binding.scrollRoot.requestFocus() }
    }

    private fun hideKeyboard() {
        val ctx = context ?: return
        val imm = ctx.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager ?: return
        imm.hideSoftInputFromWindow((activity?.currentFocus ?: binding.root).windowToken, 0)
    }

    override fun onDestroyView() { super.onDestroyView(); _binding = null }

    companion object { private const val TAG = "CTPreprocessFragment" }
}
