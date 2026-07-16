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
import com.example.dcmtkdemo.databinding.FragmentRawPixelDealBinding
import com.example.rawpixeldeal.RawPixelDealVerify
import com.example.rawpixeldeal.RawPixelDealVerify.CtSeriesConfig
import com.example.rawpixeldeal.RawPixelDealVerify.PixelMeta
import com.example.rawpixeldeal.RawPixelDealVerify.SeriesWindowResult
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * 1) 最小链路验证：[runVerify] 调用 [RawPixelDealVerify.verifyChain]，
 *    验证 Kotlin -> JNI -> OpenCV 通路；
 * 2) 原始像素数据展示：[runLoadAsset] 读取 assets 下 Data610.bin / Data622.bin
 *    （项目惯例：16-bit raw，[runLoadAsset] 走 native 做归一化/CLAHE/裁剪，
 *    最终输出 Bitmap 给 ImageView 做人工筛查验证。
 * 3) CT 序列级处理管线：[runSeriesPipeline] 实现 PRD
 *    ct-opencv-raw-buffer-windowing-prd 的完整流程：
 *    raw -> HU -> OpenCV 优化 -> 自动裁剪 -> 序列级自适应调窗 -> 8-bit。
 */
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
        binding.btnLoadAsset.setOnClickListener {
            runLoadAsset()
        }
        binding.btnRunSeries.setOnClickListener {
            runSeriesPipeline()
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
        )
        allEditTexts.forEach { it.setOnEditorActionListener(editorListener) }

        // 触摸 EditText 之外的区域 -> 根 NestedScrollView 抢焦点 -> EditText 失焦 -> 软键盘收起
        binding.scrollRoot.setOnClickListener {
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

        val assetName =
            if (binding.rbData610.isChecked) "Data610.bin" else "Data622.bin"
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

        val assetNames: List<String> = when {
            binding.rbSeries610.isChecked -> listOf("Data610.bin")
            binding.rbSeries622.isChecked -> listOf("Data622.bin")
            binding.rbSeries610622.isChecked -> listOf("Data610.bin", "Data622.bin")
            else -> listOf("Data610.bin")
        }
        val meta = PixelMeta(
            rows = h,
            cols = w,
            bitsAllocated = bitDepth,
            bitsStored = bitDepth,         // 资产文件按全位深处理
            pixelSigned = if (bitDepth == 16) 1 else 0,  // 假设 16-bit 是 signed（典型 CT）
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
                Toast.makeText(ctx, "Series pipeline done. c=%.1f w=%.1f".format(
                    result.windowCenter, result.windowWidth), Toast.LENGTH_SHORT).show()
            } catch (e: Exception) {
                Log.e(TAG, "runSeriesPipeline failed", e)
                binding.tvSeriesInfo.text = "ERROR: ${e.message}"
                Toast.makeText(ctx, e.message ?: "series pipeline failed",
                    Toast.LENGTH_LONG).show()
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
        append("Gmin=%.1f  Gmax=%.1f  H_bins=%.3f\n".format(
            r.debug.gMin, r.debug.gMax, r.debug.hBins))
        append("T0=%.1f  T1=%.1f  B=%d\n".format(r.debug.t0, r.debug.t1, r.debug.b))
        append("N0=%.4f  N1=%.4f  stride=${cfg.histSampleStride}\n".format(
            r.debug.n0, r.debug.n1))
        append("c=%.2f  w=%.2f  (window in HU)\n".format(r.windowCenter, r.windowWidth))
        append("SV range: [${r.debug.srcMin}, ${r.debug.srcMax}]\n")
        append("histogram bins (first 16): ${r.histogram.take(16).joinToString()}")
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    companion object {
        private const val TAG = "RawPixelDealFragment"
    }
}
