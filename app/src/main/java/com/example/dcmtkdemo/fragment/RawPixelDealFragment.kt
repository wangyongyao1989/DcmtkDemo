package com.example.dcmtkdemo.fragment

import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtkdemo.databinding.FragmentRawPixelDealBinding
import com.example.rawpixeldeal.RawPixelDealVerify
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * 1) 最小链路验证：[runVerify] 调用 [RawPixelDealVerify.verifyChain]，
 *    验证 Kotlin -> JNI -> OpenCV 通路；
 * 2) 原始像素数据展示：[runLoadAsset] 读取 assets 下 Data610.bin / Data622.bin
 *    （项目惯例：16-bit raw，[runLoadAsset] 走 native 做归一化/CLAHE/裁剪，
 *    最终输出 Bitmap 给 ImageView 做人工筛查验证。
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
            } catch (e: Exception) {
                Log.e(TAG, "runLoadAsset failed", e)
                binding.tvAssetInfo.text = "ERROR: ${e.message}"
                Toast.makeText(ctx, e.message ?: "load failed", Toast.LENGTH_LONG).show()
            } finally {
                _binding?.btnLoadAsset?.isEnabled = true
            }
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    companion object {
        private const val TAG = "RawPixelDealFragment"
    }
}
