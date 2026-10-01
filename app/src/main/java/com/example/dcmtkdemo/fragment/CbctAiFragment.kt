package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtkdemo.databinding.FragmentCbctAiBinding
import com.wangyao.cbctmeasure.ai.AiEngine
import com.wangyao.cbctmeasure.jni.AiJni
import com.wangyao.cbctmeasure.jni.SurgeryPlanJni
import com.wangyao.cbctmeasure.model.AiArch
import com.wangyao.cbctmeasure.model.AiCandidateInfo
import com.wangyao.cbctmeasure.model.AiResultInfo
import com.wangyao.cbctmeasure.model.ImplantItem
import com.wangyao.cbctmeasure.model.SafetyLevel
import com.wangyao.cbctmeasure.model.VolumeInfo
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * AI 辅助分析面板（PRD 5.6 Phase 2），本轮从 [CbctMeasureFragment] 剥离出来。
 *
 * 为什么它是一个「子 Fragment」而不是抽屉里的第二个页面：
 * 推理的输入是宿主会话里那份零拷贝体数据（MeasureSession -> VolumeRef），
 * 抽屉切页用的是 FragmentTransaction.replace()，离开测量页就会销毁会话与渲染窗口。
 * 所以 AI 页必须寄生在同一个会话之上：本 Fragment 只带 UI 与编排，
 * sessionHandle / 刷新列表 / 重画叠加层 / 选病例这四件事全部经 [CbctAiHost] 回到宿主，
 * 「谁拥有 Native 会话」这个所有权始终只有一个答案。
 *
 * 本类仍然不做任何数值计算：阈值来自 assets/models/teeth_cnn.json 的标定值，
 * 掩膜、连通域、逐牙统计、候选评分都在 Native core，页面上显示的每个数都是 JNI 回执，
 * 这样"屏幕上的体积""PDF 里的体积""SR 里的体积"才是同一个数（PRD 6 精度项的前提）。
 */
class CbctAiFragment : Fragment() {

    private var _binding: FragmentCbctAiBinding? = null
    private val binding get() = _binding!!

    /** 装载回执：null = 还没装载过（按钮全灰，状态行说明候选 so 路径） */
    private var aiModel: AiEngine.LoadResult? = null

    /** 最近一次推理的回执，状态行与取证导出都引用它 */
    private var aiInfo: AiResultInfo = AiResultInfo.EMPTY

    /** 最近一次推理是否留了 feat/prob 缓冲（取证导出依赖它） */
    private var parityKept = false

    /** 遮罩式忙碌：AI 期间禁用全部按钮，避免中途重复点击叠加 Native 调用 */
    private var busy = false

    private val host: CbctAiHost?
        get() = parentFragment as? CbctAiHost

    private val sessionHandle: Long
        get() = host?.aiSessionHandle() ?: 0L

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentCbctAiBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        // 按钮顺序 = 使用顺序；可用性统一由 updateButtons() 决定
        binding.btnAiLoad.setOnClickListener { loadAiModel() }
        binding.btnAiSegment.setOnClickListener { runAiSegment(keepParity = false) }
        binding.btnAiMeasure.setOnClickListener { aiAutoMeasure() }
        binding.btnAiRecommend.setOnClickListener { aiRecommend() }
        binding.btnAiParity.setOnClickListener { runAiSegment(keepParity = true) }
        binding.btnAiClear.setOnClickListener { clearAi() }
        binding.btnAiDental.setOnClickListener { host?.aiPickDentalCase() }
        binding.cbAiOverlay.setOnCheckedChangeListener { _, checked ->
            val h = sessionHandle
            if (h != 0L) {
                AiEngine.setOverlayVisible(h, checked)
                host?.aiInvalidate(overlay = true, results = false)
            }
        }
        refreshStatus()
    }

    /**
     * 宿主重建会话（换了序列 / 重新解析）时调用：
     * 新会话里旧的 OrtEngine 已随旧 MeasureSession 析构，沿用 aiModel 会让按钮
     * "看着可点"却报句柄无效 —— 所以状态必须清零并重新置灰。
     */
    fun onSessionRebuilt() {
        aiModel = null
        aiInfo = AiResultInfo.EMPTY
        parityKept = false
        if (_binding != null) refreshStatus()
    }

    /** 宿主把面板切回可见时刷新（隐藏期间可能已经换过序列或建过 ROI） */
    fun onPanelShown() {
        if (_binding != null) refreshStatus()
    }

    /** 状态是否与当前宿主会话匹配（宿主据此决定要不要提示"请先装载"） */
    fun isSegmentReady(): Boolean = aiInfo.ok

    // =========================================================================
    // 推理编排
    // =========================================================================

    /** 装载 ONNX Runtime + 模型。dlopen 失败只影响 AI，测量功能不受牵连 */
    @SuppressLint("SetTextI18n")
    private fun loadAiModel() {
        val appCtx = context?.applicationContext ?: return
        val h = sessionHandle
        if (h == 0L) {
            toast("请先解析一个 CBCT 序列（AI 的输入就是这份体数据）")
            return
        }
        setBusy(true, "正在装载 ONNX Runtime 与模型...")
        viewLifecycleOwner.lifecycleScope.launch {
            val r = try {
                withContext(Dispatchers.Default) { AiEngine.loadModel(appCtx, h) }
            } catch (e: Exception) {
                Log.e(TAG, "loadModel threw", e)
                AiEngine.LoadResult(false, "装载异常: ${e.message}")
            }
            if (_binding == null) return@launch
            aiModel = r
            setBusy(false, null)
            refreshStatus()
            if (!r.ok) toast("装载失败：${r.error}")
        }
    }

    /**
     * 一次完整推理。keepParity=true 是 AC-08 取证入口：让 Native 把 feat/prob
     * 留在会话里，随后落盘给主机脚本逐元素比对。
     */
    @SuppressLint("SetTextI18n")
    private fun runAiSegment(keepParity: Boolean) {
        val h = sessionHandle
        val model = aiModel
        if (h == 0L || model == null || !model.ok) {
            toast("请先装载模型")
            return
        }
        setBusy(true, if (keepParity) "AI 推理中（保留取证缓冲）..." else "AI 推理中...")
        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val res = AiEngine.segment(
                    h, model.threshold,
                    keepParity = keepParity, autoMeasure = false,
                )
                if (_binding == null) return@launch
                aiInfo = res.info
                parityKept = keepParity
                setBusy(false, null)
                refreshStatus()
                host?.aiInvalidate(overlay = true, results = false)
                when {
                    !res.info.ok -> toast("分割失败：${res.info.error}")
                    keepParity -> dumpAiParity()
                    else -> {
                        // 分割完就提醒下一步；掩膜不建 ROI 的话结果列表里看不到任何数
                        toast("${res.info.summary}\n下一步可点「逐牙自动测量」")
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "ai segment failed", e)
                if (_binding != null) {
                    setBusy(false, null)
                    binding.tvAiStatus.text = "推理异常: ${e.message}"
                }
            } finally {
                if (_binding != null) setBusy(false, null)
            }
        }
    }

    /** 每个分割实例 -> R-06 掩膜 ROI + M-04 体积 + M-08 骨密度，进既有的列表/报告/SR 链路 */
    private fun aiAutoMeasure() {
        val h = sessionHandle
        if (h == 0L || !aiInfo.ok) {
            toast("请先做一次自动分割")
            return
        }
        setBusy(true, "正在按分割结果逐牙建 ROI 并测量...")
        viewLifecycleOwner.lifecycleScope.launch {
            val n = withContext(Dispatchers.Default) { AiJni.nativeAiAutoMeasure(h, true) }
            if (_binding == null) return@launch
            aiInfo = AiEngine.status(h)
            setBusy(false, null)
            refreshStatus()
            host?.aiInvalidate(overlay = true, results = true)
            toast(
                if (n > 0) "已为 $n 颗牙建立掩膜 ROI 与体积/骨密度测量（结果列表可见）"
                else "没有可建的测量（先做分割）"
            )
        }
    }

    /** AI-03：缺牙间隙候选 -> 对话框 -> 选中即落成一颗按建议姿态的种植体 */
    private fun aiRecommend() {
        val h = sessionHandle
        if (h == 0L || !aiInfo.ok) {
            toast("AI-03 需要先有分割结果（牙弓与咬合平面来自它）")
            return
        }
        setBusy(true, "正在评估候选位点...")
        viewLifecycleOwner.lifecycleScope.launch {
            val (list, summary) = AiEngine.recommend(h, AI_MIN_GAP_MM, AI_MAX_CANDIDATES)
            if (_binding == null) return@launch
            setBusy(false, null)
            if (list.isEmpty()) {
                toast(summary.ifEmpty { "没有可用的缺牙间隙（牙数不足或间隙过小）" })
                return@launch
            }
            val labels = list.map { c ->
                "${c.title()} · 评分 ${String.format("%.0f", c.score)} · " +
                        "Ø${String.format("%.1f", c.diaMm)}x${String.format("%.1f", c.lengthMm)}mm · " +
                        "${SafetyLevel.label(c.level)}${if (c.reason.isEmpty()) "" else "\n${c.reason}"}"
            }.toTypedArray()
            // 候选表可能很长：加上"间隙宽度"这列后用户更容易剔除解剖不可能的候选
            AlertDialog.Builder(requireContext())
                .setTitle("种植位点推荐（${list.size} 个候选）\n$summary")
                .setItems(labels) { _, which -> placeRecommended(list[which]) }
                .setNegativeButton("关闭", null)
                .show()
        }
    }

    /** 把建议位点变成一颗真正的种植体：走既有的 S-02~S-06 安全评估通道 */
    private fun placeRecommended(c: AiCandidateInfo) {
        val h = sessionHandle
        if (h == 0L) return
        val implant = ImplantItem(
            name = "AI推荐 ${AiArch.label(c.arch)}${c.beforeId}-${c.afterId}",
            entry = c.entry,
            pitchDeg = c.pitchDeg,
            yawDeg = c.yawDeg,
            diaMm = c.diaMm,
            lengthMm = c.lengthMm,
            depthMm = c.depthMm,
        )
        val id = SurgeryPlanJni.addImplant(h, implant.toJson().toString())
        if (id <= 0) {
            toast("落库失败（Native addImplant 返回 $id）")
            return
        }
        SurgeryPlanJni.recomputePlan(h)
        host?.aiInvalidate(overlay = true, results = true)
        toast(
            "已放置推荐种植体 #$id（骨高 ${String.format("%.1f", c.boneHeightMm)}mm，" +
                    "骨宽 ${String.format("%.1f", c.boneWidthMm)}mm）"
        )
    }

    /** AC-08 取证导出：真机 feat/prob/label/inst 落到 filesDir/ai_parity */
    @SuppressLint("SetTextI18n")
    private fun dumpAiParity() {
        val ctx = context?.applicationContext ?: return
        val h = sessionHandle
        if (h == 0L || !parityKept) {
            toast("取证导出要用带缓冲的推理（直接按「导出取证数据」即可）")
            return
        }
        val info = host?.aiVolumeInfo() ?: VolumeInfo.INVALID
        val caseId = when {
            !info.valid -> "unknown"
            info.seriesDescription.isNotEmpty() -> info.seriesDescription
            else -> info.seriesInstanceUID.ifEmpty { "unknown" }
        }
        // 文件名要能被 adb / shell 直接引用，所以先把描述里的空格与中文换掉
        val safeCase = caseId.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val json = AiEngine.dumpParity(ctx, h, "device_$safeCase.parity.bin")
        val o = try {
            JSONObject(json)
        } catch (e: Exception) {
            JSONObject()
        }
        val ok = o.optBoolean("ok")
        val msg = if (ok) {
            "取证数据已写出：${o.optLong("bytes")} 字节（feat ${o.optLong("featCount")} / prob ${o.optLong("probCount")} float）\n" +
                    "拉取：adb shell run-as ${ctx.packageName} cat ${o.optString("path")}"
        } else {
            "取证导出失败：${o.optString("error").ifEmpty { json }}"
        }
        Log.d(TAG, msg)
        binding.tvAiStatus.text = "${statusText()}\n$msg"
        toast(if (ok) "取证数据已导出" else "取证导出失败")
    }

    /**
     * 丢弃推理结果。R-06 ROI 与测量行**不会**被删（它们是正常临床对象），
     * 但掩膜一失效，那些行的统计就变空并给出原因 —— 这里必须把这个后果说清楚。
     */
    private fun clearAi() {
        val h = sessionHandle
        if (h == 0L) return
        val roiKept = if (aiInfo.ok)
            "（已生成的掩膜 ROI 与测量行保留，但统计将变空并写明原因）" else ""
        AiJni.nativeAiClear(h)
        parityKept = false
        aiInfo = AiResultInfo.EMPTY
        if (_binding != null) {
            refreshStatus()
            binding.tvAiStatus.text = "${statusText()}\n已清除推理结果$roiKept"
        }
        host?.aiInvalidate(overlay = true, results = true)
        toast("AI 推理结果已清除$roiKept")
    }

    // =========================================================================
    // 状态行 / 按钮可用性
    // =========================================================================

    /** 状态栏：模型/阈值/耗时/实例数/内存 —— 报告里的 PC-05 数字就取自这里 */
    private fun statusText(): String {
        val model = aiModel
        val sb = StringBuilder()
        if (sessionHandle == 0L) {
            sb.append("当前没有会话：先解析一个 CBCT 序列")
            return sb.toString()
        }
        if (model == null || !model.ok) {
            sb.append("AI 未装载")
            if (model != null) {
                // 只有真的装载失败过才摊开错误与 so 候选路径；
                // 首次进入时两行 100 字符的绝对路径会被读成"出错了"（真机 UI 走查反馈）
                sb.append("：${model.error}")
                sb.append("\nlibonnxruntime 候选: ")
                context?.applicationContext?.let { sb.append(AiEngine.ortSoCandidates(it).joinToString(" | ")) }
            } else {
                sb.append("（模型未加载）\n下一步：点「装载模型」。换过数据例之后要重新装载一次，" +
                        "因为 ORT 会话是跟着测量会话走的。")
            }
            return sb.toString()
        }
        sb.append("运行时 ${model.runtimeInfo}；阈值 ${model.threshold}")
        sb.append("\ndlopen 命中：${model.usedSoPath}")
        if (!model.channelsMatch()) {
            sb.append("（通道不匹配：模型 ${model.inChannels} vs 特征 ${model.expectChannels}）")
        }
        if (aiInfo.ok) {
            sb.append("\n分割 ${aiInfo.instances.size} 颗 · 牙齿体素 ${aiInfo.toothVoxels}")
            sb.append(
                "\n耗时 预处理 %.0f + 推理 %.0f + 后处理 %.0f = %.0f ms（推理占 %.1f%%）".format(
                    aiInfo.prepMs, aiInfo.inferMs, aiInfo.postMs, aiInfo.totalMs,
                    aiInfo.inferSharePercent()
                )
            )
            sb.append("\n新增 Native 内存约 %.1f MB；PC-05<=5s：%s".format(
                aiInfo.allocBytes / 1048576.0,
                if (aiInfo.pc05Within5s()) "达标" else "未达标"
            ))
            sb.append("\n掩膜网格 ${aiInfo.redDim.joinToString("x")}，体素 ${String.format("%.3f", aiInfo.redVoxelMm3)} mm³")
            val biggest = aiInfo.instances.maxByOrNull { it.voxels }
            if (biggest != null && biggest.voxels > MEGA_INSTANCE_RED_VOXELS) {
                sb.append("\n注意：最大实例 #${biggest.id} 占 ${biggest.voxels} 个掩膜体素，" +
                        "大概率是多颗牙合并，体积要按牙组读")
            }
        } else if (aiInfo.error.isNotEmpty()) {
            sb.append("\n最近一次推理：${aiInfo.error}")
        }
        return sb.toString()
    }

    @SuppressLint("SetTextI18n")
    private fun setBusy(value: Boolean, hint: String?) {
        busy = value
        if (_binding == null) return
        binding.progressAi.visibility = if (value) View.VISIBLE else View.GONE
        updateButtons()
        if (hint != null) binding.tvAiStatus.text = hint
    }

    private fun canRun(): Boolean = sessionHandle != 0L && aiModel?.ok == true

    private fun updateButtons() {
        if (_binding == null) return
        binding.btnAiLoad.isEnabled = !busy && sessionHandle != 0L
        binding.btnAiSegment.isEnabled = !busy && canRun()
        binding.btnAiParity.isEnabled = !busy && canRun()
        binding.btnAiMeasure.isEnabled = !busy && canRun() && aiInfo.ok
        binding.btnAiRecommend.isEnabled = !busy && canRun() && aiInfo.ok
        binding.btnAiClear.isEnabled = !busy && sessionHandle != 0L
        binding.btnAiDental.isEnabled = !busy
    }

    @SuppressLint("SetTextI18n")
    private fun refreshStatus() {
        if (_binding == null) return
        binding.tvAiStatus.text = statusText()
        updateButtons()
    }

    private fun toast(text: String) {
        context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    companion object {
        private const val TAG = "CbctAiFragment"

        /** AI-03 的间隙下限（mm）：小于它的邻牙间隙不作为种植位点 */
        private const val AI_MIN_GAP_MM = 5.0

        /** 候选表最多列几条（对话框可读性上限，不影响 Native 侧的全量评估） */
        private const val AI_MAX_CANDIDATES = 12

        /** 红网格实例体素数超过这个量就在状态行点名"这是合并实例"（≈2cm³ 单牙上限的数倍） */
        private const val MEGA_INSTANCE_RED_VOXELS = 4000
    }
}

/**
 * AI 面板与宿主测量页之间的唯一接口。
 *
 * 刻意不给宿主"驱动 AI"的方法：宿主只负责会话与视图，AI 的编排全在面板里；
 * 反过来面板不持有体数据、渲染窗口、Native 会话的所有权，只借句柄用。
 * 这样将来再把 AI 面板挪去别处（比如做成 Activity 或底部抽屉）只需要换宿主实现。
 */
interface CbctAiHost {
    /** 当前 Native 测量会话句柄；0 = 尚未解析序列 */
    fun aiSessionHandle(): Long

    /** 体数据概况（取证文件命名要引用它），无会话时返回 VolumeInfo.INVALID */
    fun aiVolumeInfo(): VolumeInfo

    /** AI 改动了数据之后请宿主重画叠加层 / 刷新结果列表 */
    fun aiInvalidate(overlay: Boolean, results: Boolean)

    /** 让宿主走"选择加载牙科 CBCT"的完整流程（释放 assets -> 填路径 -> 解析） */
    fun aiPickDentalCase()
}
