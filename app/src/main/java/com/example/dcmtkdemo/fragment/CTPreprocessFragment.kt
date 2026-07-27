package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.SeekBar
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.ArrayAdapter
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
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * CTPreprocessFragment - Optimized Version
 * Reorganized by operation categories and added side-by-side windowing comparison.
 */
class CTPreprocessFragment : Fragment() {

    private var _binding: FragmentCtPreprocessBinding? = null
    private val binding get() = _binding!!

    // ---- 动态调窗缓存 ----
    /** 缓存上一次预处理使用的 raw buffer，避免 SeekBar 拖动时重复读 asset */
    private var cachedRawBuffer: ByteArray? = null
    private var cachedWidth: Int = 0
    private var cachedHeight: Int = 0
    private var cachedBitDepth: Int = 16
    private var cachedBigEndian: Boolean = true
    private var cachedSteps: List<PreprocessStep> = emptyList()
    /** 动态调窗防抖：SeekBar 高频回调合并为一次 native 调用 */
    private val debounceHandler = Handler(Looper.getMainLooper())
    private var debounceRunnable: Runnable? = null
    private var dynamicWindowJob: Job? = null

    /** 窗位 SeekBar 映射：max=2000 → HU [-1000, +1000] */
    private fun mapCenter(progress: Int): Double = (progress - 1000).toDouble()
    /** 窗宽 SeekBar 映射：max=4000 → [1, 4000] */
    private fun mapWidth(progress: Int): Double = maxOf(1.0, progress.toDouble())
    /** 反向映射：HU → SeekBar progress */
    private fun centerToProgress(center: Double): Int = (center + 1000).toInt().coerceIn(0, 2000)
    private fun widthToProgress(width: Double): Int = width.toInt().coerceIn(0, 4000)

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

        // 7) Statistics Export
        binding.btnExportStats.setOnClickListener {
            runExportStatistics()
        }

        setupAssetSpinner()
        setupWindowMethodRadioLogic()
        // P1-8: 8-bit 单选时禁用 HU/FFT 等不适用的算子，避免产生无意义或全黑结果
        setupBitDepthLogic()
        setupDynamicWindow()
        setupKeyboardDismiss()
    }

    /**
     * 6.1) 窗宽窗位动态调节。
     *
     * 核心原理（参考 CSDN 博客 u013598963/121023205）：
     *  - 窗位 L(=C) 控制映射中心：像素值 < L-W/2 → 黑(0)，> L+W/2 → 白(255)
     *  - 窗宽 W(=WW) 控制映射范围：W 越窄，对比度越高
     *  - 公式：output = saturate_cast<uchar>(255 * (pixel - (L - W/2)) / W)
     *
     * 交互策略：
     *  1) 用户先 LOAD & DISPLAY 或 调窗 生成一次预处理结果，缓存 raw buffer + steps；
     *  2) 勾选「启用动态调窗」后，SeekBar 拖动时复用缓存数据，仅重做 (L,W)→8bit 映射；
     *  3) 防抖 150ms，避免高频回调导致 native 调用堆积。
     */
    private fun setupDynamicWindow() {
        // 初始禁用 SeekBar，需先勾选启用
        val seekBars = listOf(binding.sbWindowCenter, binding.sbWindowWidth)
        seekBars.forEach { it.isEnabled = false }

        binding.cbDynamicWindow.setOnCheckedChangeListener { _, isChecked ->
            seekBars.forEach { it.isEnabled = isChecked }
            if (isChecked) {
                // 勾选时如果还没缓存数据，提示用户先执行一次预处理
                if (cachedRawBuffer == null) {
                    binding.tvInfo.text = "请先点击 LOAD & DISPLAY 或 调窗 生成预处理数据，再拖动 SeekBar"
                } else {
                    applyDynamicWindow(debounceMs = 0)
                }
            }
        }

        // 窗位 SeekBar
        binding.sbWindowCenter.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                val center = mapCenter(progress)
                binding.tvWindowCenter.text = "%.0f".format(center)
                if (fromUser && binding.cbDynamicWindow.isChecked) applyDynamicWindow()
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        })

        // 窗宽 SeekBar
        binding.sbWindowWidth.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                val width = mapWidth(progress)
                binding.tvWindowWidth.text = "%.0f".format(width)
                if (fromUser && binding.cbDynamicWindow.isChecked) applyDynamicWindow()
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        })

        // 快捷预设按钮
        binding.btnPresetBrain.setOnClickListener { applyPreset(40.0, 80.0) }
        binding.btnPresetLung.setOnClickListener { applyPreset(-600.0, 1500.0) }
        binding.btnPresetBone.setOnClickListener { applyPreset(400.0, 1500.0) }
        binding.btnPresetSoft.setOnClickListener { applyPreset(40.0, 400.0) }
    }

    /**
     * 将预设 (C, W) 同步到 SeekBar 并触发动态调窗。
     */
    private fun applyPreset(center: Double, width: Double) {
        if (!binding.cbDynamicWindow.isChecked) {
            binding.cbDynamicWindow.isChecked = true
        }
        binding.sbWindowCenter.progress = centerToProgress(center)
        binding.sbWindowWidth.progress = widthToProgress(width)
        // onProgressChanged 会自动调用 applyDynamicWindow
    }

    /**
     * 防抖调用 [MedicalCTPreprocess.processWithCustomWindow]。
     *
     * SeekBar 高频回调时（每 pixel 拖动一次），不做每次 native 调用，
     * 而是延迟 [debounceMs] 后合并为一次。若在此期间又有新回调，取消旧任务。
     */
    private fun applyDynamicWindow(debounceMs: Long = 150L) {
        val raw = cachedRawBuffer ?: return
        val center = mapCenter(binding.sbWindowCenter.progress)
        val width = mapWidth(binding.sbWindowWidth.progress)

        // 取消之前 pending 的防抖任务
        debounceRunnable?.let { debounceHandler.removeCallbacks(it) }
        dynamicWindowJob?.cancel()

        if (debounceMs > 0) {
            val r = Runnable {
                doDynamicWindowNative(raw, center, width)
            }
            debounceRunnable = r
            debounceHandler.postDelayed(r, debounceMs)
        } else {
            doDynamicWindowNative(raw, center, width)
        }
    }

    /**
     * 在 IO 线程执行 native 动态调窗，结果回主线程更新 UI。
     */
    @SuppressLint("SetTextI18n")
    private fun doDynamicWindowNative(raw: ByteArray, center: Double, width: Double) {
        if (cachedWidth <= 0 || cachedHeight <= 0) return
        binding.tvInfo.text = "动态调窗: C=%.0f, W=%.0f ...".format(center, width)

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    MedicalCTPreprocess.processWithCustomWindow(
                        rawBuffer = raw,
                        width = cachedWidth,
                        height = cachedHeight,
                        bitDepth = cachedBitDepth,
                        bigEndian = cachedBigEndian,
                        isUint16 = true,
                        steps = cachedSteps,
                        windowCenter = center,
                        windowWidth = width
                    )
                }
                if (_binding == null) return@launch
                binding.ivAfter.setImageBitmap(result.bitmap)
                binding.tvInfo.text =
                    "动态调窗: C=%.0f, W=%.0f | HU Range: [%.1f, %.1f]".format(
                        center, width, result.minValD, result.maxValD
                    )
            } catch (e: Exception) {
                Log.e(TAG, "Dynamic window failed", e)
                if (_binding != null) binding.tvInfo.text = "动态调窗错误: ${e.message}"
            }
        }
    }

    /**
     * P1-8: 位深切换联动。
     *
     * 8-bit 数据范围 [0, 255] 无法承载 HU（[-1024, 3071]），HU 转换会截断/溢出；
     * FFT 在 8-bit 上也容易因数值精度不足产生伪影。因此当用户切到 8-bit 时：
     *  - 自动取消勾选 [cbHu]，并禁用 checkbox 与 Slope/Intercept 输入框；
     *  - 禁用 [cbFft]（视觉价值低，且容易引发全图暗化）；
     *  - 16-bit 切回时恢复 enable，用户可自由重新勾选。
     */
    private fun setupBitDepthLogic() {
        val onChanged = {
            val is8Bit = binding.rb8bit.isChecked
            binding.cbHu.isEnabled = !is8Bit
            binding.cbFft.isEnabled = !is8Bit
            binding.etSlope.isEnabled = !is8Bit
            binding.etIntercept.isEnabled = !is8Bit
            if (is8Bit) {
                if (binding.cbHu.isChecked) binding.cbHu.isChecked = false
                if (binding.cbFft.isChecked) binding.cbFft.isChecked = false
            }
        }
        binding.rb8bit.setOnClickListener { onChanged() }
        binding.rb16bit.setOnClickListener { onChanged() }
        // 初始以 16-bit 为默认，无需操作；保持初始态即可
    }

    /**
     * 7) 统计输出逻辑：从当前已加载的原始像素缓冲区导出 txt 文件。
     * 生成：pixel_array.txt, histogram.txt, smoothed_data.txt
     */
    private fun runExportStatistics() {
        val raw = cachedRawBuffer
        if (raw == null) {
            binding.tvInfo.text = "请先点击 LOAD & DISPLAY 加载数据后再导出统计"
            return
        }

        val w = cachedWidth
        val h = cachedHeight
        val bitDepth = cachedBitDepth
        val isBigEndian = cachedBigEndian

        binding.btnExportStats.isEnabled = false
        binding.tvInfo.text = "正在导出统计数据..."

        viewLifecycleOwner.lifecycleScope.launch {
            val status = withContext(Dispatchers.IO) {
                MedicalCTPreprocess.exportStatistics(
                    requireContext(),
                    raw, w, h, bitDepth, isBigEndian
                )
            }
            if (_binding == null) return@launch
            binding.tvInfo.text = status
            binding.btnExportStats.isEnabled = true
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
        } else {
            binding.tvInfo.text = "No .bin or .raw files found in assets!"
        }
    }

    private fun setupWindowMethodRadioLogic() {
        val radioButtons = listOf(
            binding.rbWinNone,
            binding.rbWinDefault,
            binding.rbWin72,
            binding.rbWinBimodal,
            binding.rbWinAdaptive,
            binding.rbWinHistType,
            binding.rbWinMinMax,
            binding.rbWinPeak
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
        val assetName = binding.spinnerAsset.selectedItem?.toString() ?: return
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
                binding.tvInfo.text =
                    "Standard Pipeline Done. (Output: ${result.outWidth}x${result.outHeight})"
                binding.tvSummary.text =
                    "【标准流水线一键操作】\n执行了官方标准流程：HU校正、双边降噪、重采样、CLAHE增强。该流程是医学图像处理的基准。"
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

        val assetName = binding.spinnerAsset.selectedItem?.toString() ?: return
        val w = binding.etWidth.text.toString().toIntOrNull() ?: 1112
        val h = binding.etHeight.text.toString().toIntOrNull() ?: 1740
        val bitDepth = if (binding.rb16bit.isChecked) 16 else 8
        val isBigEndian = binding.rbBigEndian.isChecked
        val slope = binding.etSlope.text.toString().toDoubleOrNull() ?: 1.0
        val intercept = binding.etIntercept.text.toString().toDoubleOrNull() ?: -1024.0

        val steps = mutableListOf<PreprocessStep>()
        if (binding.cbTailor.isChecked) steps.add(
            PreprocessStep(
                Op.TAILOR,
                listOf(40000.0, 1.0, 25.0, 10.0)
            )
        )
        if (binding.cbInvert.isChecked) steps.add(PreprocessStep(Op.INVERT_LUT))
        if (binding.cbHu.isChecked) steps.add(
            PreprocessStep(
                Op.HU_CONVERT,
                listOf(slope, intercept)
            )
        )
        // P1-fix (问题3): 算子顺序校验与自动排序
        val sortWarnings = validateAndSortSteps(steps)
        if (binding.cbGaussian.isChecked) steps.add(
            PreprocessStep(
                Op.GAUSSIAN,
                listOf(binding.etGaussK.text.toString().toDoubleOrNull() ?: 5.0, 0.0)
            )
        )
        if (binding.cbMedian.isChecked) steps.add(
            PreprocessStep(
                Op.MEDIAN,
                listOf(binding.etMedianK.text.toString().toDoubleOrNull() ?: 3.0)
            )
        )
        if (binding.cbBilateral.isChecked) steps.add(
            PreprocessStep(
                Op.BILATERAL,
                listOf(binding.etBilateralD.text.toString().toDoubleOrNull() ?: 5.0, 50.0, 50.0)
            )
        )
        if (binding.cbFft.isChecked) steps.add(
            PreprocessStep(
                Op.FFT,
                listOf(binding.etFftR.text.toString().toDoubleOrNull() ?: 300.0)
            )
        )
        if (binding.cbResample.isChecked) steps.add(
            PreprocessStep(
                Op.RESAMPLE_SIZE,
                listOf(
                    binding.etResW.text.toString().toDoubleOrNull() ?: 1112.0,
                    binding.etResH.text.toString().toDoubleOrNull() ?: 1740.0,
                    0.0
                )
            )
        )
        if (binding.cbEqualize.isChecked) steps.add(PreprocessStep(Op.GLOBAL_EQUALIZE))
        if (binding.cbClahe.isChecked) steps.add(
            PreprocessStep(
                Op.CLAHE,
                listOf(binding.etClaheClip.text.toString().toDoubleOrNull() ?: 2.0, 8.0, 8.0)
            )
        )
        if (binding.cbStretch.isChecked) steps.add(PreprocessStep(Op.CONTRAST_STRETCH))
        if (binding.cbSharpen.isChecked) steps.add(
            PreprocessStep(
                Op.FEATURE_SHARPEN,
                listOf(1.5, binding.etSharpenStrength.text.toString().toDoubleOrNull() ?: 0.6)
            )
        )

        val btn = if (isWindowing) binding.btnWindowing else binding.btnRun
        btn.isEnabled = false
        binding.tvInfo.text =
            if (isWindowing) "Computing Windowing Comparison..." else "Loading & Displaying Preprocess..."

        val windowMethodIndex = if (isWindowing) {
            when {
                binding.rbWinNone.isChecked -> -1
                binding.rbWinDefault.isChecked -> 0
                binding.rbWin72.isChecked -> 1
                binding.rbWinBimodal.isChecked -> 2
                binding.rbWinAdaptive.isChecked -> 3
                binding.rbWinHistType.isChecked -> 4
                binding.rbWinMinMax.isChecked -> 5
                binding.rbWinPeak.isChecked -> 6
                else -> -1
            }
        } else -1

        // P1-fix (问题3): 如果自动排序产生了警告，在 info 中提示
        if (sortWarnings.isNotEmpty()) {
            binding.tvInfo.text = "⚠ 顺序已自动调整: ${sortWarnings.joinToString("; ")}"
        }

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                // 1) 读取 Asset 数据并缓存
                val bytes = withContext(Dispatchers.IO) {
                    ctx.assets.open(assetName).use { it.readBytes() }
                }
                cachedRawBuffer = bytes
                cachedWidth = w
                cachedHeight = h
                cachedBitDepth = bitDepth
                cachedBigEndian = isBigEndian
                cachedSteps = steps.toList()

                if (isWindowing) {
                    // 优化：以前对同一份 raw 调两次 process()，重负载跑两遍。
                    // 现在改用 processCompareWindows：重负载（裁剪/HU/去噪/重采样/增强）
                    // 只跑一次，然后对每个调窗方法只重做 8-bit 映射。
                    val results = withContext(Dispatchers.IO) {
                        MedicalCTPreprocess.processCompareWindows(
                            rawBuffer = bytes,
                            width = w,
                            height = h,
                            bitDepth = bitDepth,
                            bigEndian = isBigEndian,
                            isUint16 = true,
                            steps = steps,
                            // 第 0 个是"调窗前"（min-max），第 1 个是选中的调窗方法
                            windowMethods = listOf(-1, windowMethodIndex)
                        )
                    }
                    if (_binding == null) return@launch
                    binding.ivBefore.setImageBitmap(results[0].bitmap)
                    binding.ivAfter.setImageBitmap(results[1].bitmap)
                    val methodName =
                        when (windowMethodIndex) {
                            -1 -> "None"
                            6 -> "Peak Area Auto (Custom)"
                            else -> WindowMethod.values()[windowMethodIndex].displayName
                        }
                    binding.tvInfo.text =
                        "Comparison Ready. Method: $methodName"
                    binding.tvSummary.text = generateSummary(steps, true, methodName)
                } else {
                    // 仅预处理显示 - 固定在左侧 (Left/Before)
                    val result = withContext(Dispatchers.IO) {
                        MedicalCTPreprocess.process(
                            rawBuffer = bytes,
                            width = w,
                            height = h,
                            bitDepth = bitDepth,
                            bigEndian = isBigEndian,
                            isUint16 = true,
                            steps = steps,
                            windowMethod = -1
                        )
                    }
                    if (_binding == null) return@launch
                    binding.ivBefore.setImageBitmap(result.bitmap)
                    binding.ivAfter.setImageDrawable(null)
                    // P1-6: 展示浮点精度的 HU 范围（不再截断为 int）
                    binding.tvInfo.text =
                        "Preprocess Only. Range: [${"%.2f".format(result.minValD)}, ${"%.2f".format(result.maxValD)}] HU"
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

    /**
     * P1-fix (问题3): 算子顺序校验与自动排序。
     *
     * 规则：
     * 1. HU_CONVERT 必须在 INVERT_LUT 之前 —— 反转 raw 像素值后再做 HU 校正会失去物理意义。
     * 2. HU_CONVERT 应在 8-bit 专用算子 (GLOBAL_EQUALIZE / CLAHE) 之前 ——
     *    虽然 C++ 层已做 HU 域精度保留，但从语义上 HU 校正应先于增强。
     *
     * @return 警告消息列表（空列表表示无需调整）
     */
    private fun validateAndSortSteps(steps: MutableList<PreprocessStep>): List<String> {
        val warnings = mutableListOf<String>()

        val huIdx = steps.indexOfFirst { it.op == Op.HU_CONVERT }
        val invertIdx = steps.indexOfFirst { it.op == Op.INVERT_LUT }

        // 规则1: HU_CONVERT 必须在 INVERT_LUT 之前
        if (huIdx >= 0 && invertIdx >= 0 && huIdx > invertIdx) {
            val huStep = steps.removeAt(huIdx)
            steps.add(invertIdx, huStep)
            warnings.add("HU校正已移至Invert LUTs之前")
        }

        // 规则2: HU_CONVERT 应在 GLOBAL_EQUALIZE / CLAHE 之前
        if (huIdx >= 0 || invertIdx >= 0) {
            val newHuIdx = steps.indexOfFirst { it.op == Op.HU_CONVERT }
            if (newHuIdx >= 0) {
                listOf(Op.GLOBAL_EQUALIZE, Op.CLAHE).forEach { enhOp ->
                    val enhIdx = steps.indexOfFirst { it.op == enhOp }
                    if (enhIdx >= 0 && enhIdx < newHuIdx) {
                        val enhStep = steps.removeAt(enhIdx)
                        steps.add(newHuIdx, enhStep)
                        warnings.add("${enhOp.displayName}已移至HU校正之后")
                    }
                }
            }
        }

        return warnings
    }

    private fun generateSummary(
        steps: List<PreprocessStep>,
        isWin: Boolean,
        method: String
    ): String = buildString {
        appendLine(if (isWin) "【调窗前后类比分析】" else "【预处理操作总结】")
        if (steps.isEmpty()) appendLine("- 基础映射：展示原始或最简处理后的图像。")
        steps.forEach { step ->
            val p = step.params
            when (step.op) {
                Op.TAILOR -> {
                    val area = p.getOrNull(0)?.toInt() ?: 40000
                    appendLine("- 图片裁剪：自动定位主体并旋转，去除无效背景 (MinArea: $area)。")
                }
                Op.INVERT_LUT -> appendLine("- Invert LUTs：色度反转，改变图像极性。")
                Op.HU_CONVERT -> {
                    val s = p.getOrNull(0) ?: 1.0
                    val i = p.getOrNull(1) ?: -1024.0
                    appendLine("- HU校正：还原物理密度值 (Slope: $s, Intercept: $i)。")
                }
                Op.BILATERAL -> {
                    val d = p.getOrNull(0)?.toInt() ?: 5
                    val sc = p.getOrNull(1)?.toInt() ?: 50
                    val ss = p.getOrNull(2)?.toInt() ?: 50
                    appendLine("- 双边去噪：保边平滑，提升信噪比 (d: $d, Color: $sc, Space: $ss)。")
                }
                Op.CLAHE -> {
                    val clip = p.getOrNull(0) ?: 2.0
                    val gx = p.getOrNull(1)?.toInt() ?: 8
                    val gy = p.getOrNull(2)?.toInt() ?: 8
                    appendLine("- CLAHE：局部对比度增强 (Clip: $clip, Grid: ${gx}x${gy})。")
                }
                Op.FEATURE_SHARPEN -> {
                    val sigma = p.getOrNull(0) ?: 1.5
                    val strength = p.getOrNull(1) ?: 0.6
                    appendLine("- 特征锐化：针对骨皮质和骨小梁的USM增强 (Sigma: $sigma, Strength: $strength)。")
                }
                Op.GAUSSIAN -> {
                    val k = p.getOrNull(0)?.toInt() ?: 5
                    appendLine("- 高斯滤波：平滑图像 (Kernel: $k)。")
                }
                Op.MEDIAN -> {
                    val k = p.getOrNull(0)?.toInt() ?: 3
                    appendLine("- 中值滤波：去除椒盐噪声 (Kernel: $k)。")
                }
                Op.FFT -> {
                    val r = p.getOrNull(0)?.toInt() ?: 300
                    appendLine("- 频域FFT：滤除周期性条纹 (Radius: $r)。")
                }
                Op.RESAMPLE_SIZE -> {
                    val w = p.getOrNull(0)?.toInt() ?: 512
                    val h = p.getOrNull(1)?.toInt() ?: 512
                    appendLine("- 重采样：调整图像分辨率 (Size: ${w}x${h})。")
                }
                Op.GLOBAL_EQUALIZE -> appendLine("- 全局均衡化：直方图均衡增强。")
                Op.CONTRAST_STRETCH -> appendLine("- 对比度拉伸：线性灰度拉伸。")
                else -> appendLine("- ${step.op.displayName}${if (p.isNotEmpty()) " (参数: ${p.joinToString(", ")})" else ""}")
            }
        }
        if (isWin) {
            appendLine("- 调窗算法 ($method)：左图为基础线性映射，右图为应用算法后的诊断增强效果。")
            if (method.contains("Peak Area")) {
                appendLine("  [算法详情] 基于直方图面积最大波峰识别，结合高斯平滑与二阶导数边缘检测，实现软组织/病灶自适应增强。")
            }
        }
    }

    private fun setupKeyboardDismiss() {
        val editorListener = TextView.OnEditorActionListener { v, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                v.clearFocus(); hideKeyboard(); true
            } else false
        }
        listOf(
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
            binding.etClaheClip,
            binding.etSharpenStrength
        )
            .forEach { it.setOnEditorActionListener(editorListener) }
        binding.scrollRoot.setOnClickListener { hideKeyboard(); binding.scrollRoot.requestFocus() }
    }

    private fun hideKeyboard() {
        val ctx = context ?: return
        val imm =
            ctx.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager ?: return
        imm.hideSoftInputFromWindow((activity?.currentFocus ?: binding.root).windowToken, 0)
    }

    override fun onDestroyView() {
        super.onDestroyView(); _binding = null
    }

    companion object {
        private const val TAG = "CTPreprocessFragment"
    }
}
