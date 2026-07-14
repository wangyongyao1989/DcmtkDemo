package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtk.DicomManager
import com.example.dcmtk.model.ScanRecord
import com.example.dcmtkdemo.databinding.FragmentFileCompareBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 对比测试 Fragment：用 DCMTK native 方法实现 DicomFileUtils.kt 的 4 个方法并展示结果，
 * 同时以文字列出 dcm4che3 版本的实现方式与异同点（静态对比）。
 */
class FileCompareFragment : Fragment() {

    private var binding: FragmentFileCompareBinding? = null
    private val dcmFiles = ArrayList<File>()

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        binding = FragmentFileCompareBinding.inflate(inflater, container, false)
        return binding!!.root
    }

    @SuppressLint("SetTextI18n")
    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        binding?.tvCompare?.text = COMPARE_TEXT

        binding?.btnRefresh?.setOnClickListener { refreshFileList() }
        binding?.btnLoadInfo?.setOnClickListener { runLoadInfo() }
        binding?.btnWindowSettings?.setOnClickListener { runWindowSettings() }
        binding?.btnBitmapDefault?.setOnClickListener { runBitmapDefault() }
        binding?.btnBitmapCustom?.setOnClickListener { runBitmapCustom() }
        binding?.btnWriteDcm?.setOnClickListener { runWriteDcm() }

        refreshFileList()
    }

    private fun refreshFileList() {
        val dir = requireContext().getExternalFilesDir(null) ?: return
        val files = dir.listFiles { _, name -> name.lowercase().endsWith(".dcm") }
            ?.sortedBy { it.name }
            ?: emptyList()
        dcmFiles.clear()
        dcmFiles.addAll(files)
        val names = files.map { it.name }
        binding?.spFiles?.adapter = ArrayAdapter(
            requireContext(), android.R.layout.simple_spinner_dropdown_item, names
        )
        if (names.isEmpty()) {
            Toast.makeText(context, "未找到 .dcm 文件：${dir.absolutePath}", Toast.LENGTH_LONG).show()
        }
    }

    private fun selectedFile(): File? {
        val pos = binding?.spFiles?.selectedItemPosition ?: -1
        return if (pos in dcmFiles.indices) dcmFiles[pos] else null
    }

    // ① loadDicomFileInfoEx
    // DicomManager.loadDicomFileInfoEx 内部已切换至 Dispatchers.IO 并捕获异常，
    // 此处直接在 lifecycleScope（Main）中调用即可，无需再包 withContext(Dispatchers.IO)。
    @SuppressLint("SetTextI18n")
    private fun runLoadInfo() {
        val file = selectedFile() ?: run {
            toast("请先选择 .dcm 文件"); return
        }
        binding?.tvLoadInfo?.text = "运行中..."
        viewLifecycleOwner.lifecycleScope.launch {
            val info = DicomManager.loadDicomFileInfoEx(file.absolutePath)
            val sb = StringBuilder()
            if (info == null) {
                sb.append("调用失败/返回 null")
            } else if (info.isEmpty()) {
                sb.append("返回空 map（可能加载失败）")
            } else {
                info.forEach { (k, v) -> sb.append("$k = $v\n") }
            }
            binding?.tvLoadInfo?.text = sb
        }
    }

    // ② readDicomWindowSettings
    @SuppressLint("SetTextI18n")
    private fun runWindowSettings() {
        val file = selectedFile() ?: run {
            toast("请先选择 .dcm 文件"); return
        }
        binding?.tvWindowSettings?.text = "运行中..."
        viewLifecycleOwner.lifecycleScope.launch {
            val sb = StringBuilder()
            val ws = DicomManager.readDicomWindowSettings(file.absolutePath)
            if (ws == null) {
                sb.append("调用失败/返回 null")
            } else {
                sb.append("smallestPixelValue = ${ws.smallestPixelValue}\n")
                sb.append("largestPixelValue  = ${ws.largestPixelValue}\n")
                sb.append("autoCalculatedWindow: center=${ws.autoCalculatedWindow.center}, width=${ws.autoCalculatedWindow.width}\n")
                sb.append("windows (${ws.windows.size}):\n")
                ws.windows.forEachIndexed { i, w ->
                    sb.append("  [$i] center=${w.center}, width=${w.width}, desc=${w.description ?: "无"}\n")
                }
                sb.append("firstAvailableWindow: center=${ws.firstAvailableWindow.center}, width=${ws.firstAvailableWindow.width}\n")
            }
            binding?.tvWindowSettings?.text = sb
        }
    }

    // ③ dicomFile2Bitmap 默认窗
    @SuppressLint("SetTextI18n")
    private fun runBitmapDefault() {
        val file = selectedFile() ?: run {
            toast("请先选择 .dcm 文件"); return
        }
        viewLifecycleOwner.lifecycleScope.launch {
            val bmp = DicomManager.dicomFile2Bitmap(file.absolutePath)
            if (bmp != null) binding?.ivBitmapDefault?.setImageBitmap(bmp)
            else toast("默认窗渲染失败")
        }
    }

    // ③ dicomFile2Bitmap 自定义窗
    @SuppressLint("SetTextI18n")
    private fun runBitmapCustom() {
        val file = selectedFile() ?: run {
            toast("请先选择 .dcm 文件"); return
        }
        val ww = binding?.etWw?.text?.toString()?.toDoubleOrNull()
        val wc = binding?.etWc?.text?.toString()?.toDoubleOrNull()
        if (ww == null || wc == null) {
            toast("请输入合法 WW/WC"); return
        }
        viewLifecycleOwner.lifecycleScope.launch {
            val bmp = DicomManager.dicomFile2Bitmap(file.absolutePath, ww, wc)
            if (bmp != null) binding?.ivBitmapCustom?.setImageBitmap(bmp)
            else toast("自定义窗渲染失败")
        }
    }

    // ④ writeDcmFile round-trip
    // DicomManager 的 suspend 方法内部已切换至 Dispatchers.IO 并捕获异常，
    // 此处直接在 lifecycleScope（Main）中调用；仅 raw 文件写入是 DicomManager 之外的
    // 文件 I/O，需单独用 withContext(Dispatchers.IO) 包裹。
    @SuppressLint("SetTextI18n")
    private fun runWriteDcm() {
        binding?.tvWriteDcm?.text = "运行中..."
        binding?.ivWriteDcm?.setImageDrawable(null)
        viewLifecycleOwner.lifecycleScope.launch {
            val sb = StringBuilder()
            try {
                val dir = requireContext().getExternalFilesDir(null)!!
                val w = 256
                val h = 256
                // 合成 16-bit 渐变 raw（小端）
                val raw = ByteArray(w * h * 2)
                var idx = 0
                for (y in 0 until h) {
                    for (x in 0 until w) {
                        val v = ((x + y) * 65535 / (w + h - 2)).coerceIn(0, 65535)
                        raw[idx++] = (v and 0xFF).toByte()
                        raw[idx++] = ((v shr 8) and 0xFF).toByte()
                    }
                }
                val rawFile = File(dir, "compare_synth.raw")
                withContext(Dispatchers.IO) {
                    rawFile.writeBytes(raw)
                }

                val record = ScanRecord(
                    examineNo = 12345,
                    patientName = "测试^患者",
                    patientAge = "030Y",
                    patientSex = "男",
                    toothPosition = "Tooth11"
                )
                val dcmFile = File(dir, "compare_synth.dcm")
                val px = DicomManager.writeDcmFile(
                    record, rawFile.absolutePath, dcmFile.absolutePath, w, h
                )
                if (px == null) {
                    sb.append("writeDcmFile 返回 null")
                } else {
                    sb.append("writeDcmFile 成功\n")
                    sb.append("  rows=${px.rows}, columns=${px.columns}\n")
                    sb.append("  win_width=${px.win_width}, win_center=${px.win_center}\n")
                    sb.append("  exposure_leve=${px.exposure_leve}, largest=${px.largestImagePixelValue}\n")
                    sb.append("  data.size=${px.data.size}\n")
                    sb.append("  dcm: ${dcmFile.absolutePath} (${dcmFile.length()} bytes)\n")

                    // 回读验证
                    val back = DicomManager.loadDicomFileInfoEx(dcmFile.absolutePath)
                    sb.append("\n[回读 loadDicomFileInfoEx]\n")
                    if (back.isNullOrEmpty()) sb.append("  回读失败\n")
                    else back.forEach { (k, v) -> sb.append("  $k = $v\n") }

                    // DicomManager 返回后协程已回到 Main，可直接更新 ImageView
                    val bmp = DicomManager.dicomFile2Bitmap(dcmFile.absolutePath)
                    if (bmp != null) {
                        binding?.ivWriteDcm?.setImageBitmap(bmp)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "runWriteDcm", e)
                sb.append("异常：${e.message}")
            }
            binding?.tvWriteDcm?.text = sb
        }
    }

    private fun toast(msg: String) {
        Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }

    companion object {
        private const val TAG = "FileCompareFragment"
        private const val COMPARE_TEXT = """【目的】用 DCMTK native 重新实现 DicomFileUtils.kt（dcm4che3）的 4 个方法，并对比异同。

① loadDicomFileInfo / loadDicomFileInfoEx
  dcm4che3: DicomInputStream.readDataset → Attributes.getString(Tag.X) 取各命名字段；
            StudyDate+StudyTime 拼成 "YYYY-MM-DD HH:MM:SS"；PatientSex M→男/F→女。
  native:   DcmFileFormat.loadFile → dataset->findAndGetOFString(DCM_X)；
            同样 10 个命名 key、同样的日期/性别映射。
  异同:     返回字段与格式完全一致；dcm4che 用 Attributes，native 用 DcmDataset。

② readDicomWindowSettings
  dcm4che3: Attributes.getDoubles(WindowCenter/WindowWidth) 多值配对，
            再 getDoubles/Sequence 取 VOI LUT Sequence 回退；返回 DicomWindowSettings。
  native:   findAndGetOFString + 按 '\' 拆分多值配对，findAndGetSequence 遍历 VOI LUT；
            返回扁平 map 由 Kotlin 组装为同一 DicomWindowSettings。
  异同:     配对逻辑、autoCalculatedWindow 计算、VOI LUT 回退均一致；
            native 多值用 '\' 字符串拆分，dcm4che 用 getDoubles。

③ dicomFile2Bitmap
  dcm4che3: DicomImageReader.applyLUTs(readRaster, 8) → Raster →
            RasterUtil.rasterToBitmap（按 TYPE_BYTE/USHORT/INT 分灰度/RGB565/ARGB）。
  native:   DicomImage.setWindow + getOutputData(8) → 8-bit 灰度 →
            逐像素展开为 RGBA8888 (R=G=B=v,A=255) → AndroidBitmap。
  异同:     均输出 8-bit 灰度图；DicomImage 与 applyLUTs 都自动处理 MONOCHROME1 反转；
            dcm4che 输出 Bitmap.Config 受 Raster 类型影响，native 统一 ARGB_8888。

④ writeDcmFile
  dcm4che3: Attributes.setTag + DicomOutputStream.writeDataset；
            ProcessPixelData.process 算 win_width/win_center 等；UIDUtils 生成 UID。
  native:   dataset->putAndInsertString/Uint16 + saveFile(EXS_LittleEndianExplicit)；
            native 内等价 ProcessPixelData（min/max 窗宽窗位）；dcmGenerateUniqueIdentifier 生成 UID。
  异同:     写入的 tag 集合、MONOCHROME1、CR SOPClass、SpecificCharacterSet=ISO_IR 192 一致；
            dcm4che 用 UIDUtils.createUID(root=1.2.123.234)，native 用 dcmGenerateUniqueIdentifier。

注：dcm4che3 非本项目依赖，此处为静态文字对比；native 方法可在上方实际运行验证。"""
    }
}
