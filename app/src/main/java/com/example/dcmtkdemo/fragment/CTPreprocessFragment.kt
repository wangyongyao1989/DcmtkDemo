package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.graphics.Bitmap
import android.graphics.drawable.BitmapDrawable
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Toast
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
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * CTPreprocessFragment - Optimized Version
 * Supports configurable pipeline steps and Write DICOM.
 */
class CTPreprocessFragment : Fragment() {

    private var _binding: FragmentCtPreprocessBinding? = null
    private val binding get() = _binding!!

    // 缓存当前处理参数，用于写入 DCM 时重新提取像素
    private var cachedRawBuffer: ByteArray? = null
    private var cachedWidth: Int = 0
    private var cachedHeight: Int = 0
    private var cachedBitDepth: Int = 16
    private var cachedBigEndian: Boolean = true

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

        binding.btnCustomAdjustment.setOnClickListener {
            runCustomPipeline()
        }

        binding.btnWriteDcm.setOnClickListener {
            runSaveDcmFile()
        }

        binding.btnTestPostProcess.setOnClickListener {
            runPostProcessTest()
        }

        binding.btnTestRotation.setOnClickListener {
            runRotationTest()
        }

        binding.btnTestFineGrained.setOnClickListener {
            runFineGrainedTest()
        }

        setupAssetSpinner()
    }

    /**
     * 测试后处理接口 (processImage)
     */
    private fun runPostProcessTest() {
        val currentBitmap = (binding.ivAfter.drawable as? BitmapDrawable)?.bitmap ?: run {
            Toast.makeText(context, "请先执行预处理以获取图像", Toast.LENGTH_SHORT).show()
            return
        }

        val contrast = binding.sbContrast.progress.toDouble() // 0-100
        val brightness = binding.sbBrightness.progress.toDouble() // 0-100
        val sharpen = binding.sbPostSharpen.progress.toDouble() // 0-100
        val invert = binding.cbPostInvert.isChecked
        val falseColor = binding.cbPostFalseColor.isChecked
        val relief = binding.cbPostRelief.isChecked

        viewLifecycleOwner.lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                MedicalCTPreprocess.processImage(
                    currentBitmap, contrast, brightness, sharpen,
                    invert, falseColor, relief, 0.0, 100.0
                )
            }
            if (result != null) {
                binding.ivAfter.setImageBitmap(result)
                binding.tvInfo.text = "后处理完成: C=$contrast, B=$brightness, S=$sharpen"
            }
        }
    }

    /**
     * 测试图像旋转
     */
    private fun runRotationTest() {
        val currentBitmap = (binding.ivAfter.drawable as? BitmapDrawable)?.bitmap ?: return
        viewLifecycleOwner.lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                MedicalCTPreprocess.applyRotation(currentBitmap, 90.0)
            }
            if (result != null) {
                binding.ivAfter.setImageBitmap(result)
                binding.tvInfo.text = "旋转 90° 完成"
            }
        }
    }

    /**
     * 测试细粒度接口链式调用
     */
    private fun runFineGrainedTest() {
        val currentBitmap = (binding.ivAfter.drawable as? BitmapDrawable)?.bitmap ?: return
        val w = currentBitmap.width
        val h = currentBitmap.height

        viewLifecycleOwner.lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                // 1) 转灰度并获取 Native 地址
                val addr0 = MedicalCTPreprocess.convertToGrayScale(currentBitmap)
                if (addr0 == 0L) return@withContext null

                // 2) 链式处理 (每个方法返回一个新的 Mat 地址)
                val addr1 = MedicalCTPreprocess.appBrightnessContrast(addr0, 50.0, 60.0, 0.0, 100.0)
                val addr2 = MedicalCTPreprocess.applySharpen(addr1, 30.0, 0.0, 100.0)
                val addr3 = MedicalCTPreprocess.applyFalseColor(addr2, true)
                val addr4 = MedicalCTPreprocess.applyRotationMat(addr3, 90.0)

                // 3) 转回 Bitmap
                val newW = if (90.0 % 180.0 != 0.0) h else w
                val newH = if (90.0 % 180.0 != 0.0) w else h
                val bmp = MedicalCTPreprocess.convertMatToBitmap(addr4, newW, newH)

                // 4) 释放 Native 内存，防止泄漏
                listOf(addr0, addr1, addr2, addr3, addr4).forEach {
                    if (it != 0L) MedicalCTPreprocess.releaseMat(it)
                }

                bmp
            }
            if (result != null) {
                binding.ivAfter.setImageBitmap(result)
                binding.tvInfo.text = "细粒度链式处理完成 (Gray -> BC -> Sharpen -> FalseColor)"
            }
        }
    }

    /**
     * 执行全套最优预处理（封装自 MedicalCTPreprocess）
     */
    @SuppressLint("SetTextI18n")
    private fun runOptimalAdjustment() {
        val ctx = context ?: return
        val assetName = binding.spinnerAsset.selectedItem?.toString() ?: return
        val w = binding.etWidth.text.toString().toIntOrNull() ?: 1112
        val h = binding.etHeight.text.toString().toIntOrNull() ?: 1740
        val bitDepth = if (binding.rb16bit.isChecked) 16 else 8
        val isBigEndian = binding.rbBigEndian.isChecked

        binding.btnOptimalAdjustment.isEnabled = false
        binding.btnCustomAdjustment.isEnabled = false
        binding.btnWriteDcm.isEnabled = false
        binding.tvInfo.text = "正在执行最优调节..."

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val bytes = withContext(Dispatchers.IO) {
                    ctx.assets.open(assetName).use { it.readBytes() }
                }

                // 缓存参数，供后续 runSaveDcmFile 使用
                cachedRawBuffer = bytes
                cachedWidth = w
                cachedHeight = h
                cachedBitDepth = bitDepth
                cachedBigEndian = isBigEndian

                val results = withContext(Dispatchers.IO) {
                    MedicalCTPreprocess.runOptimalPipeline(
                        rawBuffer = bytes,
                        width = w,
                        height = h,
                        bitDepth = bitDepth,
                        bigEndian = isBigEndian,
                        isUint16 = true
                    )
                }
                if (_binding == null) return@launch

                binding.ivBefore.setImageBitmap(results[0].bitmap)
                binding.ivAfter.setImageBitmap(results[1].bitmap)
                binding.tvInfo.text = "处理完成. 模式: 最优调节 (Auto)"
                binding.tvSummary.text = generateSummary(
                    MedicalCTPreprocess.getLastAppliedSteps(),
                    true,
                    "Peak Area Auto"
                )
                binding.btnWriteDcm.isEnabled = true
            } catch (e: Exception) {
                Log.e(TAG, "Optimal pipeline failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                binding.btnOptimalAdjustment.isEnabled = true
                binding.btnCustomAdjustment.isEnabled = true
            }
        }
    }

    /**
     * 自定义流水线逻辑
     */
    @SuppressLint("SetTextI18n")
    private fun runCustomPipeline() {
        val ctx = context ?: return
        val assetName = binding.spinnerAsset.selectedItem?.toString() ?: return
        val w = binding.etWidth.text.toString().toIntOrNull() ?: 1112
        val h = binding.etHeight.text.toString().toIntOrNull() ?: 1740
        val bitDepth = if (binding.rb16bit.isChecked) 16 else 8
        val isBigEndian = binding.rbBigEndian.isChecked

        // 收集勾选的算子
        val selectedOps = mutableListOf<Op>()
        if (binding.cbHu.isChecked) selectedOps.add(Op.HU_CONVERT)
        if (binding.cbTailor.isChecked) selectedOps.add(Op.TAILOR)
        if (binding.cbInvert.isChecked) selectedOps.add(Op.INVERT_LUT)
        if (binding.cbBilateral.isChecked) selectedOps.add(Op.BILATERAL)
        if (binding.cbClahe.isChecked) selectedOps.add(Op.CLAHE)
        if (binding.cbSharpen.isChecked) selectedOps.add(Op.FEATURE_SHARPEN)

        // 使用 UI 当前输入的自定义参数
        val slope = binding.etSlope.text.toString().toDoubleOrNull() ?: 1.0
        val intercept = binding.etIntercept.text.toString().toDoubleOrNull() ?: -1024.0
        val dBilateral = binding.etBilateralD.text.toString().toDoubleOrNull() ?: 5.0
        val clipClahe = binding.etClaheClip.text.toString().toDoubleOrNull() ?: 3.0
        val strengthSharpen = binding.etSharpenStrength.text.toString().toDoubleOrNull() ?: 6.0

        val steps = mutableListOf<PreprocessStep>()
        selectedOps.forEach { op ->
            when (op) {
                Op.HU_CONVERT -> steps.add(PreprocessStep(op, listOf(slope, intercept)))
                Op.BILATERAL -> steps.add(PreprocessStep(op, listOf(dBilateral, 75.0, 75.0)))
                Op.CLAHE -> steps.add(PreprocessStep(op, listOf(clipClahe, 8.0, 8.0)))
                Op.TAILOR -> steps.add(PreprocessStep(op, listOf(40000.0, 1.0, 25.0, 10.0)))
                Op.FEATURE_SHARPEN -> steps.add(PreprocessStep(op, listOf(1.5, strengthSharpen)))
                Op.INVERT_LUT -> steps.add(PreprocessStep(op))
            }
        }
        MedicalCTPreprocess.validateAndSortSteps(steps)

        binding.btnOptimalAdjustment.isEnabled = false
        binding.btnCustomAdjustment.isEnabled = false
        binding.btnWriteDcm.isEnabled = false
        binding.tvInfo.text = "正在执行自定义流水线..."

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val bytes = withContext(Dispatchers.IO) {
                    ctx.assets.open(assetName).use { it.readBytes() }
                }

                // 缓存参数，供后续 runSaveDcmFile 使用 (确保写入的是最后一次执行的结果)
                cachedRawBuffer = bytes
                cachedWidth = w
                cachedHeight = h
                cachedBitDepth = bitDepth
                cachedBigEndian = isBigEndian

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

                binding.ivBefore.setImageBitmap(results[0].bitmap)
                binding.ivAfter.setImageBitmap(results[1].bitmap)
                binding.tvInfo.text = "处理完成. 模式: 自定义"
                binding.tvSummary.text = generateSummary(steps, true, "Peak Area Auto")
                binding.btnWriteDcm.isEnabled = true
            } catch (e: Exception) {
                Log.e(TAG, "Pipeline failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                binding.btnOptimalAdjustment.isEnabled = true
                binding.btnCustomAdjustment.isEnabled = true
            }
        }
    }

    /**
     * 将处理后的数据保存为 DICOM 文件
     */
    private fun runSaveDcmFile() {
        val raw = cachedRawBuffer ?: return
        val ctx = context ?: return

        binding.btnWriteDcm.isEnabled = false
        binding.tvInfo.text = "正在写入 DICOM 文件..."

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    // 1) 获取处理（如裁剪）后的 16-bit 原始像素（大端），并强制使用 Peak Area 算法获取调窗参数
                    val processed = MedicalCTPreprocess.getProcessedRawPixels(
                        raw, cachedWidth, cachedHeight, cachedBitDepth, cachedBigEndian, true,
                        windowMethod = 6
                    )

                    // 2) 构造文件名
                    val sdf = SimpleDateFormat("yyyyMMdd_HHmm", Locale.getDefault())
                    val fileName = "processed_${sdf.format(Date())}.dcm"
                    val outDir = File(ctx.filesDir, "dcm_out")
                    if (!outDir.exists()) outDir.mkdirs()
                    val dcmFile = File(outDir, fileName)
                    val dcmPath = dcmFile.absolutePath

                    // 3) 准备 DCM 元数据
                    val record = ScanRecord(
                        examineNo = System.currentTimeMillis(),
                        patientName = "CT_PREPROCESS_TEST",
                        patientAge = "030Y",
                        patientSex = "M",
                        toothPosition = "FULL_BODY"
                    )

                    // 4) 准备像素数据结构
                    val pixelDataNew = PixelDataNew(
                        rows = processed.height,
                        columns = processed.width,
                        data = processed.data,
                        largestImagePixelValue = processed.maxVal,
                        win_center = processed.windowCenter.toInt(),
                        win_width = processed.windowWidth.toInt(),
                        exposure_leve = 1000,
                        standardDeviation = 0.0
                    )

                    // 5) 调用 dcmtk 模块写入
                    val ok = DcmtkJni.writeDcmFile(record, pixelDataNew, dcmPath)
                    if (ok) dcmPath else null
                }

                if (_binding == null) return@launch
                if (result != null) {
                    binding.tvInfo.text = "DICOM 写入成功: $result"
                } else {
                    binding.tvInfo.text = "DICOM 写入失败"
                }
            } catch (e: Exception) {
                Log.e(TAG, "Save DCM failed", e)
                binding.tvInfo.text = "Error: ${e.message}"
            } finally {
                binding.btnWriteDcm.isEnabled = true
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

    private fun generateSummary(
        steps: List<PreprocessStep>,
        isWin: Boolean,
        method: String
    ): String = buildString {
        appendLine(if (isWin) "【调窗前后类比分析】" else "【预处理操作总结】")
        steps.forEach { step ->
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
