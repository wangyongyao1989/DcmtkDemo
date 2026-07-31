package com.example.dcmtkdemo.fragment

import android.content.Context
import android.os.Bundle
import android.os.Environment
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.ArrayAdapter
import android.widget.TextView
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtkdemo.databinding.FragmentRawPixelDealBinding
import com.example.dcmtk.DicomManager
import com.example.dcmtk.model.ScanRecord
import com.example.rawpixeldeal.RawPixelDealVerify
import com.example.rawpixeldeal.RawPixelDealVerify.CtSeriesConfig
import com.example.rawpixeldeal.RawPixelDealVerify.PixelMeta
import com.example.rawpixeldeal.RawPixelDealVerify.SeriesWindowResult
import com.example.rawpixeldeal.xray.WindowMethod
import com.example.rawpixeldeal.xray.XrayConfig
import com.example.rawpixeldeal.xray.XrayPipeline
import com.example.rawpixeldeal.xray.XrayResult
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 1) 最小链路验证：[runVerify] 调用 [RawPixelDealVerify.verifyChain]，
 *    验证 Kotlin -> JNI -> OpenCV 通路；
 * 2) 原始像素数据展示：[runLoadAsset] 动态读取 assets 下 .bin/.raw 文件
 *    （项目惯例：16-bit raw，[runLoadAsset] 走 native 做归一化/CLAHE/裁剪，
 *    最终输出 Bitmap 给 ImageView 做人工筛查验证。
 * 3) CT 序列级处理管线：[runSeriesPipeline] 实现 PRD
 *    ct-opencv-raw-buffer-windowing-prd 的完整流程：
 *    raw -> HU -> OpenCV 优化 -> 自动裁剪 -> 序列级自适应调窗 -> 8-bit。
 */
class RawPixelDealFragment : Fragment() {

    private var _binding: FragmentRawPixelDealBinding? = null
    private val binding get() = _binding!!

    // 缓存 X-ray 管线结果，供 "WRITE DCM" 按钮读取
    private var lastXrayResult: XrayResult? = null

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

        setupAssetSpinners()

        binding.btnVerify.setOnClickListener {
            runVerify()
        }
        binding.btnLoadAsset.setOnClickListener {
            runLoadAsset()
        }
        binding.btnRunSeries.setOnClickListener {
            runSeriesPipeline()
        }

        // 4) X-ray 管线（ProcessPixelData-readme 5 步管线）
        binding.btnRunXrayPipeline.setOnClickListener {
            runXrayPipeline()
        }
        binding.btnWriteDcm.setOnClickListener {
            writeXrayDcm()
        }

        setupKeyboardDismiss()
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
            binding.etCropL,
            binding.etCropT,
            binding.etCropR,
            binding.etCropB,
            binding.etClip,
            binding.etTile,
            // CT 序列管线参数
            binding.etSlope,
            binding.etIntercept,
            binding.etBodyThr,
            binding.etN0,
            binding.etN1,
            binding.etStride,
            // v2 新增
            binding.etPLow,
            binding.etPHigh,
        )
        allEditTexts.forEach { it.setOnEditorActionListener(editorListener) }

        // 触摸 EditText 之外的区域 -> 根 NestedScrollView 抢焦点 -> EditText 失焦 -> 软键盘收起
        binding.scrollRoot.setOnClickListener {
            hideKeyboard()
            binding.scrollRoot.requestFocus()
        }
    }

    private fun setupAssetSpinners() {
        val ctx = context ?: return
        val assets = ctx.assets.list("") ?: emptyArray()
        val fileList = assets.filter { it.endsWith(".bin") || it.endsWith(".raw") }

        if (fileList.isNotEmpty()) {
            val adapter = ArrayAdapter(ctx, android.R.layout.simple_spinner_item, fileList)
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            binding.spinnerAsset.adapter = adapter
            binding.spinnerXrayAsset.adapter = adapter

            val seriesList = mutableListOf("All Assets")
            seriesList.addAll(fileList)
            val seriesAdapter = ArrayAdapter(ctx, android.R.layout.simple_spinner_item, seriesList)
            seriesAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            binding.spinnerSeries.adapter = seriesAdapter
        }
    }

    private fun hideKeyboard() {
        val ctx = context ?: return
        val imm = ctx.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
            ?: return
        val tokenOwner = activity?.currentFocus ?: binding.root
        imm.hideSoftInputFromWindow(tokenOwner.windowToken, 0)
    }

    // 1) 链路验证：保留原行为
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

    // 2) 加载 assets 下的 .bin -> 走 native OpenCV 处理 -> 在 ImageView 显示
    private fun runLoadAsset() {
        val ctx = context ?: return

        val assetName = binding.spinnerAsset.selectedItem?.toString() ?: return
        val w = binding.etWidth.text.toString().toIntOrNull()
        val h = binding.etHeight.text.toString().toIntOrNull()
        val bitDepth =
            if (binding.rb16bit.isChecked) 16 else 8
        val cl = binding.etCropL.text.toString().toIntOrNull() ?: 0
        val ct = binding.etCropT.text.toString().toIntOrNull() ?: 0
        val cr = binding.etCropR.text.toString().toIntOrNull() ?: 0
        val cb = binding.etCropB.text.toString().toIntOrNull() ?: 0
        val enableClahe = binding.cbClahe.isChecked
        val clip = binding.etClip.text.toString().toDoubleOrNull() ?: 2.0
        val tile = binding.etTile.text.toString().toIntOrNull() ?: 8

        if (w == null || h == null || w <= 0 || h <= 0) {
            Toast.makeText(ctx, "请输入合法的 W/H", Toast.LENGTH_SHORT).show()
            return
        }

        binding.btnLoadAsset.isEnabled = false
        binding.tvAssetInfo.text = "Loading $assetName ..."
        binding.ivRawImage.setImageDrawable(null)
        hideKeyboard() // 启动加载时顺便把键盘收掉，避免遮挡结果

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    RawPixelDealVerify.processAssetFromAssets(
                        context = ctx,
                        assetName = assetName,
                        srcWidth = w,
                        srcHeight = h,
                        bitDepth = bitDepth,
                        cropLeft = cl,
                        cropTop = ct,
                        cropRight = cr,
                        cropBottom = cb,
                        enableClahe = enableClahe,
                        clipLimit = clip,
                        tileSize = tile,
                    )
                }
                // 回到主线程后操作 UI
                if (_binding == null) return@launch
                binding.ivRawImage.setImageBitmap(result.bitmap)
                binding.tvAssetInfo.text = buildString {
                    append("asset=${result.assetName}\n")
                    append("src=${result.srcWidth}x${result.srcHeight}@${result.bitDepth}bit\n")
                    append("crop LTRB=${result.cropLeft},${result.cropTop},${result.cropRight},${result.cropBottom}\n")
                    append("CLAHE=${result.enableClahe} clip=${result.clipLimit} tile=${result.tileSize}\n")
                    append("srcMin=${result.srcMin} srcMax=${result.srcMax}\n")
                    append("display=${result.outWidth}x${result.outHeight}")
                }
                // 加载完成后自动滚动到图像区域，避免用户还要手动翻页
                binding.ivRawImage.post {
                    _binding?.let { b ->
                        val target = b.ivRawImage.top -
                                b.scrollRoot.height / 3   // 留出 1/3 屏给图像上方的信息
                        b.scrollRoot.smoothScrollTo(0, target.coerceAtLeast(0))
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "runLoadAsset failed", e)
                binding.tvAssetInfo.text = "ERROR: ${e.message}"
                Toast.makeText(ctx, e.message ?: "load failed", Toast.LENGTH_LONG).show()
            } finally {
                _binding?.btnLoadAsset?.isEnabled = true
            }
        }
    }

    // 3) CT 序列级处理管线：raw -> HU -> 优化 -> 自动裁剪 -> 自适应调窗 -> 8-bit
    private fun runSeriesPipeline() {
        val ctx = context ?: return

        // 1) 解析参数
        val w = binding.etWidth.text.toString().toIntOrNull()
        val h = binding.etHeight.text.toString().toIntOrNull()
        val bitDepth = if (binding.rb16bit.isChecked) 16 else 8
        if (w == null || h == null || w <= 0 || h <= 0) {
            Toast.makeText(ctx, "请输入合法的 W/H", Toast.LENGTH_SHORT).show()
            return
        }
        val slope = binding.etSlope.text.toString().toDoubleOrNull() ?: 1.0
        val intercept = binding.etIntercept.text.toString().toDoubleOrNull() ?: -1024.0
        val bodyThr = binding.etBodyThr.text.toString().toFloatOrNull() ?: -600f
        // N0/N1 在 UI 上以 "x1000" 显示，转回真实比例
        val n0 = (binding.etN0.text.toString().toDoubleOrNull() ?: 1.5) / 1000.0
        val n1 = (binding.etN1.text.toString().toDoubleOrNull() ?: 1.5) / 1000.0
        val stride = binding.etStride.text.toString().toIntOrNull() ?: 1
        val enableBilateral = binding.cbBilateral.isChecked
        // v2 新增参数
        val pixelSigned = if (binding.rbPixelSigned.isChecked) 1 else 0
        val pLow = binding.etPLow.text.toString().toFloatOrNull() ?: 0.5f
        val pHigh = binding.etPHigh.text.toString().toFloatOrNull() ?: 99.5f
        val enableDisplayClahe = binding.cbDisplayClahe.isChecked

        val selected = binding.spinnerSeries.selectedItem?.toString() ?: return
        val assetNames: List<String> = if (selected == "All Assets") {
            val assets = context?.assets?.list("") ?: emptyArray()
            assets.filter { it.endsWith(".bin") || it.endsWith(".raw") }
        } else {
            listOf(selected)
        }
        val meta = PixelMeta(
            rows = h,
            cols = w,
            bitsAllocated = bitDepth,
            bitsStored = bitDepth,         // 资产文件按全位深处理
            pixelSigned = pixelSigned,      // v2: UI 可切换 signed/unsigned
            rescaleSlope = slope,
            rescaleIntercept = intercept,
            photometric = 0,                // 默认 MONOCHROME2
            littleEndian = true,
        )
        val config = CtSeriesConfig(
            bodyThreshold = bodyThr,
            n0 = n0,
            n1 = n1,
            histSampleStride = stride.coerceAtLeast(1),
            enableBilateral = enableBilateral,
            // v2 新增
            enableAutoPixelSign = true,
            gminPercentile = pLow,
            gmaxPercentile = pHigh,
            enableHistFallback = true,
            fallbackWindowCenter = 40.0,
            fallbackWindowWidth = 400.0,
            enableDisplayClahe = enableDisplayClahe,
            displayClaheClip = 2.0,
            displayClaheTile = 8,
            cropFirst = true,
        )

        binding.btnRunSeries.isEnabled = false
        binding.ivSeriesImage.setImageDrawable(null)
        binding.tvSeriesInfo.text = "Running pipeline on ${assetNames.joinToString()} ..."
        hideKeyboard()

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val result: SeriesWindowResult = withContext(Dispatchers.IO) {
                    RawPixelDealVerify.processCtSeriesFromAssets(
                        context = ctx,
                        assetNames = assetNames,
                        meta = meta,
                        config = config,
                    )
                }
                if (_binding == null) return@launch
                // 显示第一片
                val firstBmp = result.bitmaps.firstOrNull()
                if (firstBmp != null) {
                    binding.ivSeriesImage.setImageBitmap(firstBmp)
                }
                binding.tvSeriesInfo.text = formatSeriesResult(assetNames, result, config)
                // 自动滚到结果图
                binding.ivSeriesImage.post {
                    _binding?.let { b ->
                        val target = b.ivSeriesImage.top -
                                b.scrollRoot.height / 3
                        b.scrollRoot.smoothScrollTo(0, target.coerceAtLeast(0))
                    }
                }
                Toast.makeText(
                    ctx, "Series pipeline done. c=%.1f w=%.1f".format(
                        result.windowCenter, result.windowWidth
                    ), Toast.LENGTH_SHORT
                ).show()
            } catch (e: Exception) {
                Log.e(TAG, "runSeriesPipeline failed", e)
                binding.tvSeriesInfo.text = "ERROR: ${e.message}"
                Toast.makeText(
                    ctx, e.message ?: "series pipeline failed",
                    Toast.LENGTH_LONG
                ).show()
            } finally {
                _binding?.btnRunSeries?.isEnabled = true
            }
        }
    }

    private fun formatSeriesResult(
        assetNames: List<String>,
        r: SeriesWindowResult,
        cfg: CtSeriesConfig,
    ): String = buildString {
        append("series=${assetNames.joinToString()}\n")
        append("nSlices=${r.debug.sliceCount}\n")
        append("cropRect=(L=${r.cropLeft},T=${r.cropTop},W=${r.cropWidth},H=${r.cropHeight})\n")
        append(
            "Gmin=%.1f  Gmax=%.1f  H_bins=%.3f\n".format(
                r.debug.gMin, r.debug.gMax, r.debug.hBins
            )
        )
        append("T0=%.1f  T1=%.1f  B=%d\n".format(r.debug.t0, r.debug.t1, r.debug.b))
        append(
            "N0=%.4f  N1=%.4f  stride=${cfg.histSampleStride}\n".format(
                r.debug.n0, r.debug.n1
            )
        )
        append("c=%.2f  w=%.2f  (window in HU)\n".format(r.windowCenter, r.windowWidth))
        append("SV range: [${r.debug.srcMin}, ${r.debug.srcMax}]\n")
        // v2：百分位 + 直方图质量（增强可观测性）
        append(
            "Percentiles: pLow=%.2f  pHigh=%.2f\n".format(
                cfg.gminPercentile, cfg.gmaxPercentile
            )
        )
        // 统计直方图熵与 maxBinFrac（用 Kotlin 重算一次，便于 UI 展示）
        val hist = r.histogram
        if (hist.isNotEmpty()) {
            val total = hist.sum().toDouble()
            if (total > 0.0) {
                val logN = kotlin.math.ln(hist.size.toDouble())
                var entropy = 0.0
                var maxFrac = 0.0
                var peaks = 0
                var inPeak = false
                for (v in hist) {
                    val p = v / total
                    if (p > 0.0 && logN > 0.0) entropy -= p * kotlin.math.ln(p) / logN
                    if (p > maxFrac) maxFrac = p
                    val curPeak = p > 0.05
                    if (curPeak && !inPeak) peaks++
                    inPeak = curPeak
                }
                append(
                    "hist stats: entropy=%.3f maxBinFrac=%.3f peaks=%d\n".format(
                        entropy, maxFrac, peaks
                    )
                )
                if (maxFrac > 0.6 || entropy < 0.3) {
                    append("[!] histogram skewed -> fallback window likely used\n")
                }
            }
        }
        if (cfg.cropFirst) append("pipeline: cropFirst=1 (crop before denoise)\n")
        if (cfg.enableDisplayClahe) append(
            "display CLAHE: ON (clip=%.2f tile=%d)\n".format(
                cfg.displayClaheClip, cfg.displayClaheTile
            )
        )
        append("histogram bins (first 16): ${r.histogram.take(16).joinToString()}")
    }

    // 4) X-ray 管线：ProcessPixelData-readme 5 步管线
    //   raw → 16-bit → tailorImage → 直方图 → 调窗 → 显示/写 DCM
    private fun runXrayPipeline() {
        val ctx = context ?: return

        // 1) 解析 UI 参数：assets、窗方法
        val assetName = binding.spinnerXrayAsset.selectedItem?.toString() ?: return
        val method = when (binding.rgXrayWindow.checkedRadioButtonId) {
            binding.rbXrayMethodDefault.id -> WindowMethod.DEFAULT
            binding.rbXrayMethodCum72.id -> WindowMethod.CUMULATIVE_72
            binding.rbXrayMethodBimodal.id -> WindowMethod.BIMODAL_PEAK
            binding.rbXrayMethodAdaptive.id -> WindowMethod.ADAPTIVE_HISTOGRAM
            binding.rbXrayMethodMinmax.id -> WindowMethod.MIN_MAX
            else -> WindowMethod.MIN_MAX
        }
        Log.i(TAG, "runXrayPipeline: asset=$assetName method=${method.displayName}")

        binding.btnRunXrayPipeline.isEnabled = false
        binding.btnWriteDcm.isEnabled = false
        binding.ivXrayImage.setImageDrawable(null)
        binding.tvXrayInfo.text = "Running X-ray pipeline on $assetName ..."
        hideKeyboard()

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val cfg = XrayConfig(windowMethod = method)
                val result: XrayResult = withContext(Dispatchers.IO) {
                    // processXrayFromAssets 内部已经做了 "② tailorImage + ④ 直方图 +
                    // ⑤ 调窗 + ⑥ 8-bit 显示 + ⑦ 打包 PixelData"，与 readme §3 流程图对齐
                    XrayPipeline.processXrayFromAssets(
                        ctx = ctx,
                        assetNames = listOf(assetName),
                        config = cfg,
                    ) ?: throw IllegalStateException("XrayPipeline returned null")
                }
                if (_binding == null) return@launch
                lastXrayResult = result
                binding.ivXrayImage.setImageBitmap(result.bitmap)
                binding.tvXrayInfo.text = formatXrayResult(assetName, result, cfg)
                binding.btnWriteDcm.isEnabled = true
                // 自动滚到结果图
                binding.ivXrayImage.post {
                    _binding?.let { b ->
                        val target = b.ivXrayImage.top -
                                b.scrollRoot.height / 3
                        b.scrollRoot.smoothScrollTo(0, target.coerceAtLeast(0))
                    }
                }
                Toast.makeText(
                    ctx,
                    "X-ray done. c=%d w=%d (HU)".format(
                        result.debug.winCenter, result.debug.winWidth
                    ),
                    Toast.LENGTH_SHORT
                ).show()
            } catch (e: Exception) {
                Log.e(TAG, "runXrayPipeline failed", e)
                binding.tvXrayInfo.text = "ERROR: ${e.message}"
                Toast.makeText(
                    ctx, e.message ?: "xray pipeline failed",
                    Toast.LENGTH_LONG
                ).show()
            } finally {
                _binding?.btnRunXrayPipeline?.isEnabled = true
            }
        }
    }

    /**
     * 把 runXrayPipeline 的产物（PixelData）打包成 DICOM。
     *
     * 流程：
     *  1) 用 lastXrayResult.pixelData 转 dcmtk.model.PixelDataNew
     *  2) 拼一个最小可写的 ScanRecord（demo 用固定值）
     *  3) 调 DicomManager.writeDcmFile，输出到 app 私有目录
     *  4) 同时把写出的 .dcm 回读并渲染到 ImageView，让用户看到 "DICOM 渲染结果"，
     *     验证写入与读出环路。
     */
    private fun writeXrayDcm() {
        val ctx = context ?: return
        val result = lastXrayResult
        if (result == null) {
            Toast.makeText(ctx, "请先运行 RUN X-RAY PIPELINE", Toast.LENGTH_SHORT).show()
            return
        }

        binding.btnWriteDcm.isEnabled = false
        binding.tvXrayInfo.append("\n\nWriting DICOM ...")
        hideKeyboard()

        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val dcmPath = withContext(Dispatchers.IO) {
                    val outDir = ctx.getExternalFilesDir(Environment.DIRECTORY_PICTURES)
                        ?: ctx.filesDir
                    if (!outDir.exists()) outDir.mkdirs()
                    val ts = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US)
                        .format(Date())
                    File(outDir, "Xray_${ts}.dcm").absolutePath
                }

                val record = ScanRecord(
                    examineNo = System.currentTimeMillis(),
                    patientName = "Xray^Demo",
                    patientAge = "030Y",
                    patientSex = "O",
                    toothPosition = "BODY",
                )
                val pixelNew = XrayPipeline.toPixelDataNew(result.pixelData)
                Log.i(
                    TAG, "writeXrayDcm: -> $dcmPath " +
                            "(${pixelNew.columns}x${pixelNew.rows} " +
                            "c=${pixelNew.win_center} w=${pixelNew.win_width})"
                )

                val ok = DicomManager.writeDcmFile(record, pixelNew, dcmPath)
                if (!ok) throw IllegalStateException("writeDcmFile returned false")

                // 回读 + 用写出的窗位窗宽渲染
                val renderBmp = DicomManager.dicomFile2Bitmap(
                    dcmPath,
                    pixelNew.win_width.toDouble(),
                    pixelNew.win_center.toDouble(),
                )
                if (_binding == null) return@launch
                if (renderBmp != null) {
                    binding.ivXrayImage.setImageBitmap(renderBmp)
                }
                binding.tvXrayInfo.append(
                    "\nDICOM written: $dcmPath\n" +
                            "Render: ${if (renderBmp != null) "OK" else "NULL"}\n" +
                            "WC=${pixelNew.win_center} WW=${pixelNew.win_width} " +
                            "rows=${pixelNew.rows} cols=${pixelNew.columns}\n" +
                            "LargestPV=${pixelNew.largestImagePixelValue} " +
                            "Exposure=${pixelNew.exposure_leve} " +
                            "Stddev=%.2f".format(pixelNew.standardDeviation)
                )
                Toast.makeText(ctx, "DICOM saved:\n$dcmPath", Toast.LENGTH_LONG).show()
            } catch (e: Exception) {
                Log.e(TAG, "writeXrayDcm failed", e)
                binding.tvXrayInfo.append("\nERROR: ${e.message}")
                Toast.makeText(
                    ctx, e.message ?: "write dcm failed",
                    Toast.LENGTH_LONG
                ).show()
            } finally {
                _binding?.btnWriteDcm?.isEnabled = (lastXrayResult != null)
            }
        }
    }

    /**
     * 把 XrayResult 格式化为一段可读的诊断字符串。
     */
    private fun formatXrayResult(
        assetName: String,
        r: XrayResult,
        cfg: XrayConfig,
    ): String = buildString {
        append("asset=$assetName\n")
        append("window method: ${cfg.windowMethod.displayName}\n")
        append(
            "cropRect=(L=${r.cropRect[0]},T=${r.cropRect[1]}," +
                    "W=${r.cropRect[2]},H=${r.cropRect[3]})\n"
        )
        append("SV range=[${r.debug.minPixelValue}, ${r.debug.largestPixelValue}]\n")
        append("min_i(5%)=${r.debug.minI}  max_i(95%)=${r.debug.maxI}\n")
        append("WC=${r.debug.winCenter}  WW=${r.debug.winWidth}  (HU)\n")
        append(
            "exposure=${r.debug.exposureLevel}  stddev=%.2f\n".format(
                r.debug.standardDeviation
            )
        )
        append("rotateAngle=%.2f deg\n".format(r.debug.rotateAngle))
        append(
            "histogram total=${r.debug.histogramTotal}  " +
                    "clip=[${"%.2f".format(r.debug.cdfMinFraction)}," +
                    " ${"%.2f".format(r.debug.cdfMaxFraction)}]\n"
        )
        append("bitmap=${r.cropRect[2]}x${r.cropRect[3]}\n")
        append("hist(0..15)=${r.debug.histogramFirst16.joinToString()}\n")
        append("8u(0..15)=${r.debug.display8uFirst16.joinToString()}\n")
        append(
            "PixelDataNew: rows=${r.pixelData.height} cols=${r.pixelData.width} " +
                    "largestPV=${r.pixelData.largestImagePixelValue} " +
                    "bytes=${r.pixelData.data?.size ?: 0}"
        )
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    companion object {
        private const val TAG = "RawPixelDealFragment"
    }
}
