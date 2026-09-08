package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.graphics.Bitmap
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.SeekBar
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtkdemo.databinding.FragmentCbctParseBinding
import com.example.dcmtkdemo.utils.FileUtil
import com.wangyao.cbctdeal.engine.CbctParseEngine
import com.wangyao.cbctdeal.jni.CbctJni
import com.wangyao.cbctdeal.jni.CbctVtkJni
import com.wangyao.cbctdeal.model.CbctVolumeHandle
import com.wangyao.cbctdeal.transfer.CbctFileTransfer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException

/**
 * CBCT DICOM 序列解析演示页：
 * 1) SAF 选择序列目录（或手动输入路径）；
 * 2) 调用 cbctdeal 模块解析（Native 多线程，进度回调）；
 * 3) MPR 浏览：横断面 / 冠状面 / 矢状面切换，位置与窗宽窗位实时调节；
 * 4) VTK 三维可视化：渲染载体切换（Bitmap 2D / VTK 3D），VTK 下支持
 *    VR 体绘制与 MPR 切面两种模式，手势旋转 / 缩放 / 平移。
 *
 * 说明：UI 与渲染功能完全解耦——本 Fragment 只负责交互与状态路由，
 * 解析能力内聚在 cbctdeal 模块（JNI + engine + transfer），
 * VTK 渲染能力同样内聚在 cbctdeal 模块（CbctVtkView + Native 渲染线程）。
 */
class CbctParseFragment : Fragment() {

    private var _binding: FragmentCbctParseBinding? = null
    private val binding get() = _binding!!

    /** 当前 Volume 句柄（Native 内存），页面销毁时释放 */
    private var volumeHandle: CbctVolumeHandle? = null

    /** 切面提取任务（滚动条快速拖动时取消旧任务，避免排队） */
    private var extractJob: Job? = null

    /** WC SeekBar 偏移：值域 [-1000, 3000] HU 映射到 [0, 4000] */
    private val wcOffset = 1000

    // 当前显示状态
    private var curPlane = PLANE_AXIAL
    private var curWw = 4000.0
    private var curWc = 600.0

    /** 渲染载体：true = VTK 3D（VR/MPR），false = Bitmap 2D（CPU 切面提取） */
    private var useVtk = false

    /** VTK 渲染子模式：VR 体绘制 / MPR 切面 */
    private var vtkMode = CbctVtkJni.MODE_VR

    private val dirPicker = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri == null) return@registerForActivityResult
        val ctx = context ?: return@registerForActivityResult
        try {
            // 持久化目录权限，避免下次进入需重新授权
            ctx.contentResolver.takePersistableUriPermission(
                uri,
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (e: SecurityException) {
            Log.w(TAG, "takePersistableUriPermission failed", e)
        }
        binding.tvInfo.text = "正在拷贝目录到私有存储..."
        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val dir = CbctFileTransfer.copyTreeUri(ctx, uri)
                if (_binding == null) return@launch
                binding.etPath.setText(dir.absolutePath)
                binding.tvInfo.text = "目录拷贝完成: ${dir.name}，可以开始解析"
            } catch (e: IOException) {
                Log.e(TAG, "copyTreeUri failed", e)
                if (_binding != null) binding.tvInfo.text = "拷贝失败: ${e.message}"
            }
        }
    }

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentCbctParseBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        binding.btnSelectDir.setOnClickListener { dirPicker.launch(null) }
        binding.btnParse.setOnClickListener { runParse() }
        binding.btnLoadAssets.setOnClickListener { loadNeckCtAssets() }
        binding.btnBoneWindow.setOnClickListener { applyWindow(4000.0, 600.0) }
        binding.btnDefaultWindow.setOnClickListener {
            volumeHandle?.let { applyWindow(it.meta.windowWidth, it.meta.windowCenter) }
        }
        binding.rgPlane.setOnCheckedChangeListener { _, _ -> onPlaneChanged() }
        binding.rgRenderer.setOnCheckedChangeListener { _, _ -> onRendererChanged() }
        binding.rgVtkMode.setOnCheckedChangeListener { _, _ -> onVtkModeChanged() }
        binding.btnResetCam.setOnClickListener { binding.vtkView.resetCamera() }

        // 解决 NestedScrollView 与 CbctVtkView 的滑动冲突，确保双指手势正常
        binding.vtkView.setOnTouchListener { v, event ->
            if (event.pointerCount >= 2 || event.actionMasked == MotionEvent.ACTION_POINTER_DOWN) {
                v.parent.requestDisallowInterceptTouchEvent(true)
            }
            false // 返回 false 以便 vtkView.onTouchEvent 能继续接收事件
        }

        val seekListener = object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (fromUser) onViewerParamsChanged()
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        }
        binding.sbPosition.setOnSeekBarChangeListener(seekListener)
        binding.sbWw.setOnSeekBarChangeListener(seekListener)
        binding.sbWc.setOnSeekBarChangeListener(seekListener)

        // 默认窗值展示
        updateWindowLabels()
    }

    /**
     * 一键加载内置测试序列：assets/neck_ct -> 私有存储 -> 自动解析。
     * 免 SAF 授权，用于真机快速验证解析与三维可视化全链路。
     */
    @SuppressLint("SetTextI18n")
    private fun loadNeckCtAssets() {
        val ctx = context ?: return
        binding.btnLoadAssets.isEnabled = false
        binding.tvInfo.text = "正在释放 assets/neck_ct 到私有存储..."
        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val dir = FileUtil.copyAssetDirToFiles(ctx, "neck_ct", "neck_ct")
                if (_binding == null) return@launch
                binding.etPath.setText(dir.absolutePath)
                binding.tvInfo.text = "assets 拷贝完成，开始解析..."
                runParse()
            } catch (e: IOException) {
                Log.e(TAG, "copy assets failed", e)
                if (_binding != null) binding.tvInfo.text = "assets 拷贝失败: ${e.message}"
            } finally {
                if (_binding != null) binding.btnLoadAssets.isEnabled = true
            }
        }
    }

    /** 解析序列（后台线程，进度实时上报） */
    @SuppressLint("SetTextI18n")
    private fun runParse() {
        val path = binding.etPath.text.toString().trim()
        if (path.isEmpty()) {
            Toast.makeText(context, "请先选择或输入 CBCT 序列目录路径", Toast.LENGTH_SHORT).show()
            return
        }
        val dir = File(path)
        if (!dir.exists() || !dir.isDirectory) {
            Toast.makeText(context, "目录不存在: $path", Toast.LENGTH_SHORT).show()
            return
        }

        // 释放旧资源，避免连续解析造成 Native 内存堆积。
        // 顺序关键：先销毁零拷贝引用旧 Volume 的 VTK 渲染器，再释放 Volume 本体
        _binding?.vtkView?.release()
        volumeHandle?.release()
        volumeHandle = null

        binding.btnParse.isEnabled = false
        binding.btnSelectDir.isEnabled = false
        binding.btnLoadAssets.isEnabled = false
        binding.progressCbct.visibility = View.VISIBLE
        binding.tvInfo.text = "正在解析序列..."

        viewLifecycleOwner.lifecycleScope.launch {
            var handle: CbctVolumeHandle? = null
            val appCtx = context?.applicationContext
            try {
                if (appCtx == null) {
                    handle?.release()
                    return@launch
                }
                // NonCancellable：保证 Native 解析完成后句柄一定被持有，
                // 避免 UI 提前销毁导致 Native 内存泄漏
                val result = withContext(NonCancellable) {
                    val h = CbctParseEngine.parse(appCtx, dir) { cur, total ->
                        activity?.runOnUiThread {
                            if (_binding != null && total > 0) {
                                binding.progressCbct.max = total
                                binding.progressCbct.progress = cur
                                binding.tvInfo.text = "解析中... $cur / $total"
                            }
                        }
                    }
                    // 引擎返回后立即在本协程内接管句柄（不会被中途取消丢弃）
                    handle = h
                    h
                }
                if (_binding == null) {
                    // 页面已销毁：直接释放
                    result?.release()
                    return@launch
                }
                if (result == null) {
                    binding.tvInfo.text = "解析失败：目录中没有有效的 DICOM 序列"
                    return@launch
                }
                volumeHandle = result
                binding.tvInfo.text = "解析完成 (${result.meta.elapsedMs} ms)"
                binding.tvCbctSummary.text = buildSummary(result)
                setupViewer(result)
                // VTK 渲染器挂载新 Volume（零拷贝，UI 状态自动重放）
                binding.vtkView.setVolume(result)
                refreshViewer()
            } catch (e: Exception) {
                Log.e(TAG, "parse failed", e)
                if (_binding != null) binding.tvInfo.text = "解析异常: ${e.message}"
                handle?.release()
            } finally {
                if (_binding != null) {
                    binding.btnParse.isEnabled = true
                    binding.btnSelectDir.isEnabled = true
                    binding.btnLoadAssets.isEnabled = true
                    binding.progressCbct.visibility = View.GONE
                }
            }
        }
    }

    /** 解析成功后初始化查看器（位置/窗宽窗位 SeekBar） */
    private fun setupViewer(handle: CbctVolumeHandle) {
        val meta = handle.meta
        // 位置：Axial -> depth；Coronal -> height；Sagittal -> width（取当前平面）
        setPositionRange()
        binding.sbPosition.progress = binding.sbPosition.max / 2
        // 窗宽窗位：优先文件自带值
        curWw = meta.windowWidth
        curWc = meta.windowCenter
        binding.sbWw.progress = curWw.toInt().coerceIn(1, binding.sbWw.max)
        binding.sbWc.progress = (curWc + wcOffset).toInt().coerceIn(0, binding.sbWc.max)
        updateWindowLabels()
    }

    /** 平面切换：刷新位置 SeekBar 范围并刷新查看器 */
    private fun onPlaneChanged() {
        curPlane = when (binding.rgPlane.checkedRadioButtonId) {
            binding.rbCoronal.id -> PLANE_CORONAL
            binding.rbSagittal.id -> PLANE_SAGITTAL
            else -> PLANE_AXIAL
        }
        setPositionRange()
        binding.sbPosition.progress = binding.sbPosition.max / 2
        refreshViewer()
    }

    /** 渲染载体切换（Bitmap 2D / VTK 3D）：互斥显示 + 控件可见性路由 */
    private fun onRendererChanged() {
        useVtk = binding.rgRenderer.checkedRadioButtonId == binding.rbVtk.id
        binding.vtkView.visibility = if (useVtk) View.VISIBLE else View.GONE
        binding.tvVtkHint.visibility = if (useVtk) View.VISIBLE else View.GONE
        binding.ivCbct.visibility = if (useVtk) View.GONE else View.VISIBLE
        updateViewerControlsVisibility()
        refreshViewer()
    }

    /** VTK 子模式切换（VR 体绘制 / MPR 切面） */
    private fun onVtkModeChanged() {
        vtkMode = if (binding.rgVtkMode.checkedRadioButtonId == binding.rbVtkMpr.id) {
            CbctVtkJni.MODE_MPR
        } else {
            CbctVtkJni.MODE_VR
        }
        binding.vtkView.setRenderMode(vtkMode)
        updateViewerControlsVisibility()
        refreshViewer()
    }

    /**
     * 切面相关控件可见性：
     * VR 模式下平面切换与层位置无意义（隐藏）；Bitmap / VTK-MPR 显示。
     */
    private fun updateViewerControlsVisibility() {
        val showSliceControls = !useVtk || vtkMode == CbctVtkJni.MODE_MPR
        binding.rgPlane.visibility = if (showSliceControls) View.VISIBLE else View.GONE
        binding.rowPosition.visibility = if (showSliceControls) View.VISIBLE else View.GONE
        binding.rgVtkMode.visibility = if (useVtk) View.VISIBLE else View.GONE
    }

    /** 按当前渲染载体刷新查看器（VTK -> 状态下发；Bitmap -> CPU 切面提取） */
    private fun refreshViewer() {
        if (volumeHandle == null) return
        if (useVtk) applyVtkState() else extractCurrentSlice()
    }

    /** 将当前渲染模式/平面/位置/窗宽窗位状态同步到 VTK 渲染器 */
    private fun applyVtkState() {
        binding.vtkView.setRenderMode(vtkMode)
        binding.vtkView.setPlane(curPlane, binding.sbPosition.progress)
        binding.vtkView.setWindowLevel(curWw, curWc)
        updatePositionLabel()
    }

    private fun setPositionRange() {
        val handle = volumeHandle ?: return
        binding.sbPosition.max = when (curPlane) {
            PLANE_AXIAL -> (handle.meta.depth - 1).coerceAtLeast(0)
            PLANE_CORONAL -> (handle.meta.height - 1).coerceAtLeast(0)
            else -> (handle.meta.width - 1).coerceAtLeast(0)
        }
    }

    /** 位置 / 窗宽窗位变化：取消旧任务并刷新查看器 */
    private fun onViewerParamsChanged() {
        curWw = binding.sbWw.progress.toDouble().coerceAtLeast(1.0)
        curWc = binding.sbWc.progress - wcOffset.toDouble()
        updateWindowLabels()
        refreshViewer()
    }

    private fun applyWindow(ww: Double, wc: Double) {
        binding.sbWw.progress = ww.toInt().coerceIn(1, binding.sbWw.max)
        binding.sbWc.progress = (wc + wcOffset).toInt().coerceIn(0, binding.sbWc.max)
        onViewerParamsChanged()
    }

    /** 从 Volume 提取当前切面并显示（IO 线程执行，结果回主线程） */
    private fun extractCurrentSlice() {
        val handle = volumeHandle ?: return
        val plane = curPlane
        val position = binding.sbPosition.progress
        val ww = curWw
        val wc = curWc

        extractJob?.cancel()
        extractJob = viewLifecycleOwner.lifecycleScope.launch {
            val bitmap: Bitmap? = withContext(Dispatchers.IO) {
                when (plane) {
                    PLANE_AXIAL -> handle.extractAxial(position, ww, wc)
                    PLANE_CORONAL -> handle.extractMpr(CbctJni.PLANE_CORONAL, position, ww, wc)
                    else -> handle.extractMpr(CbctJni.PLANE_SAGITTAL, position, ww, wc)
                }
            }
            if (_binding == null || bitmap == null) {
                if (bitmap == null) Log.e(TAG, "extractCurrentSlice: bitmap is null at pos $position")
                return@launch
            }
            binding.ivCbct.setImageBitmap(bitmap)
            updatePositionLabel()
        }
    }

    private fun updateWindowLabels() {
        binding.tvWw.text = curWw.toInt().toString()
        binding.tvWc.text = curWc.toInt().toString()
    }

    @SuppressLint("SetTextI18n")
    private fun updatePositionLabel() {
        val handle = volumeHandle ?: return
        val max = binding.sbPosition.max
        binding.tvPosition.text = "${binding.sbPosition.progress}/$max"
    }

    private fun buildSummary(handle: CbctVolumeHandle): String {
        val m = handle.meta
        val sizeMm = "%.1f x %.1f x %.1f".format(
            m.width * m.spacingX, m.height * m.spacingY, m.depth * m.spacingZ
        )
        return buildString {
            appendLine("【序列解析摘要】")
            appendLine("- 患者: ${m.patientName.ifEmpty { "N/A" }} (${m.patientSex.ifEmpty { "-" }}) ID: ${m.patientID.ifEmpty { "N/A" }}")
            appendLine("- 检查日期: ${m.studyDate.ifEmpty { "N/A" }}    设备: ${m.manufacturer.ifEmpty { "N/A" }} (${m.modality.ifEmpty { "-" }})")
            appendLine("- 体数据: ${m.width} x ${m.height} x ${m.depth} (物理尺寸 $sizeMm mm)")
            appendLine("- 有效切片: ${m.sliceCount}（按 ImagePositionPatient Z 轴排序，过滤无效文件 ${m.skippedFiles} 个）")
            appendLine("- 体素间距: %.4f / %.4f / %.4f mm".format(m.spacingX, m.spacingY, m.spacingZ))
            appendLine("- HU 参数: slope=%.2f, intercept=%.2f（16bit 体素全程保留）".format(m.slope, m.intercept))
            appendLine("- 解析耗时: ${m.elapsedMs} ms（Native 多线程）")
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        extractJob?.cancel()
        // 释放 Native Volume 内存（IO 线程，避免阻塞 UI）
        val handle = volumeHandle
        volumeHandle = null
        if (handle != null && !handle.isReleased) {
            lifecycleScope.launch(Dispatchers.IO) { handle.release() }
        }
        _binding = null
    }

    companion object {
        private const val TAG = "CbctParseFragment"

        private const val PLANE_AXIAL = 0
        private const val PLANE_CORONAL = 1
        private const val PLANE_SAGITTAL = 2
    }
}
