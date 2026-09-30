package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.SeekBar
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import com.example.dcmtkdemo.adapter.MeasureResultAdapter
import com.example.dcmtkdemo.adapter.MeasureRow
import com.example.dcmtkdemo.databinding.FragmentCbctMeasureBinding
import com.example.dcmtkdemo.utils.FileUtil
import com.wangyao.cbctdeal.engine.CbctParseEngine
import com.wangyao.cbctdeal.jni.CbctVtkJni
import com.wangyao.cbctdeal.model.CbctVolumeHandle
import com.wangyao.cbctdeal.transfer.CbctFileTransfer
import com.wangyao.cbctmeasure.jni.AnnotationJni
import com.wangyao.cbctmeasure.jni.MeasureJni
import com.wangyao.cbctmeasure.jni.RoiJni
import com.wangyao.cbctmeasure.jni.SurgeryPlanJni
import com.wangyao.cbctmeasure.model.AnnotationItem
import com.wangyao.cbctmeasure.model.AnnotationType
import com.wangyao.cbctmeasure.model.CombineOp
import com.wangyao.cbctmeasure.model.MeasurePlane
import com.wangyao.cbctmeasure.model.MeasureType
import com.wangyao.cbctmeasure.model.OverlayOwner
import com.wangyao.cbctmeasure.model.RoiItem
import com.wangyao.cbctmeasure.model.RoiStatsResult
import com.wangyao.cbctmeasure.model.RoiType
import com.wangyao.cbctmeasure.model.SafetyLevel
import com.wangyao.cbctmeasure.model.ToolState
import com.wangyao.cbctmeasure.model.VolumeInfo
import com.wangyao.cbctmeasure.model.WorldPoint
import com.wangyao.cbctmeasure.report.ReportExporter
import com.wangyao.cbctmeasure.store.MeasurementStore
import com.wangyao.cbctmeasure.view.MeasureToolController
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.IOException

/**
 * CBCT 测量与手术规划页（:cbctmeasure 的宿主界面）。
 *
 * 页面只做三件事，与模块分层严格对齐：
 *   1) 数据入口与渲染参数（与 CBCT Parse 页同一套流程）；
 *   2) 把工具按钮映射成 [ToolState]，手势交给 MeasureToolController（PRD 8.5 九态状态机）；
 *   3) 把会话数据（测量/ROI/方案/标注）投影成结果列表，并把导出交给 ReportExporter。
 * 所有数值都来自 Native core：本页不重新计算，只显示 JNI 回传的读数，
 * 这样"界面上的数字""PDF 里的数字""SR 里的数字"必然一致（PRD 6 精度项的前提）。
 */
class CbctMeasureFragment : Fragment(), MeasureToolController.Host {

    private var _binding: FragmentCbctMeasureBinding? = null
    private val binding get() = _binding!!

    /** Native 体数据句柄 */
    private var volumeHandle: CbctVolumeHandle? = null

    /** Native 测量会话句柄（0 = 未创建）；必须早于体数据释放销毁 */
    private var sessionHandle: Long = 0L

    private var controller: MeasureToolController? = null
    private var exporter: ReportExporter? = null

    /** 体数据概况（归档文件名、报告页眉、SR 患者字段都来自这里） */
    private var volumeInfo: VolumeInfo = VolumeInfo.INVALID

    private var adapter: MeasureResultAdapter? = null

    /** 列表选中的行：决定叠加层高亮哪个归属对象 */
    private var selectedRow: MeasureRow? = null

    /** 隔离显示（只保留阈值区间体素）是否开启 */
    private var isolating = false

    /** RadioGroup 程序化设值时的防重入标志（避免 listener <-> controller 互相回调） */
    private var syncingToolRadio = false

    private var syncingModeRadio = false

    private val wcOffset = 1000
    private val huOffset = 1000

    private var curPlane = MeasurePlane.AXIAL
    private var curWw = 4000.0
    private var curWc = 600.0

    private val dirPicker = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri == null) return@registerForActivityResult
        val ctx = context ?: return@registerForActivityResult
        try {
            ctx.contentResolver.takePersistableUriPermission(
                uri, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
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
        _binding = FragmentCbctMeasureBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val appCtx = requireContext().applicationContext
        exporter = ReportExporter(appCtx)

        setupController()
        setupResultsList()

        binding.btnSelectDir.setOnClickListener { dirPicker.launch(null) }
        binding.btnParse.setOnClickListener { runParse() }
        binding.btnLoadAssets.setOnClickListener { loadNeckCtAssets() }

        binding.rgTool.setOnCheckedChangeListener { _, _ -> onToolChanged() }
        binding.rgMeasureType.setOnCheckedChangeListener { _, _ -> onMeasureTypeChanged() }
        binding.rgAnnotationType.setOnCheckedChangeListener { _, _ -> onAnnotationTypeChanged() }
        binding.rgVtkMode.setOnCheckedChangeListener { _, _ -> onVtkModeChanged() }
        binding.rgPlane.setOnCheckedChangeListener { _, _ -> onPlaneChanged() }

        binding.btnUndoPoint.setOnClickListener { controller?.undoPendingPoint() }
        binding.btnResetCam.setOnClickListener { binding.measureView.resetCamera() }
        binding.btnClearDraft.setOnClickListener {
            controller?.abortDraft()
            controller?.syncOverlay()
        }
        binding.btnClearAll.setOnClickListener { confirmClearAll() }

        binding.btnAddHuRoi.setOnClickListener { addHuThresholdRoi() }
        binding.btnIsolate.setOnClickListener { toggleIsolate() }
        binding.btnAddCompositeRoi.setOnClickListener { addCompositeRoi() }
        binding.btnAddNerve.setOnClickListener { addNervePathAndTrace() }

        binding.btnSaveArchive.setOnClickListener { saveArchive() }
        binding.btnLoadArchive.setOnClickListener { loadArchive() }
        binding.btnExportReport.setOnClickListener { exportReport() }

        val seekListener = object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                if (!fromUser) return
                when (sb?.id) {
                    binding.sbWw.id, binding.sbWc.id -> onWindowChanged()
                    binding.sbPosition.id -> onPositionChanged()
                    binding.sbHuLo.id, binding.sbHuHi.id -> updateHuLabels()
                }
            }

            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        }
        binding.sbPosition.setOnSeekBarChangeListener(seekListener)
        binding.sbWw.setOnSeekBarChangeListener(seekListener)
        binding.sbWc.setOnSeekBarChangeListener(seekListener)
        binding.sbHuLo.setOnSeekBarChangeListener(seekListener)
        binding.sbHuHi.setOnSeekBarChangeListener(seekListener)
        // HU 滑杆初值给骨组织区间（200 ~ 3000 HU），与 core 的默认阈值一致
        binding.sbHuLo.progress = 200 + huOffset
        binding.sbHuHi.progress = 3000 + huOffset
        updateHuLabels()
        updateWindowLabels()
        onVtkModeChanged()
        // 初始态是"浏览"：测量类型/标注类型两行都不该露出来
        updateToolRows(controller?.state ?: ToolState.VIEW)
    }

    /** 状态机装配：Host 就是本 Fragment，叠加层与渲染容器由 measure_view 提供 */
    private fun setupController() {
        val view = binding.measureView
        val c = MeasureToolController(this, view.overlayView)
        controller = c
        view.controller = c
        view.post { c.syncOverlay() }
    }

    private fun setupResultsList() {
        adapter = MeasureResultAdapter(emptyList())
        binding.rvResults.layoutManager = LinearLayoutManager(context)
        // 列表在 NestedScrollView 里且高度为 wrap_content：必须关掉自身嵌套滚动，
        // 否则它会自己吃掉竖向拖动，页面滚不动、下面的行也永远看不见（真机踩过）
        binding.rvResults.isNestedScrollingEnabled = false
        binding.rvResults.adapter = adapter
        adapter?.setOnItemClickListener { row -> onRowClicked(row) }
        adapter?.setOnVisibleChangeListener { row, visible -> onRowVisibility(row, visible) }
        adapter?.setOnDeleteClickListener { row -> onRowDelete(row) }
    }

    // =========================================================================
    // 数据入口（与 CBCT Parse 页同流程）
    // =========================================================================

    /** 一键加载内置测试序列，免 SAF 授权 */
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

    /**
     * 解析序列 -> 建会话 -> 挂渲染 -> 读概况 -> 自动恢复历史归档。
     *
     * 释放顺序（Native 内存安全，与 :cbctdeal 的约定一致）：
     * 渲染器（零拷贝引用体数据）-> 测量会话 -> 体数据本体。
     */
    @SuppressLint("SetTextI18n")
    private fun runParse() {
        val path = binding.etPath.text.toString().trim()
        if (path.isEmpty()) {
            toast("请先选择或输入 CBCT 序列目录路径")
            return
        }
        val dir = File(path)
        if (!dir.exists() || !dir.isDirectory) {
            toast("目录不存在: $path")
            return
        }
        val appCtx = context?.applicationContext ?: return

        releaseSessionAndVolume()

        binding.btnParse.isEnabled = false
        binding.btnSelectDir.isEnabled = false
        binding.btnLoadAssets.isEnabled = false
        binding.progressMeasure.visibility = View.VISIBLE
        binding.tvInfo.text = "正在解析序列..."

        viewLifecycleOwner.lifecycleScope.launch {
            var handle: CbctVolumeHandle? = null
            // 迟到的进度回调不能覆盖"解析完成"文案（CBCT Parse 页已踩过这个坑）
            var finished = false
            try {
                val result = withContext(NonCancellable) {
                    val h = CbctParseEngine.parse(appCtx, dir) { cur, total ->
                        activity?.runOnUiThread {
                            if (!finished && _binding != null && total > 0) {
                                binding.progressMeasure.max = total
                                binding.progressMeasure.progress = cur
                                binding.tvInfo.text = "解析中... $cur / $total"
                            }
                        }
                    }
                    finished = true
                    handle = h
                    h
                }
                if (_binding == null) {
                    result?.release()
                    return@launch
                }
                if (result == null) {
                    binding.tvInfo.text = "解析失败：目录中没有有效的 DICOM 序列"
                    return@launch
                }
                volumeHandle = result
                binding.tvInfo.text = "解析完成 (${result.meta.elapsedMs} ms)"
                setupSession(result)
            } catch (e: Exception) {
                Log.e(TAG, "parse failed", e)
                if (_binding != null) binding.tvInfo.text = "解析异常: ${e.message}"
                handle?.release()
            } finally {
                if (_binding != null) {
                    binding.btnParse.isEnabled = true
                    binding.btnSelectDir.isEnabled = true
                    binding.btnLoadAssets.isEnabled = true
                    binding.progressMeasure.visibility = View.GONE
                }
            }
        }
    }

    /** 解析成功后建会话并初始化查看器 */
    private fun setupSession(handle: CbctVolumeHandle) {
        val h = MeasureJni.createSession(handle.ptr)
        if (h == 0L) {
            binding.tvInfo.text = "会话创建失败：Native 侧无法绑定体数据"
            return
        }
        sessionHandle = h
        volumeInfo = MeasureJni.volumeInfo(h)

        binding.measureView.setVolume(handle)
        setupViewerRanges()
        applyViewport()
        refreshResults()
        binding.tvMeasureSummary.text = sessionSummaryText()
        // 有历史归档就自动恢复：PRD 5.6 的"同一序列下次进入继续编辑"
        if (MeasurementStore.hasArchive(requireContext(), volumeInfo.archiveKey())) {
            loadArchive(silent = true)
        }
    }

    private fun setupViewerRanges() {
        val meta = volumeHandle?.meta ?: return
        setPositionRange()
        binding.sbPosition.progress = binding.sbPosition.max / 2
        curWw = meta.windowWidth
        curWc = meta.windowCenter
        binding.sbWw.progress = curWw.toInt().coerceIn(1, binding.sbWw.max)
        binding.sbWc.progress = (curWc + wcOffset).toInt().coerceIn(0, binding.sbWc.max)
        updateWindowLabels()
    }

    private fun sessionSummaryText(): String {
        val text = exporter?.sessionSummary(sessionHandle).orEmpty()
        return if (text.isEmpty()) "摘要区域（尚无数据）" else "【会话摘要】\n$text"
    }

    // =========================================================================
    // 视口：渲染模式 / 平面 / 层位 / 窗宽窗位
    // =========================================================================

    private fun onVtkModeChanged() {
        if (syncingModeRadio) return
        applyViewport()
        updateSliceControlsVisibility()
        controller?.syncOverlay()
    }

    /** 面积测量与神经描记依赖当前切面，自动从 VR 切到 MPR，避免"点了没反应" */
    private fun ensureMprForSliceTools(state: Int) {
        val needSlice = state == ToolState.MEASURE_AREA || state == ToolState.NERVE_TRACE
        val isVr = binding.rgVtkMode.checkedRadioButtonId == binding.rbVtkVr.id
        if (!needSlice || !isVr) return
        syncingModeRadio = true
        binding.rbVtkMpr.isChecked = true
        syncingModeRadio = false
        applyViewport()
        updateSliceControlsVisibility()
        toast("已切换到 MPR 切面模式：面积/描记需要在切面上取点")
    }

    private fun onPlaneChanged() {
        curPlane = when (binding.rgPlane.checkedRadioButtonId) {
            binding.rbCoronal.id -> MeasurePlane.CORONAL
            binding.rbSagittal.id -> MeasurePlane.SAGITTAL
            else -> MeasurePlane.AXIAL
        }
        setPositionRange()
        binding.sbPosition.progress = binding.sbPosition.max / 2
        applyViewport()
        controller?.syncOverlay()
    }

    private fun onPositionChanged() {
        applyViewport()
        controller?.syncOverlay()
    }

    private fun onWindowChanged() {
        curWw = binding.sbWw.progress.toDouble().coerceAtLeast(1.0)
        curWc = binding.sbWc.progress - wcOffset.toDouble()
        updateWindowLabels()
        binding.measureView.setWindowLevel(curWw, curWc)
    }

    /** 把当前 UI 的视口状态推给渲染容器（VTK 侧是命令式的，改一次生效一次） */
    private fun applyViewport() {
        if (_binding == null) return
        val mpr = binding.rgVtkMode.checkedRadioButtonId == binding.rbVtkMpr.id
        binding.measureView.setRenderMode(
            if (mpr) CbctVtkJni.MODE_MPR else CbctVtkJni.MODE_VR
        )
        binding.measureView.setPlane(curPlane, binding.sbPosition.progress)
        binding.measureView.setWindowLevel(curWw, curWc)
        updatePositionLabel()
    }

    private fun updateSliceControlsVisibility() {
        val mpr = binding.rgVtkMode.checkedRadioButtonId == binding.rbVtkMpr.id
        binding.rgPlane.visibility = if (mpr) View.VISIBLE else View.GONE
        binding.rowPosition.visibility = if (mpr) View.VISIBLE else View.GONE
    }

    private fun setPositionRange() {
        val meta = volumeHandle?.meta ?: return
        binding.sbPosition.max = when (curPlane) {
            MeasurePlane.AXIAL -> (meta.depth - 1).coerceAtLeast(0)
            MeasurePlane.CORONAL -> (meta.height - 1).coerceAtLeast(0)
            else -> (meta.width - 1).coerceAtLeast(0)
        }
    }

    @SuppressLint("SetTextI18n")
    private fun updatePositionLabel() {
        binding.tvPosition.text = "${binding.sbPosition.progress}/${binding.sbPosition.max}"
    }

    @SuppressLint("SetTextI18n")
    private fun updateWindowLabels() {
        binding.tvWw.text = curWw.toInt().toString()
        binding.tvWc.text = curWc.toInt().toString()
    }

    @SuppressLint("SetTextI18n")
    private fun updateHuLabels() {
        binding.tvHuLo.text = huLo().toString()
        binding.tvHuHi.text = huHi().toString()
    }

    private fun huLo(): Int = (binding.sbHuLo.progress - huOffset).coerceAtMost(huHi() - 1)

    private fun huHi(): Int = binding.sbHuHi.progress - huOffset

    // =========================================================================
    // 工具路由（PRD 8.5）
    // =========================================================================

    private fun onToolChanged() {
        if (syncingToolRadio) return
        val c = controller ?: return
        val state = toolStateOf(binding.rgTool.checkedRadioButtonId)
        ensureMprForSliceTools(state)
        c.switchState(state)
        // 状态机可能在 switchState 里改 measureType（角度 <-> 距离），把结果同步回单选按钮
        syncMeasureTypeRadio(c.measureType)
        onStateChanged(state)
    }

    private fun toolStateOf(checkedId: Int): Int = when (checkedId) {
        binding.rbDistance.id -> ToolState.MEASURE_DISTANCE
        binding.rbAngle.id -> ToolState.MEASURE_ANGLE
        binding.rbVolume.id -> ToolState.MEASURE_VOLUME
        binding.rbArea.id -> ToolState.MEASURE_AREA
        binding.rbRoi.id -> ToolState.ROI_EDIT
        binding.rbImplant.id -> ToolState.IMPLANT_PLACE
        binding.rbAnnotate.id -> ToolState.ANNOTATE
        binding.rbNerve.id -> ToolState.NERVE_TRACE
        else -> ToolState.VIEW
    }

    private fun onMeasureTypeChanged() {
        if (syncingToolRadio) return
        val type = when (binding.rgMeasureType.checkedRadioButtonId) {
            binding.rbMtPointLine.id -> MeasureType.POINT_TO_LINE
            binding.rbMtArc.id -> MeasureType.ARC_LENGTH
            binding.rbMtHu.id -> MeasureType.HU_SAMPLE
            binding.rbMtDensity.id -> MeasureType.BONE_DENSITY
            binding.rbMtArch.id -> MeasureType.ARCH_LENGTH
            binding.rbMtAngulation.id -> MeasureType.TOOTH_ANGULATION
            binding.rbMtMidline.id -> MeasureType.MIDLINE_OFFSET
            binding.rbMtOverbite.id -> MeasureType.OVERBITE
            else -> MeasureType.DISTANCE
        }
        controller?.setMeasureType(type)
    }

    private fun onAnnotationTypeChanged() {
        val type = when (binding.rgAnnotationType.checkedRadioButtonId) {
            binding.rbAnLine.id -> AnnotationType.LINE
            binding.rbAnArrow.id -> AnnotationType.ARROW
            binding.rbAnCurve.id -> AnnotationType.FREE_CURVE
            binding.rbAnRing.id -> AnnotationType.RING
            binding.rbAnMpr.id -> AnnotationType.MPR_SLICE
            binding.rbAnShot.id -> AnnotationType.SCREENSHOT
            else -> AnnotationType.TEXT_LABEL
        }
        controller?.setAnnotationType(type)
    }

    /** 只切换类型行的可见性；态本身的推进由状态机负责 */
    private fun updateToolRows(state: Int) {
        if (_binding == null) return
        binding.hsvMeasureType.visibility =
            if (state == ToolState.MEASURE_DISTANCE) View.VISIBLE else View.GONE
        binding.hsvAnnotationType.visibility =
            if (state == ToolState.ANNOTATE) View.VISIBLE else View.GONE
    }

    private fun syncMeasureTypeRadio(type: Int) {
        val id = when (type) {
            MeasureType.POINT_TO_LINE -> binding.rbMtPointLine.id
            MeasureType.ARC_LENGTH -> binding.rbMtArc.id
            MeasureType.HU_SAMPLE -> binding.rbMtHu.id
            MeasureType.BONE_DENSITY -> binding.rbMtDensity.id
            MeasureType.ARCH_LENGTH -> binding.rbMtArch.id
            MeasureType.TOOTH_ANGULATION -> binding.rbMtAngulation.id
            MeasureType.MIDLINE_OFFSET -> binding.rbMtMidline.id
            MeasureType.OVERBITE -> binding.rbMtOverbite.id
            else -> binding.rbMtDistance.id
        }
        if (binding.rgMeasureType.checkedRadioButtonId == id) return
        syncingToolRadio = true
        binding.rgMeasureType.check(id)
        syncingToolRadio = false
    }

    /** 进入 NERVE_TRACE 前必须先有路径：界面负责建路径，状态机只负责描点 */
    private fun addNervePathAndTrace() {
        if (sessionHandle == 0L) {
            toast("请先解析序列")
            return
        }
        val count = SurgeryPlanJni.nervesOf(MeasureJni.dumpPlan(sessionHandle)).size + 1
        val id = SurgeryPlanJni.addNervePath(
            sessionHandle, "神经管 $count", SafetyLevel.COLOR_RED, NERVE_RADIUS_MM
        )
        if (id <= 0) {
            toast("神经管路径创建失败")
            return
        }
        syncingToolRadio = true
        binding.rgTool.check(binding.rbNerve.id)
        syncingToolRadio = false
        ensureMprForSliceTools(ToolState.NERVE_TRACE)
        controller?.switchState(ToolState.NERVE_TRACE, id)
        controller?.notifyDataChanged()
        toast("已新建神经管 $id，请在切面上逐层点击描记")
    }

    /** R-01：把当前 HU 滑杆区间存成一个阈值 ROI（体积/骨密度测量与隔离显示都依赖它） */
    private fun addHuThresholdRoi() {
        if (sessionHandle == 0L) {
            toast("请先解析序列")
            return
        }
        val lo = huLo().toDouble()
        val hi = huHi().toDouble()
        val id = RoiJni.addRoi(
            sessionHandle,
            RoiItem(
                type = RoiType.HU_THRESHOLD,
                name = "HU ${lo.toInt()}~${hi.toInt()}",
                huMin = lo,
                huMax = hi,
            )
        )
        if (id <= 0) {
            toast("ROI 创建失败：HU 区间无效")
            return
        }
        controller?.notifyDataChanged()
        toast("已建 HU 阈值 ROI #$id")
    }

    /** 单选弹窗小工具：R-05 的四步向导用它串起来，避免嵌套 Builder 复制粘贴 */
    private fun pickFrom(items: List<String>, title: String, onPick: (Int) -> Unit) {
        AlertDialog.Builder(requireContext())
            .setTitle(title)
            .setItems(items.toTypedArray()) { d, which ->
                d.dismiss()
                onPick(which)
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /**
     * R-05 组合 ROI 的 UI 入口。core（RoiExtractor 的 ROI_COMPOSITE 分支）、归档与
     * 报告早就支持 childA/B/C + opAB/opAC，之前只差一个能点的按钮，这里补上。
     *
     * 算子映射严格对齐 core/RoiExtractor.cpp::roiWeight(ROI_COMPOSITE)：
     *   并 A∪B      -> childA=A, childB=B, opAB=UNION
     *   交 A∩B      -> childA=A, childB=B, opAB=INTERSECT
     *   差 A−B      -> childA=A, childC=B, opAC=SUBTRACT（core 用 "NOT C" 表达差集）
     *   (A∪B)−C     -> childA=A, childB=B, opAB=UNION, childC=C, opAC=SUBTRACT
     *   (A∩B)−C     -> 同上，opAB=INTERSECT
     */
    private fun addCompositeRoi() {
        if (sessionHandle == 0L) {
            toast("请先解析序列")
            return
        }
        val pool = RoiJni.list(sessionHandle).filter { it.type != RoiType.COMPOSITE }
        if (pool.size < 2) {
            toast("组合 ROI 至少需要 2 个已有 ROI（当前 ${pool.size} 个）")
            return
        }
        val label = { r: RoiItem -> "#${r.id} ${RoiType.label(r.type)} ${r.name}" }
        pickFrom(pool.map(label), "组合 ROI（R-05）1/3：选 A") { ai ->
            val a = pool[ai]
            val rest = pool.filterIndexed { i, _ -> i != ai }
            pickFrom(rest.map(label), "2/3：选 B") { bi ->
                val b = rest[bi]
                val ops = listOf("并集 A∪B", "交集 A∩B", "差集 A−B", "混合 (A∪B)−C", "混合 (A∩B)−C")
                pickFrom(ops, "3/3：选组合方式") { oi ->
                    if (oi < 3) {
                        commitComposite(a, b, null, oi)
                    } else {
                        val rest2 = pool.filter { it.id != a.id && it.id != b.id }
                        if (rest2.isEmpty()) {
                            toast("没有第 3 个 ROI 可用于差集")
                        } else {
                            pickFrom(rest2.map(label), "4/4：选 C（从结果里减去）") { ci ->
                                commitComposite(a, b, rest2[ci], oi)
                            }
                        }
                    }
                }
            }
        }
    }

    private fun commitComposite(a: RoiItem, b: RoiItem, c: RoiItem?, opIndex: Int) {
        val union = opIndex == 0 || opIndex == 3
        val subtract = opIndex >= 2
        val childB = if (opIndex == 2) 0 else b.id
        val childC = if (opIndex == 2) b.id else (c?.id ?: 0)
        if (opIndex >= 3 && c == null) {
            toast("混合组合必须选 C")
            return
        }
        val name = when (opIndex) {
            0 -> "组合 ${a.id}∪${b.id}"
            1 -> "组合 ${a.id}∩${b.id}"
            2 -> "组合 ${a.id}−${b.id}"
            3 -> "组合 (${a.id}∪${b.id})−${c?.id}"
            else -> "组合 (${a.id}∩${b.id})−${c?.id}"
        }
        val id = RoiJni.addRoi(
            sessionHandle,
            RoiItem(
                type = RoiType.COMPOSITE,
                name = name,
                childA = a.id,
                childB = childB,
                childC = childC,
                opAB = if (union) CombineOp.UNION else CombineOp.INTERSECT,
                opAC = if (subtract) CombineOp.SUBTRACT else CombineOp.NONE,
            )
        )
        if (id <= 0) {
            toast("组合 ROI 创建失败")
            return
        }
        controller?.notifyDataChanged()
        toast("已建组合 ROI #$id（$name）")
    }

    /**
     * 隔离显示：把体数据不透明度曲线换成阈值区间（:cbctdeal 的 segment 接口）。
     * 区间优先取会话里主阈值 ROI，没有就用滑杆当前值。
     *
     * 注意：PRD §5.2.4 明确复用 VR 的 Opacity Transfer Function，"将阈值范围外体素
     * 不透明度设为 0"。本模块体数据只有一个 actor，阈值内外共用它，所以这里**不能**
     * 再调 setVolumeVisible(false)——那会连 ROI 子集一起隐藏成白屏（真机已复现）。
     * 隔离只作用于 VR；MPR 切面走 LUT 着色，不受该曲线影响，需要提示用户切回 VR。
     */
    private fun toggleIsolate() {
        if (sessionHandle == 0L) {
            toast("请先解析序列")
            return
        }
        val vtk = binding.measureView.vtkView
        if (isolating) {
            vtk.resetSegmentHuRange()
            isolating = false
            toast("已恢复整体显示")
            return
        }
        if (vtk.renderSnapshot()?.get(0)?.toInt() == CbctVtkJni.MODE_MPR) {
            toast("隔离显示基于体渲染的不透明度曲线，请先切回 VR 模式")
            return
        }
        val range = RoiJni.dominantHuRange(sessionHandle)
            ?: doubleArrayOf(huLo().toDouble(), huHi().toDouble())
        val lo = range.getOrElse(0) { 200.0 }
        val hi = range.getOrElse(1) { 3000.0 }
        vtk.setSegmentHuRange(lo, hi, SEGMENT_FEATHER_HU)
        isolating = true
        toast("隔离显示 ${lo.toInt()} ~ ${hi.toInt()} HU")
    }

    /** 清空全部数据（不可撤销，必须二次确认） */
    private fun confirmClearAll() {
        if (sessionHandle == 0L) return
        AlertDialog.Builder(requireContext())
            .setTitle("清空全部测量数据？")
            .setMessage("测量项、ROI、种植体方案与标注都会被删除，磁盘上的归档不受影响。")
            .setNegativeButton("取消", null)
            .setPositiveButton("清空") { _, _ ->
                MeasureJni.clearMeasures(sessionHandle)
                AnnotationJni.clearAnnotations(sessionHandle)
                SurgeryPlanJni.clearPlan(sessionHandle)
                isolating = false
                binding.measureView.vtkView.resetSegmentHuRange()
                binding.measureView.vtkView.setVolumeVisible(true)
                selectedRow = null
                binding.measureView.overlayView.setHighlight(OverlayOwner.NONE, -1)
                controller?.notifyDataChanged()
                binding.tvMeasureSummary.text = sessionSummaryText()
            }
            .show()
    }

    // =========================================================================
    // 结果列表
    // =========================================================================

    @SuppressLint("SetTextI18n")
    private fun refreshResults() {
        val h = sessionHandle
        if (h == 0L || _binding == null) return
        val rows = ArrayList<MeasureRow>()

        MeasureJni.records(h).forEach { m ->
            val sub = if (m.isFailed()) m.errorText() else measureDetail(m.detail, m.type)
            rows.add(
                MeasureRow(
                    ownerKind = OverlayOwner.MEASURE,
                    id = m.id,
                    title = "#${m.id} ${m.name}",
                    value = if (m.isFailed()) "待重算" else m.displayValue(),
                    sub = sub,
                    color = m.color,
                    visible = m.visible,
                )
            )
        }
        RoiJni.list(h).forEach { r ->
            rows.add(
                MeasureRow(
                    ownerKind = OverlayOwner.ROI,
                    id = r.id,
                    title = "#${r.id} ${RoiType.label(r.type)} ${r.name}".trim(),
                    value = roiParameterText(r),
                    sub = if (r.visible) "显示中" else "已隐藏",
                    color = r.color,
                    visible = r.visible,
                )
            )
        }
        val planJson = MeasureJni.dumpPlan(h)
        SurgeryPlanJni.implantsOf(planJson).forEach { im ->
            rows.add(
                MeasureRow(
                    ownerKind = OverlayOwner.IMPLANT,
                    id = im.id,
                    title = "#${im.id} ${im.name.ifEmpty { "种植体" }}（${SafetyLevel.label(im.level)}）",
                    value = "骨高 %.1f / 骨宽 %.1f mm".format(im.boneHeightMm, im.boneWidthMm),
                    sub = "神经距 %.1f · 间距 %.1f mm%s".format(
                        im.nerveDistMm, im.minSpacingMm,
                        if (im.warnText.isEmpty()) "" else " · ${im.warnText}"
                    ),
                    color = SafetyLevel.color(im.level),
                    visible = im.visible,
                )
            )
        }
        SurgeryPlanJni.nervesOf(planJson).forEach { np ->
            rows.add(
                MeasureRow(
                    ownerKind = OverlayOwner.NERVE,
                    id = np.id,
                    title = "#${np.id} ${np.name.ifEmpty { "神经管" }}",
                    value = "${np.points.size} 点 · R=%.1fmm".format(np.radiusMm),
                    sub = "点击行可继续描记",
                    color = np.color,
                    visible = np.visible,
                )
            )
        }
        AnnotationJni.list(h).forEach { a ->
            rows.add(
                MeasureRow(
                    ownerKind = OverlayOwner.ANNOTATION,
                    id = a.id,
                    title = "#${a.id} ${AnnotationType.label(a.type)}",
                    value = a.text.ifEmpty { "(未填写文字)" },
                    sub = annotationSubText(a),
                    color = a.color,
                    visible = a.visible,
                )
            )
        }
        adapter?.updateData(rows)
        binding.tvListTitle.text = "测量结果（${rows.size} 项）"
        binding.tvMeasureSummary.text = sessionSummaryText()
    }

    /** 标注行的次要信息：点数 + 是否绑定测量 + 是否像素坐标（A-07） */
    private fun annotationSubText(a: AnnotationItem): String {
        val parts = ArrayList<String>(3)
        parts.add("${a.points.size} 点")
        if (a.measureId > 0) parts.add("绑定测量#${a.measureId}")
        if (a.isPixelSpace()) parts.add("像素坐标")
        return parts.joinToString(" · ")
    }

    /** detail JSON -> 一行明细；只挑对医生有意义的字段 */
    private fun measureDetail(detail: JSONObject?, type: Int): String = when (type) {
        MeasureType.HU_SAMPLE ->
            "组织：${detail?.optString("tissue").orEmpty()}"
        MeasureType.ARC_LENGTH ->
            "采样点 ${detail?.optInt("samples", 0) ?: 0}"
        MeasureType.ROI_AREA ->
            "多边形 ${detail?.optInt("samples", 0) ?: 0} 点，%.1f mm²".format(
                detail?.optDouble("areaMm2", 0.0) ?: 0.0
            )
        MeasureType.ROI_VOLUME ->
            "遍历 ${detail?.optLong("scannedVoxels", 0) ?: 0} 体素 / " +
                    "${detail?.optLong("elapsedMs", 0) ?: 0} ms"
        MeasureType.ANGLE ->
            "AB %.1f · BC %.1f · AC %.1f mm".format(
                detail?.optDouble("ab", 0.0) ?: 0.0,
                detail?.optDouble("bc", 0.0) ?: 0.0,
                detail?.optDouble("ac", 0.0) ?: 0.0,
            )
        else -> detail?.optString("note").orEmpty()
    }

    private fun roiParameterText(r: RoiItem): String = when (r.type) {
        RoiType.HU_THRESHOLD -> "%.0f ~ %.0f HU".format(r.huMin, r.huMax)
        RoiType.BOX -> "盒 %.1f×%.1f×%.1f mm".format(
            r.boxMax.x - r.boxMin.x, r.boxMax.y - r.boxMin.y, r.boxMax.z - r.boxMin.z
        )
        RoiType.PLANE -> "${MeasurePlane.label(r.plane)} ${r.polygon.size} 点多边形"
        RoiType.SPHERE -> "球 R=%.1f mm".format(r.sphereRadius)
        RoiType.COMPOSITE -> "(${r.childA} ${CombineOp.label(r.opAB)} ${r.childB}) " +
                "${CombineOp.label(r.opAC)} ${r.childC}"
        else -> "ROI"
    }

    /** 点行：高亮归属对象；ROI 顺带跑一次量化统计（遍历体素，放 IO 线程） */
    private fun onRowClicked(row: MeasureRow) {
        selectedRow = row
        binding.measureView.overlayView.setHighlight(row.ownerKind, row.id)
        when (row.ownerKind) {
            OverlayOwner.ROI -> showRoiStats(row.id)
            OverlayOwner.NERVE -> {
                syncingToolRadio = true
                binding.rgTool.check(binding.rbNerve.id)
                syncingToolRadio = false
                ensureMprForSliceTools(ToolState.NERVE_TRACE)
                controller?.switchState(ToolState.NERVE_TRACE, row.id)
            }
            OverlayOwner.ANNOTATION -> editAnnotationText(row.id)
            else -> binding.tvMeasureSummary.text = "【选中】${row.title}\n${row.value}\n${row.sub}"
        }
    }

    /** R-01~R-05 的量化统计：体积/面积/HU 分布，Native 遍历体素，耗时可能上百 ms */
    @SuppressLint("SetTextI18n")
    private fun showRoiStats(roiId: Int) {
        val h = sessionHandle
        if (h == 0L) return
        viewLifecycleOwner.lifecycleScope.launch {
            val text = withContext(Dispatchers.IO) {
                val stats = try {
                    RoiStatsResult.from(JSONObject(RoiJni.statRoi(h, roiId)))
                } catch (e: Exception) {
                    Log.e(TAG, "statRoi failed", e)
                    RoiStatsResult.EMPTY
                }
                "【ROI #$roiId 量化】\n${stats.summaryText()}"
            }
            if (_binding != null) binding.tvMeasureSummary.text = text
        }
    }

    /** A-01 的快捷改字：列表里点标注行即可编辑 */
    private fun editAnnotationText(annoId: Int) {
        val h = sessionHandle
        if (h == 0L) return
        val current = AnnotationJni.list(h).firstOrNull { it.id == annoId } ?: return
        val input = EditText(requireContext()).apply {
            setText(current.text)
            setSelection(current.text.length)
            hint = "标注文字"
        }
        AlertDialog.Builder(requireContext())
            .setTitle("编辑标注 #$annoId")
            .setView(input)
            .setNegativeButton("取消") { d, _ ->
                d.dismiss()
                showAnnotationSummary(current)
            }
            .setPositiveButton("确定") { d, _ ->
                val text = input.text.toString()
                if (AnnotationJni.setAnnotationText(h, annoId, text)) {
                    controller?.notifyDataChanged()
                } else {
                    toast("文字更新失败")
                }
                d.dismiss()
            }
            .show()
    }

    @SuppressLint("SetTextI18n")
    private fun showAnnotationSummary(a: AnnotationItem) {
        binding.tvMeasureSummary.text =
            "【标注 #${a.id}】${AnnotationType.label(a.type)}\n${a.text}\n" +
                    "点数 ${a.points.size} · ${MeasurePlane.label(a.plane)} 第 ${a.planePosition} 层"
    }

    private fun onRowVisibility(row: MeasureRow, visible: Boolean) {
        val h = sessionHandle
        if (h == 0L) return
        val ok = when (row.ownerKind) {
            OverlayOwner.MEASURE -> MeasureJni.setMeasureVisible(h, row.id, visible)
            OverlayOwner.ROI -> RoiJni.setRoiVisible(h, row.id, visible)
            OverlayOwner.IMPLANT -> SurgeryPlanJni.setImplantVisible(h, row.id, visible)
            OverlayOwner.NERVE -> SurgeryPlanJni.setNerveVisible(h, row.id, visible)
            OverlayOwner.ANNOTATION -> AnnotationJni.setAnnotationVisible(h, row.id, visible)
            else -> false
        }
        if (!ok) toast("显示状态更新失败 #${row.id}")
        controller?.notifyDataChanged()
    }

    private fun onRowDelete(row: MeasureRow) {
        val h = sessionHandle
        if (h == 0L) return
        val ok = when (row.ownerKind) {
            OverlayOwner.MEASURE -> MeasureJni.removeMeasure(h, row.id)
            OverlayOwner.ROI -> RoiJni.removeRoi(h, row.id)
            OverlayOwner.IMPLANT -> SurgeryPlanJni.removeImplant(h, row.id)
                    .also { SurgeryPlanJni.recomputePlan(h) }
            OverlayOwner.NERVE -> SurgeryPlanJni.removeNervePath(h, row.id)
            OverlayOwner.ANNOTATION -> AnnotationJni.removeAnnotation(h, row.id)
            else -> false
        }
        if (ok && selectedRow?.ownerKind == row.ownerKind && selectedRow?.id == row.id) {
            selectedRow = null
            binding.measureView.overlayView.setHighlight(OverlayOwner.NONE, -1)
        }
        if (!ok) toast("删除失败 #${row.id}")
        controller?.notifyDataChanged()
    }

    // =========================================================================
    // 归档与报告（PRD 5.5 / 5.6 / 8.4）
    // =========================================================================

    @SuppressLint("SetTextI18n")
    private fun saveArchive() {
        val ctx = context ?: return
        if (sessionHandle == 0L) {
            toast("请先解析序列")
            return
        }
        val key = volumeInfo.archiveKey()
        val results = MeasurementStore.saveAll(ctx, sessionHandle, key)
        binding.tvMeasureSummary.text =
            "【归档保存】$key\n" + results.joinToString("\n") { "- ${it.message} ${it.pathText()}" }
        toast("已保存到 measurements/annotations/plans")
    }

    @SuppressLint("SetTextI18n")
    private fun loadArchive(silent: Boolean = false) {
        val ctx = context ?: return
        if (sessionHandle == 0L) {
            if (!silent) toast("请先解析序列")
            return
        }
        val results = exporter?.restore(sessionHandle, volumeInfo) ?: return
        if (!silent) {
            binding.tvMeasureSummary.text =
                "【归档恢复】${volumeInfo.archiveKey()}\n" +
                        results.joinToString("\n") { "- ${it.message}${if (it.usedBackup) "（备份档）" else ""}" }
        }
        controller?.notifyDataChanged()
        if (!silent) toast("归档已恢复")
    }

    /**
     * 导出 PDF + DICOM SR。
     * 截图必须在主线程取（SurfaceView 的帧），之后的归档/PDF/SR 全部放 IO 线程。
     */
    @SuppressLint("SetTextI18n")
    private fun exportReport() {
        val h = sessionHandle
        val ex = exporter ?: return
        if (h == 0L) {
            toast("请先解析序列")
            return
        }
        val evidence = binding.measureView.captureEvidence()
        if (evidence == null) {
            toast("未取到渲染帧：请确认三维视图已显示")
            return
        }
        val dictError = ex.ensureDictionary()
        if (dictError.isNotEmpty()) {
            binding.tvMeasureSummary.text = "【导出中止】$dictError"
            return
        }
        askOperator { name ->
            binding.btnExportReport.isEnabled = false
            binding.tvInfo.text = "正在导出报告..."
            viewLifecycleOwner.lifecycleScope.launch {
                val t0 = SystemClock.elapsedRealtime()
                val lines = withContext(Dispatchers.IO) {
                    ex.exportAll(h, volumeInfo, evidence, name).lines()
                }
                if (_binding == null) return@launch
                // PC-03/PC-01 口径：归档 + PDF + SR + 回读校验整条链路的耗时
                Log.i(TAG, "exportAll cost=%dms".format(SystemClock.elapsedRealtime() - t0))
                binding.btnExportReport.isEnabled = true
                binding.tvInfo.text = "导出完成"
                binding.tvMeasureSummary.text = "【报告导出】\n" + lines.joinToString("\n")
                toast("PDF 与 SR 已导出")
            }
        }
    }

    /** 报告署名：默认值让回归脚本一次点击即可确认 */
    private fun askOperator(done: (String) -> Unit) {
        val input = EditText(requireContext()).apply {
            setText(DEFAULT_OPERATOR)
            setSelection(text.length)
            hint = "操作者姓名"
        }
        AlertDialog.Builder(requireContext())
            .setTitle("报告署名")
            .setView(input)
            .setNegativeButton("取消") { d, _ -> d.dismiss() }
            .setPositiveButton("导出") { d, _ ->
                d.dismiss()
                done(input.text.toString().ifEmpty { DEFAULT_OPERATOR })
            }
            .show()
    }

    // =========================================================================
    // MeasureToolController.Host
    // =========================================================================

    override fun vtkView() = _binding?.measureView?.vtkView

    override fun sessionHandle(): Long = sessionHandle

    override fun onHintChanged(hint: String) {
        if (_binding != null) binding.tvToolHint.text = hint
    }

    override fun onStateChanged(state: Int) {
        updateToolRows(state)
    }

    override fun onDataChanged() {
        refreshResults()
    }

    /** A-01/A-02/A-04 需要文字：弹窗确认后回调 done，取消则丢弃这一笔 */
    override fun promptAnnotationText(
        type: Int,
        points: List<WorldPoint>,
        done: (String) -> Unit
    ) {
        val ctx = context ?: return
        val input = EditText(ctx).apply { hint = "标注文字（可留空）" }
        AlertDialog.Builder(ctx)
            .setTitle("${AnnotationType.label(type)}：${points.size} 点")
            .setMessage("落点世界坐标 %.1f, %.1f, %.1f mm".format(
                points.firstOrNull()?.x ?: 0.0,
                points.firstOrNull()?.y ?: 0.0,
                points.firstOrNull()?.z ?: 0.0
            ))
            .setView(input)
            .setCancelable(false)
            .setNegativeButton("放弃") { d, _ -> d.dismiss() }
            .setPositiveButton("确定") { d, _ ->
                d.dismiss()
                done(input.text.toString())
            }
            .show()
    }

    // =========================================================================
    // 生命周期
    // =========================================================================

    override fun onDestroyView() {
        super.onDestroyView()
        releaseSessionAndVolume()
        controller = null
        exporter = null
        adapter = null
        volumeInfo = VolumeInfo.INVALID
        _binding = null
    }

    /** 渲染器 -> 会话 -> 体数据，顺序不可颠倒（Native 侧会话与渲染器都只持引用） */
    private fun releaseSessionAndVolume() {
        _binding?.measureView?.vtkView?.release()
        if (sessionHandle != 0L) {
            MeasureJni.destroySession(sessionHandle)
            sessionHandle = 0L
        }
        val handle = volumeHandle
        volumeHandle = null
        if (handle != null && !handle.isReleased) {
            // 释放可能涉及大数组 munmap，放 IO 线程避免拖慢页面切换
            lifecycleScope.launch(Dispatchers.IO) { handle.release() }
        }
    }

    private fun toast(text: String) {
        context?.let { Toast.makeText(it, text, Toast.LENGTH_SHORT).show() }
    }

    companion object {
        private const val TAG = "CbctMeasureFragment"
        private const val DEFAULT_OPERATOR = "Qoder Operator"

        /** 神经管描记的默认半径（S-05 的安全距离按此半径外推） */
        private const val NERVE_RADIUS_MM = 1.5

        /** 隔离显示时区间边界的羽化宽度（HU） */
        private const val SEGMENT_FEATHER_HU = 60.0
    }
}
