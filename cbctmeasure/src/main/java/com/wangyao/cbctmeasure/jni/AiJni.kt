package com.wangyao.cbctmeasure.jni

import com.wangyao.cbctmeasure.model.AiCandidateInfo
import com.wangyao.cbctmeasure.model.AiResultInfo

/**
 * AI JNI（PRD 5.6 的 AiJni，功能项 AI-01 分割 / AI-03 位点推荐）。
 *
 * 与 MeasureJni / RoiJni 同一套约定：
 *   - 句柄是 Native 会话指针（long），生命周期归上层，销毁顺序见 ai/AiEngine；
 *   - 结构化结果一律 JSON 双向传递（字段名单点来自 MeasureJniHelper::aiResultToJson
 *     与 AiPlanner::candidatesJson）；
 *   - 本 object 不算任何数，只做类型搬运。
 *
 * 方法名带 native 前缀且不省略，是因为注册表在 cpp/ai-native-lib.cpp 的
 * kAiMethods[] 里按"逐字符相同"登记（RegisterNatives 不认重载与别名）；
 * 改这里的方法名必须同步改那张表，否则 JNI_OnLoad 只会在日志里给一条
 * "RegisterNatives failed"，运行期表现为 UnsatisfiedLinkError。
 *
 * 失败路径都返回 ok=false 的 JSON 而不是抛异常：AI 是增量能力，
 * 模型缺失 / ORT 装载失败时必须让测量功能继续可用。
 */
object AiJni {

    init {
        MeasureNative.ensureLoaded()
    }

    /**
     * 装载 ONNX Runtime 与模型。
     *
     * @param soPath 绝对路径，指向随 AAR 打进 APK 的 libonnxruntime.so
     *               （本模块用 dlopen 取它，原因见 cpp/ai/OrtEngine.h 顶部）
     * @param modelPath assets 里 .onnx 拷到私有目录后的绝对路径
     * @return JSON：{ok,error,runtimeInfo,inputName,outputName,modelDim,inChannels,outChannels,expectChannels}
     */
    @JvmStatic
    external fun nativeLoadModel(
        handle: Long,
        soPath: String,
        modelPath: String,
        modelName: String,
        threads: Int,
    ): String

    /** 释放 ORT 会话（必须在销毁测量会话之前调用） */
    @JvmStatic
    external fun nativeReleaseModel(handle: Long)

    /** 运行时 + 模型是否就绪（UI 用它决定 AI 按钮是否可点） */
    @JvmStatic
    external fun nativeAiReady(handle: Long): Boolean

    /**
     * 整次推理：体数据 -> 6 通道特征 -> ORT Run -> 阈值掩膜 -> 连通域 -> 牙弓归属。
     *
     * @param threshold 判定阈值（用模型标定值，见 ai/AiEngine.MODEL_THRESHOLD）
     * @param keepParity 仅在 AC-08 取证时传 true：把 feat/prob 留在会话里
     *                   供 nativeAiDumpParity 导出（约 14MB，正常使用必须 false）
     */
    @JvmStatic
    external fun nativeRunSegment(handle: Long, threshold: Double, keepParity: Boolean): String

    /** 当前结果的回执 JSON（不重算，只序列化） */
    @JvmStatic
    external fun nativeAiStatus(handle: Long): String

    /** 丢弃推理结果（掩膜叠加与 R-06 统计随之为空） */
    @JvmStatic
    external fun nativeAiClear(handle: Long)

    /** 叠加层开关：只影响绘制，不影响数据与统计 */
    @JvmStatic
    external fun nativeAiOverlayVisible(handle: Long, visible: Boolean)

    /** 每个实例 -> 一条 R-06 掩膜 ROI + M-04 体积 +（可选）M-08 骨密度；返回新建条数 */
    @JvmStatic
    external fun nativeAiAutoMeasure(handle: Long, withBoneDensity: Boolean): Int

    /** AI-03 候选位点表 JSON：{candidates:[...],summary,minGapMm} */
    @JvmStatic
    external fun nativeAiRecommend(handle: Long, minGapMm: Double, maxOut: Int): String

    /** 世界坐标 -> 落在哪个分割实例上（0 = 无）；MPR "点一下牙齿看它是第几号" */
    @JvmStatic
    external fun nativeAiPickInstance(handle: Long, x: Double, y: Double, z: Double): Int

    /** 把 feat/prob/label/inst 落成原始字节文件（AC-08 主机/真机比对用），返回结果 JSON */
    @JvmStatic
    external fun nativeAiDumpParity(handle: Long, path: String): String

    // ---- 便捷包装（JSON -> 数据类） ----

    fun runSegment(handle: Long, threshold: Double, keepParity: Boolean = false): AiResultInfo =
        AiResultInfo.parse(nativeRunSegment(handle, threshold, keepParity))

    fun status(handle: Long): AiResultInfo = AiResultInfo.parse(nativeAiStatus(handle))

    fun recommend(handle: Long, minGapMm: Double = 5.0, maxOut: Int = 12):
            Pair<List<AiCandidateInfo>, String> =
        AiCandidateInfo.parse(nativeAiRecommend(handle, minGapMm, maxOut))
}
