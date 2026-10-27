package com.example.dcmtkdemo.fragment

import android.annotation.SuppressLint
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.dcmtkdemo.databinding.FragmentCbctAiBinding
import com.wangyao.cbctmeasure.ai.AiEngine
import com.wangyao.cbctmeasure.ai.CanalEngine
import com.wangyao.cbctmeasure.jni.AiJni
import com.wangyao.cbctmeasure.jni.MeasureJni
import com.wangyao.cbctmeasure.jni.SurgeryPlanJni
import com.wangyao.cbctmeasure.model.AiArch
import com.wangyao.cbctmeasure.model.AiCandidateInfo
import com.wangyao.cbctmeasure.model.AiResultInfo
import com.wangyao.cbctmeasure.model.ImplantItem
import com.wangyao.cbctmeasure.model.NerveRiskItem
import com.wangyao.cbctmeasure.model.SafetyLevel
import com.wangyao.cbctmeasure.model.VolumeInfo
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

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

    /** AI-02 下颌管：装载回执与最近一次追踪回执（null = 还没试过外置模型） */
    private var canalModel: CanalEngine.LoadResult? = null
    private var canalInfo = CanalEngine.CanalResult()

    /** 下颌管通道是否留了 feat/prob（导出取证依赖） */
    private var canalParityKept = false

    /**
     * P-10：会话内的风险条目。每次从 Native 现取（恢复归档、换病例都要能看见真相），
     * 不在 Kotlin 侧留副本 —— 留了就会出现"界面上还有风险行，Native 早就清空了"。
     */
    private var riskItems: List<NerveRiskItem> = emptyList()

    /** 最近一次一键评估的墙钟耗时（真机取证要看它是否落在 PC 预算内） */
    private var riskAssessMs = 0L

    /** 正在把 native 的判级开关回写到勾选框（此期间忽略监听器） */
    private var syncingCanalCheck = false

    /**
     * 系统文件选择器：只为"没有 adb 的场合"准备（决策 2 把权重放在外部）。
     * 结果复制进 filesDir/ai_models 后再装载 —— native 只认绝对路径，不认 Uri。
     */
    private val pickCanalModel =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri == null) return@registerForActivityResult
            val ctx = context?.applicationContext ?: return@registerForActivityResult
            setBusy(true, "正在导入下颌管模型...")
            viewLifecycleOwner.lifecycleScope.launch {
                val msg = try {
                    val f = CanalEngine.importFromUri(ctx, uri)
                    "已导入 ${f.absolutePath}（${f.length()} B）"
                } catch (e: Exception) {
                    "导入失败：${e.message}"
                }
                if (_binding == null) return@launch
                setBusy(false, null)
                canalModel = null          // 换权重就必须重新装载，旧回执不能沿用
                toast(msg)
                refreshStatus()
            }
        }

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
        // ---- AI-02 下颌管（P-01）----
        binding.btnAiCanalPick.setOnClickListener {
            // mime 用 */* 而不是 application/octet-stream：SAF 对 .onnx 的
            // MIME 推断在各提供器上不一致，按扩展名筛反而会"选了目录却灰着"
            try {
                pickCanalModel.launch(arrayOf("*/*"))
            } catch (e: Exception) {
                toast("文件选择器不可用：${e.message}\n可改用 adb push 到 " +
                        CanalEngine.searchPathsText(requireContext().applicationContext))
            }
        }
        binding.btnAiCanalLoad.setOnClickListener { loadCanalModel() }
        binding.btnAiCanalRun.setOnClickListener { runCanal(keepParity = false) }
        binding.btnAiCanalParity.setOnClickListener { runCanal(keepParity = true) }
        binding.btnAiCanalClear.setOnClickListener { clearCanal() }
        // ---- P-10 第三磨牙 ↔ 下牙槽神经风险度量 ----
        binding.btnAiRiskAssess.setOnClickListener { assessNerveRisks() }
        binding.btnAiRiskDump.setOnClickListener { dumpNerveRiskEvidence() }
        binding.btnAiRiskClear.setOnClickListener { clearNerveRisks() }
        binding.cbAiCanalGrading.setOnCheckedChangeListener { _, checked ->
            if (syncingCanalCheck) return@setOnCheckedChangeListener   // 回显不是用户操作
            val h = sessionHandle
            if (h == 0L) return@setOnCheckedChangeListener
            CanalEngine.setFeedGrading(h, checked)
            // 立刻用 native 的回执覆盖本地状态：refreshStatus() 会以 canalInfo.feedsGrading
            // 回显勾选框，不刷新的话这里会把医生刚勾上的一格又弹回去。
            canalInfo = CanalEngine.status(h)
            // 判级输入变了必须让宿主刷新结果列表：种植体行的颜色跟着重算
            host?.aiInvalidate(overlay = true, results = true)
            refreshStatus()
            toast(if (checked)
                "已采纳自动下颌管参与 S-05/AI-03 判级（主机门禁未过，误差可达 18mm，请务必核对）"
            else "自动下颌管退回预览：仍绘制、仍归档，但不参与判级")
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
        // 下颌管跟着一起清：canalOrt 是会话的成员，旧句柄一销毁它就不存在了
        canalModel = null
        canalInfo = CanalEngine.CanalResult()
        canalParityKept = false
        if (_binding != null) {
            // 判级开关是会话内的状态，新会话回到默认（关）；界面必须跟着回显，
            // 但要走 syncCanalCheck —— 直接置 isChecked 会触发监听器去动新会话的 native 状态
            syncCanalCheck(false)
            refreshStatus()
        }
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
            // D-19：setBusy(false, null) 只收进度条、不重写状态行；
            // 这里不刷新就会一直停在「正在评估候选位点...」。
            refreshStatus()
            if (list.isEmpty()) {
                toast(summary.ifEmpty { "没有可用的缺牙间隙（牙数不足或间隙过小）" })
                return@launch
            }
            val placed = placedCandidateNames()
            val labels = list.map { c ->
                val again = if (candidateKey(c) in placed) "（已放置）" else ""
                "${c.title()}$again · 评分 ${String.format("%.0f", c.score)} · " +
                        (if (c.sku.isEmpty()) "无匹配型号" else c.sku) + " · " +
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

    /**
     * D-11：一枚 AI 候选位点的身份键。
     *
     * 候选是**确定性**的：`AiPlanner::recommend` 只在同一牙弓的相邻实例对（t, t+1）上产候选，
     * 所以 (牙弓, 前牙实例 id, 后牙实例 id) 就是这个位点唯一的标识，落进会话后就是那颗种植体的
     * 名字。用它去重，而不是"两点距离小于某个容差"——位置容差属于临床口径，本轮决策 4 明确
     * "PRD 数字不由我定"，所以这里只引入一个**零阈值**的相等判据。
     */
    private fun candidateKey(c: AiCandidateInfo): String =
        "AI推荐 ${AiArch.label(c.arch)}${c.beforeId}-${c.afterId}"

    /** 当前会话里已经存在的位点名字（每次从 Native 读，恢复归档后同样有效） */
    private fun placedCandidateNames(): Set<String> {
        val h = sessionHandle
        if (h == 0L) return emptySet()
        return SurgeryPlanJni.implantsOf(MeasureJni.dumpPlan(h)).map { it.name }.toSet()
    }

    /** 把建议位点变成一颗真正的种植体：走既有的 S-02~S-06 安全评估通道 */
    private fun placeRecommended(c: AiCandidateInfo) {
        val h = sessionHandle
        if (h == 0L) return
        val key = candidateKey(c)
        val exist = SurgeryPlanJni.implantsOf(MeasureJni.dumpPlan(h)).firstOrNull { it.name == key }
        if (exist != null) {
            Log.d(TAG, "placeRecommended: 候选「$key」已存在为 id=${exist.id}，拒绝重复放置")
            toast("该推荐位点已是 #${exist.id}，未重复放置\n（要改参数请在结果列表里编辑它）")
            return
        }
        val implant = ImplantItem(
            name = key,
            entry = c.entry,
            pitchDeg = c.pitchDeg,
            yawDeg = c.yawDeg,
            diaMm = c.diaMm,
            lengthMm = c.lengthMm,
            depthMm = c.depthMm,
            // P-09：把 AI-03 在型号库里选中的那档一起落库，报告/归档/SR 都能对上产品目录
            sku = c.sku,
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
                    "骨宽 ${String.format("%.1f", c.boneWidthMm)}mm" +
                    (if (c.sku.isEmpty()) "" else "，型号 ${c.sku}") + "）"
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
    // AI-02 下颌管编排（P-01）
    // =========================================================================

    /** 装载下颌管模型：权重从外置目录找，找不到就置灰并把找过的位置写进状态行 */
    @SuppressLint("SetTextI18n")
    private fun loadCanalModel() {
        val ctx = context?.applicationContext ?: return
        val h = sessionHandle
        if (h == 0L) {
            toast("请先解析一个 CBCT 序列")
            return
        }
        setBusy(true, "正在装载下颌管模型...")
        viewLifecycleOwner.lifecycleScope.launch {
            val r = try {
                withContext(Dispatchers.Default) { CanalEngine.load(ctx, h) }
            } catch (e: Exception) {
                Log.e(TAG, "loadCanalModel threw", e)
                CanalEngine.LoadResult(false, "装载异常: ${e.message}")
            }
            if (_binding == null) return@launch
            canalModel = r
            canalInfo = CanalEngine.CanalResult()
            setBusy(false, null)
            refreshStatus()
            if (r.ok) {
                toast("下颌管模型已就绪\n${r.thresholdSource}\n（默认只预览，不参与判级）")
            } else {
                toast("装载失败：${r.error}")
            }
        }
    }

    /**
     * 推理 + 中心线追踪。keepParity=true 时顺带导出真机 feat/prob/label/inst，
     * 主机脚本 canal_parity_export.py 拿它和 canal.py 的折线逐点比对（AC-08-2）。
     */
    @SuppressLint("SetTextI18n")
    private fun runCanal(keepParity: Boolean) {
        val h = sessionHandle
        val model = canalModel
        if (h == 0L || model == null || !model.ok) {
            toast("请先装载下颌管模型")
            return
        }
        setBusy(true, if (keepParity) "下颌管推理中（保留取证缓冲）..." else "下颌管推理中...")
        viewLifecycleOwner.lifecycleScope.launch {
            try {
                val res = CanalEngine.run(h, model.threshold, keepParity = keepParity)
                if (_binding == null) return@launch
                canalInfo = res
                canalParityKept = keepParity
                setBusy(false, null)
                refreshStatus()
                host?.aiInvalidate(overlay = true, results = true)
                when {
                    !res.ok -> toast("下颌管推理失败：${res.error}")
                    keepParity -> dumpCanalParity()
                    else -> toast(res.trackSummary.ifEmpty { res.line() })
                }
            } catch (e: Exception) {
                Log.e(TAG, "canal run failed", e)
                if (_binding != null) {
                    setBusy(false, null)
                    binding.tvAiStatus.text = "下颌管推理异常: ${e.message}"
                }
            } finally {
                if (_binding != null) setBusy(false, null)
            }
        }
    }

    /** 只清自动管道；手描神经管一条都不动（判定依据 = 名称前缀，与 R-06 同一套路） */
    @SuppressLint("SetTextI18n")
    private fun clearCanal() {
        val h = sessionHandle
        if (h == 0L) return
        val (removed, remain) = CanalEngine.clear(h)
        canalInfo = CanalEngine.CanalResult()
        canalParityKept = false
        if (_binding == null) return
        refreshStatus()
        binding.tvAiStatus.text = "${statusText()}\n已清除 $removed 条自动下颌管（剩余神经管 $remain 条，手描保留）"
        host?.aiInvalidate(overlay = true, results = true)
        toast("已清除 $removed 条自动下颌管，手描神经管保留")
    }

    /** AC-08-2 取证：把下颌管通道的 feat/prob/label/inst 落盘，路径打印出来给 adb 拉取 */
    @SuppressLint("SetTextI18n")
    private fun dumpCanalParity() {
        val ctx = context?.applicationContext ?: return
        val h = sessionHandle
        if (h == 0L || !canalParityKept) {
            toast("请先用「导出下颌管取证」跑一次推理（它会保留 feat/prob 缓冲）")
            return
        }
        val json = CanalEngine.dumpParity(ctx, h)
        val o = try {
            JSONObject(json)
        } catch (e: Exception) {
            JSONObject()
        }
        val ok = o.optBoolean("ok")
        val msg = if (ok) {
            "下颌管取证已写出：${o.optLong("bytes")} 字节" +
                    "（feat ${o.optLong("featCount")} / prob ${o.optLong("probCount")}）\n" +
                    "拉取：adb shell run-as ${ctx.packageName} cat ${o.optString("path")}"
        } else "下颌管取证失败：${o.optString("error").ifEmpty { json }}"
        Log.d(TAG, msg)
        binding.tvAiStatus.text = "${statusText()}\n$msg"
        toast(if (ok) "下颌管取证已导出" else "下颌管取证失败")
    }

    // =========================================================================
    // P-10 第三磨牙 ↔ 下牙槽神经风险度量
    // =========================================================================

    /**
     * 一键评估：下颌弓（arch==1）的全部牙根实例 × 会话里的全部神经管路径。
     *
     * 只在 UI 上做"前置条件提示 + 展示"，数值全部来自 Native：
     * 屏幕上的距离、PDF 报告里的距离、SR 里的距离必须同一个数（PRD 6 精度项的前提）。
     * 结果**不判级**（决策点 4：PRD 没给阻生牙阈值就不自定），所以这里不刷叠加层颜色，
     * 只刷新结果列表与状态行。
     */
    @SuppressLint("SetTextI18n")
    private fun assessNerveRisks() {
        val h = sessionHandle
        if (h == 0L) {
            toast("请先解析一个 CBCT 序列")
            return
        }
        val plan = MeasureJni.dumpPlan(h)
        val nerveCount = SurgeryPlanJni.nervesOf(plan).size
        if (nerveCount == 0) {
            toast("需要至少一条神经管路径：先在测量页描记，或跑「下颌管自动追踪」")
            return
        }
        if (!aiInfo.ok) {
            toast("需要 AI-01 的牙根掩膜：请先装载模型并「牙齿自动分割」")
            return
        }
        setBusy(true, "正在评估下颌弓牙根与下颌管的风险度量...")
        viewLifecycleOwner.lifecycleScope.launch {
            val t0 = SystemClock.elapsedRealtime()
            val (n, lines) = withContext(Dispatchers.Default) {
                SurgeryPlanJni.assessAllRisks(h)
            }
            if (_binding == null) return@launch
            riskAssessMs = SystemClock.elapsedRealtime() - t0
            riskItems = SurgeryPlanJni.risksOf(h)
            setBusy(false, null)
            val caveat = SurgeryPlanJni.gradingCaveat(h)
            val msg = buildString {
                append("\nP-10 风险度量：新增 $n 条（下颌弓实例 × $nerveCount 条路径，" +
                        "耗时 ${riskAssessMs}ms）")
                if (n == 0) append(" —— 没有可评估的组合：会话里没有下颌弓（arch=1）的分割实例，" +
                        "或该实例的掩膜不可用")
                lines.take(4).forEach { append("\n · $it") }
                if (lines.size > 4) append("\n · 其余 ${lines.size - 4} 条见「导出风险取证」")
                append("\n · $caveat")
            }
            Log.d(TAG, msg.trim())
            binding.tvAiStatus.text = statusText() + msg
            host?.aiInvalidate(overlay = false, results = true)
            updateButtons()
            toast(if (n > 0) "已评估 $n 条（只给几何事实，不判级）" else "没有可评估的组合")
        }
    }

    /**
     * 取证导出：把每条风险的原始字段（含 FOV/间距/剂量口径/判读说明）写成 JSON 文件。
     *
     * 之所以单独导一份而不是只看 plan.json：风险条目是会话内对象，恢复别的归档会被覆盖；
     * 真机回归要核对"屏幕上的 2.4mm 与文件里的 2.4mm 是不是同一个数"，就得有个不动的副本。
     */
    @SuppressLint("SetTextI18n")
    private fun dumpNerveRiskEvidence() {
        val ctx = context?.applicationContext ?: return
        val h = sessionHandle
        if (h == 0L) return
        riskItems = SurgeryPlanJni.risksOf(h)
        if (riskItems.isEmpty()) {
            toast("还没有风险条目：先点「下颌管风险评估」")
            return
        }
        val info = host?.aiVolumeInfo() ?: VolumeInfo.INVALID
        val caseId = info.seriesDescription.ifEmpty { info.seriesInstanceUID }.ifEmpty { "unknown" }
        val safeCase = caseId.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val dir = File(ctx.filesDir, "nerve_risk").apply { mkdirs() }
        val file = File(dir, "risk_$safeCase.json")
        val msg = try {
            file.writeText(SurgeryPlanJni.dumpNerveRisks(h))
            "风险取证已写出：${riskItems.size} 条 / ${file.length()} 字节\n" +
                    "拉取：adb shell run-as ${ctx.packageName} cat ${file.absolutePath}"
        } catch (e: Exception) {
            "风险取证写入失败：${e.message}"
        }
        Log.d(TAG, msg)
        binding.tvAiStatus.text = "${statusText()}\n$msg"
        toast(if (msg.startsWith("风险取证已写出")) "已导出 ${file.name}" else "导出失败")
    }

    /** 清空风险条目：只影响 P-10，神经管描记与种植体一条都不动 */
    @SuppressLint("SetTextI18n")
    private fun clearNerveRisks() {
        val h = sessionHandle
        if (h == 0L) return
        val before = SurgeryPlanJni.risksOf(h).size
        SurgeryPlanJni.clearNerveRisks(h)
        riskItems = emptyList()
        refreshStatus()
        binding.tvAiStatus.text = "${statusText()}\n已清除 $before 条 P-10 风险条目（神经管与种植体未改动）"
        toast("已清除 $before 条风险条目")
    }

    // =========================================================================
    // 状态行 / 按钮可用性
    // =========================================================================

    /** 状态栏：牙齿段 + 下颌管段（报告与真机取证引用同一份措辞） */
    private fun statusText(): String = aiStatusText() + canalStatusText() + riskStatusText()

    /**
     * P-10 段落：条数、每条的一行结论、以及那句"没有判级阈值"。
     * 判读口径直接引用 Native 的 gradingCaveat —— 三处（UI/报告/SR）同一句话，
     * 才不会出现在 UI 上写"仅供参考"、在报告里却像结论的情况。
     */
    private fun riskStatusText(): String {
        if (riskItems.isEmpty()) return ""
        val sb = StringBuilder()
        sb.append("\n风险度量（P-10）：${riskItems.size} 条（最近一次评估 ${riskAssessMs}ms）")
        riskItems.take(4).forEach { sb.append("\n · ${it.summary()}") }
        if (riskItems.size > 4) sb.append("\n · 其余 ${riskItems.size - 4} 条见报告")
        sb.append("\n · ${riskItems.first().gradingCaveat}")
        return sb.toString()
    }

    /** 下颌管段落：装载来源、阈值出处、追踪结果、是否参与判级，四件事逐行说 */
    @SuppressLint("SetTextI18n")
    private fun canalStatusText(): String {
        val ctx = context?.applicationContext
        val sb = StringBuilder()
        val model = canalModel
        if (model == null) {
            sb.append("\n下颌管（AI-02）：未装载。权重不进 APK（决策 2），")
            if (ctx != null) sb.append("外置路径 ${CanalEngine.searchPathsText(ctx)}")
            return sb.toString()
        }
        if (!model.ok) {
            sb.append("\n下颌管（AI-02）：装载失败 ${model.error}")
            sb.append("\n已找 ${model.searchedPaths}")
            return sb.toString()
        }
        sb.append("\n下颌管（AI-02）：${model.modelPath}")
        sb.append("\n${model.thresholdSource}")
        if (!model.channelsMatch()) {
            sb.append("（通道不匹配：模型 ${model.inChannels} vs 特征 ${model.expectChannels}）")
        }
        val info = canalInfo
        if (info.ok) {
            sb.append("\n${info.line()}")
            info.tracks.forEachIndexed { i, t ->
                sb.append("\n · #${i + 1} ${t.voxels}体素 半径${"%.2f".format(t.radiusMm)}mm " +
                        "轴${when (t.traceAxis) { 0 -> "x"; 1 -> "y"; else -> "z" }} " +
                        "${t.points.size}点/${"%.1f".format(t.lengthMm())}mm")
            }
            if (info.trackSummary.isNotEmpty()) sb.append("\n${info.trackSummary}")
            sb.append("\n判级：${if (info.feedsGrading) "已采纳自动下颌管" else "未采纳（预览）"}" +
                    "，实际参与判级的神经管 ${info.gradingNerveCount} 条")
            sb.append("\n耗时 预处理 %.0f + 推理 %.0f + 后处理 %.0f = %.0f ms；新增内存约 %.1f MB".format(
                info.prepMs, info.inferMs, info.postMs, info.totalMs, info.allocBytes / 1048576.0))
        } else if (info.error.isNotEmpty()) {
            sb.append("\n最近一次追踪：${info.error}")
        }
        return sb.toString()
    }

    /** 牙齿段（原名 statusText；拆出来是为了让下颌管能追加而不重写这段逻辑） */
    private fun aiStatusText(): String {
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

        // ---- 下颌管通道（P-01 / AI-02）----
        // 这四个按钮和判级勾选框在 XML 里都是 enabled=false，只有这里会翻它们。
        // 真机走查抓到过一次"整条 P-01 只能靠 JNI 调"的缺陷（D-13）：牙齿通道
        // 的 enable 逻辑写完就收工了，下颌管那五个控件从没被刷新过，装载完牙齿
        // 模型也只能看着一排灰按钮 —— 所以新增控件必须同时新增它的可用性判定。
        binding.btnAiCanalLoad.isEnabled = !busy && sessionHandle != 0L
        val canalReady = canalModel?.ok == true
        binding.btnAiCanalRun.isEnabled = !busy && canalReady
        binding.btnAiCanalParity.isEnabled = !busy && canalReady
        // 清除：有会话就可能留着上一轮的自动管道（换病例时 native 会跟着会话一起清，
        // 但同一会话内跑过一轮之后医生要能立刻撤掉），无结果时点它只是回执 0 条
        binding.btnAiCanalClear.isEnabled = !busy && sessionHandle != 0L
        // 判级开关只在"会话里真的有自动管道"时可点：否则勾上它什么也不改变，
        // 却会让状态行出现"已采纳自动下颌管"这种无主语的句子。
        binding.cbAiCanalGrading.isEnabled = !busy && canalInfo.autoNerveCount > 0

        // ---- P-10 风险度量 ----
        // 评估需要"掩膜 + 至少一条神经管"，两者都不满足时按钮仍然可点（点了会给出具体的
        // 那一条原因），但导出/清除只在真有条目时可点：没有条目时它们是空动作。
        binding.btnAiRiskAssess.isEnabled = !busy && sessionHandle != 0L
        binding.btnAiRiskDump.isEnabled = !busy && sessionHandle != 0L && riskItems.isNotEmpty()
        binding.btnAiRiskClear.isEnabled = !busy && sessionHandle != 0L && riskItems.isNotEmpty()
    }

    @SuppressLint("SetTextI18n")
    private fun refreshStatus() {
        if (_binding == null) return
        // 风险条目每次从 Native 现取：测量页可能刚恢复了一份归档（里面带 nerveRisks），
        // 也可能换了序列（Native 随会话清空）。本地缓存会让 AI 页显示上一份方案的风险。
        riskItems = if (sessionHandle != 0L) SurgeryPlanJni.risksOf(sessionHandle) else emptyList()
        binding.tvAiStatus.text = statusText()
        // 判级勾选框以 native 回执为真值回显（canalInfo.feedsGrading）：
        // 否则"清除自动管道 -> 重新追踪"之后界面还留着上一次勾过的样子，
        // 而 native 侧的开关是另一个状态，文案与颜色就会互相矛盾。
        syncCanalCheck(canalInfo.feedsGrading)
        updateButtons()
    }

    /**
     * 单向同步判级勾选框（native -> UI），并且不让这次回写触发监听器。
     *
     * 监听器里会调 nativeCanalFeedGrading + recomputePlan + 弹 toast；
     * 换病例时如果由回写触发，医生会看到一句无来由的"自动下颌管退回预览"。
     */
    private fun syncCanalCheck(checked: Boolean) {
        if (_binding == null || syncingCanalCheck) return
        if (binding.cbAiCanalGrading.isChecked == checked) return
        syncingCanalCheck = true
        binding.cbAiCanalGrading.isChecked = checked
        syncingCanalCheck = false
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
