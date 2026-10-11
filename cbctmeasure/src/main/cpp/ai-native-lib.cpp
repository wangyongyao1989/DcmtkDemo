// AI-01 / AI-02 / AI-03 的 JNI 桥接（PRD 5.6，对应 Kotlin 侧 AiJni）。
//
// 分层约束与 measure-native-lib.cpp 完全一致：本文件只做
//   "取会话 -> 解析入参 -> 调 core/ai 的 C++ 接口 -> 结果转 JSON"，
// 不做任何数值计算，也不出现 ORT 类型（OrtEngine 把 ORT 藏在自己的 .cpp 里）。
// 因此"AI 崩了会不会带崩测量会话"这个问题在结构上就只有一个答案：
// 除非 JNI 层写错，否则不会 —— AI 的失败路径全部是返回 JSON 里的 ok=false。
//
// 共 20 个原生方法 = 牙齿通道 11 个 + 下颌管通道 9 个（Canal 前缀）。两条通道
// 各持一个 OrtEngine 会话（见 MeasureJniHelper.h 的 canalOrt 注释），所以下颌管
// 没有"复用同一入口换个参数"的写法：一旦共用，牙齿掩膜与管腔掩膜就不能同时在场，
// AI-03 与 S-05 会互相把对方的结果清掉。

#include <jni.h>

#include <android/log.h>

#include <cmath>
#include <string>
#include <vector>

#include "include/AiCore.h"
#include "include/AiEngine.h"
#include "include/AiPlanner.h"
#include "include/Json.h"
#include "include/MeasureJniHelper.h"
#include "include/NerveAutoTracker.h"

#define TAG "CbctMeasureAi"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace MeasureJniHelper;

static jstring jsonStr(JNIEnv *env, const Json &j) {
    return MeasureJniHelper::toJString(env, j.dump(false));
}

static Json okErr(bool ok, const std::string &err) {
    Json j = Json::makeObject();
    j.set("ok", Json::makeBool(ok));
    j.set("error", Json::makeString(err));
    return j;
}

/**
 * 每条自动下颌管是否都拿到了实测截面半径。
 *
 * 只要有一条回退到 NervePath 默认 1.5mm，就不能对医生宣称"神经距离里的半径
 * 是量出来的"（S-05 的减项口径直接受影响），所以判定取"全部"而不是"第一条"。
 */
static bool allRadiusMeasured(const std::vector<NerveTrack> &tr) {
    if (tr.empty()) return false;
    for (size_t i = 0; i < tr.size(); ++i) {
        if (tr[i].radiusMm <= 0.0) return false;
    }
    return true;
}

/**
 * nativeLoadModel(handle, soPath, modelPath, modelName, threads)
 *
 * soPath 必须是绝对路径：本模块经 dlopen 打开随 AAR 打进 APK 的
 * libonnxruntime.so（原因见 ai/OrtEngine.h 顶部），而 linker namespace 不允许
 * 用相对名打开别人的库，所以 Kotlin 侧传 applicationInfo.nativeLibraryDir 拼出来的
 * 全路径。modelPath 是 assets/models 拷到 filesDir 后的 .onnx 全路径。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeLoadModel(JNIEnv *env, jclass clazz, jlong handle,
                                                       jstring so_path, jstring model_path,
                                                       jstring model_name, jint threads) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));

    JniStr so(env, so_path);
    JniStr mp(env, model_path);
    JniStr mn(env, model_name);
    if (!so.c() || !mp.c()) return jsonStr(env, okErr(false, "soPath/modelPath 为空"));

    std::string err;
    const bool ok = s->ort.load(so.c(), mp.c(), (int) threads, err);
    s->aiReady = ok;
    if (ok) {
        s->aiModelName = mn.c() ? mn.c() : "model";
        // 模型契约校验：通道数必须与 core 的特征构造一致，否则推理阶段必然给出
        // 无意义的掩膜。放在这里提前失败，报告里能直接引用这条原因。
        int dim[3];
        if (s->ort.inputDim3(dim)) {
            Json j = okErr(true, "");
            j.set("runtimeSo", Json::makeString(s->ort.usedSo()));
            j.set("runtimeInfo", Json::makeString(s->ort.info()));
            j.set("inputName", Json::makeString(s->ort.inputName()));
            j.set("outputName", Json::makeString(s->ort.outputName()));
            Json d = Json::makeArray();
            for (int a = 0; a < 3; ++a) d.push(Json::makeNumber(dim[a]));
            j.set("modelDim", d);
            j.set("inChannels", Json::makeNumber(s->ort.inputChannels()));
            j.set("outChannels", Json::makeNumber(s->ort.outputChannels()));
            j.set("expectChannels", Json::makeNumber(AiConst::IN_CHANNELS));
            LOGD("loadModel ok: %s", s->ort.info().c_str());
            return jsonStr(env, j);
        }
    }
    LOGE("loadModel failed: %s", err.c_str());
    return jsonStr(env, okErr(false, err));
}

/** nativeReleaseModel(handle): 释放会话与运行时（destroySession 之前调用） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeReleaseModel(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    s->ort.unload();
    s->aiReady = false;
    s->parityFeat.clear();
    s->parityProb.clear();
    LOGD("releaseModel: ORT 会话已释放");
}

/** nativeAiReady(handle): 运行时 + 模型是否可用（UI 决定 AI 按钮是否可点） */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiReady(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    return (s && s->aiReady && s->mgr.hasVolume()) ? JNI_TRUE : JNI_FALSE;
}

/**
 * nativeRunSegment(handle, threshold, keepParity)
 *
 * 整条链路：体数据 -> 6 通道特征 -> OrtApi Run -> 阈值掩膜 -> 26 邻域连通域
 * -> 每牙统计（体积/质心/包围盒/HU/长轴 PCA）-> 牙弓归属。耗时三段分别计时，
 * 报告里的 PC-05（<=5s）与"推理占比"都由这几个数直接给出。
 *
 * keepParity=true 时把 feat/prob 留在会话里（约 14MB），供 nativeAiDumpParity
 * 导出与主机脚本逐元素比对；正常使用时必须 false。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeRunSegment(JNIEnv *env, jclass clazz, jlong handle,
                                                        jdouble threshold, jboolean keep_parity) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));
    if (!s->mgr.hasVolume()) return jsonStr(env, okErr(false, "尚未绑定体数据，请先加载序列"));
    if (!s->aiReady) return jsonStr(env, okErr(false, "模型未加载（nativeLoadModel 未成功）"));

    AiResult res;
    std::vector<float> tmpFeat, tmpProb;
    std::vector<float> *fp = keep_parity ? &s->parityFeat : &tmpFeat;
    std::vector<float> *pp = keep_parity ? &s->parityProb : &tmpProb;
    AiEngine::runSegment(s->mgr.volume(), s->ort, (double) threshold, res, fp, pp);
    res.modelName = s->aiModelName;
    s->mgr.adoptAiResult(res);
    s->invalidate();          // 掩膜轮廓进了叠加层，缓存必须重算

    Json j = aiResultToJson(s->mgr.ai());
    LOGD("runSegment -> ok=%d inst=%zu total=%.0fms (prep %.0f / infer %.0f / post %.0f)",
         s->mgr.ai().ok ? 1 : 0, s->mgr.ai().instances.size(),
         s->mgr.ai().totalMs, s->mgr.ai().prepMs, s->mgr.ai().inferMs, s->mgr.ai().postMs);
    return jsonStr(env, j);
}

/** nativeAiStatus(handle): 当前结果的取证 JSON（不重算，只序列化） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiStatus(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));
    Json j = aiResultToJson(s->mgr.ai());
    j.set("runtimeReady", Json::makeBool(s->aiReady));
    j.set("runtimeInfo", Json::makeString(s->aiReady ? s->ort.info() : std::string("未加载")));
    return jsonStr(env, j);
}

/** nativeAiClear(handle): 丢弃推理结果（掩膜叠加与 R-06 统计随之为空） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiClear(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    s->mgr.clearAi();
    s->parityFeat.clear();
    s->parityProb.clear();
    s->invalidate();
}

/** nativeAiOverlayVisible(handle, visible): 只影响绘制，不影响数据 */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiOverlayVisible(JNIEnv *env, jclass clazz,
                                                              jlong handle, jboolean visible) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    s->mgr.setAiOverlayVisible(visible == JNI_TRUE);
    s->invalidate();
}

/**
 * nativeAiAutoMeasure(handle, withBoneDensity)
 * 每个分割实例 -> 一条 R-06 掩膜 ROI + M-04 体积 +（可选）M-08 骨密度。
 * 返回值 = 新建的测量条数；这些记录与普通手建记录同构，因此列表/持久化/
 * PDF 报告/DICOM SR 一期就有的链路会全部自动带上它们。
 */
extern "C" JNIEXPORT jint JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiAutoMeasure(JNIEnv *env, jclass clazz, jlong handle,
                                                           jboolean with_bone_density) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return 0;
    const int n = s->mgr.aiAutoMeasure(with_bone_density == JNI_TRUE);
    s->invalidate();
    return (jint) n;
}

/** nativeAiRecommend(handle, minGapMm, maxOut): AI-03 候选表（JSON 数组） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiRecommend(JNIEnv *env, jclass clazz, jlong handle,
                                                         jdouble min_gap_mm, jint max_out) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, Json::makeArray());
    const std::vector<AiCandidate> list = s->mgr.aiRecommend((double) min_gap_mm, (int) max_out);
    Json j = Json::makeObject();
    j.set("candidates", AiPlanner::candidatesJson(list));
    j.set("summary", Json::makeString(AiPlanner::summaryText(list)));
    j.set("minGapMm", Json::makeNumber(min_gap_mm > 0 ? min_gap_mm : 5.0));
    LOGD("aiRecommend: %zu 条候选", list.size());
    return jsonStr(env, j);
}

/**
 * nativeAiPickInstance(handle, x, y, z): 世界坐标 -> 落在哪个分割实例上（0 = 无）。
 * MPR 上"点一下牙齿看它是第几号"的交互依据，也是列表反向定位的入口。
 */
extern "C" JNIEXPORT jint JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiPickInstance(JNIEnv *env, jclass clazz, jlong handle,
                                                            jdouble x, jdouble y, jdouble z) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return 0;
    const AiResult &r = s->mgr.ai();
    if (!r.hasMask()) return 0;
    const AiGrid &g = r.grid;
    int i = (int) std::floor(x / (g.spacing[0] > 1e-9 ? g.spacing[0] : 1.0));
    int j = (int) std::floor(y / (g.spacing[1] > 1e-9 ? g.spacing[1] : 1.0));
    int k = (int) std::floor(z / (g.spacing[2] > 1e-9 ? g.spacing[2] : 1.0));
    if (i < 0 || j < 0 || k < 0 || i >= g.redDim[0] || j >= g.redDim[1] || k >= g.redDim[2]) {
        return 0;
    }
    return (jint) r.inst[(size_t) g.redIndex(i, j, k)];
}

/**
 * nativeAiDumpParity(handle, path): 把真机上的 feat / prob / label / inst 落成
 * 原始字节，供主机脚本与 Python 侧同形状数组逐元素比对（AC-08）。
 * 必须是 nativeRunSegment(..., keepParity=true) 之后立刻调用。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiDumpParity(JNIEnv *env, jclass clazz, jlong handle,
                                                          jstring path) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));
    JniStr p(env, path);
    if (!p.c()) return jsonStr(env, okErr(false, "path 为空"));
    std::string err;
    const long long bytes = AiEngine::dumpParity(s->mgr.ai(), s->parityFeat, s->parityProb,
                                                 p.c(), err);
    Json j = okErr(bytes >= 0, err);
    if (bytes >= 0) {
        j.set("bytes", Json::makeNumber((double) bytes));
        j.set("featCount", Json::makeNumber((double) s->parityFeat.size()));
        j.set("probCount", Json::makeNumber((double) s->parityProb.size()));
        j.set("path", Json::makeString(p.c()));
        LOGD("aiDumpParity ok: %lld bytes -> %s", bytes, p.c());
    } else {
        LOGE("aiDumpParity failed: %s", err.c_str());
    }
    return jsonStr(env, j);
}

// =============================================================================
// AI-02 下颌管（升级建议 P-01）：第二个模型通道
//
// 这一组原生函数与牙齿那组是同一段代码路径的两次使用：runSegment / dumpParity
// 都不区分"牙"还是"管"（它们只认 AiBackend 与阈值），差别全在
//   1) 用会话里另一个 OrtEngine（canalRes 与牙齿掩膜必须能同时在场：
//      AI-03 读牙弓、S-05 读神经管，串成一个会话就得丢掉另一半）；
//   2) 后处理不是"每牙一条记录"，而是 NerveAutoTracker 把连通域追成折线，
//      再通过既有的 addNervePath 通道进会话 —— 于是叠加层、S-05 安全判级、
//      PDF 报告、DICOM SR 一期就有的链路全部自动带上它，零下游分支。
// 阈值不在这里发明：Kotlin 从 canal_cnn.json（主机在验证集上标定）读来传入。
// =============================================================================

/**
 * nativeLoadCanalModel(handle, soPath, modelPath, modelName, threads, threshold)
 *
 * 与 nativeLoadModel 同样的通道数契约校验（6 入 2 出）。差别是权重不在 assets：
 * 决策 2 要求 AI 资产"不进 APK、只在主机侧"，所以 modelPath 指向外部目录或
 * 用户选择的文件，本函数只负责 dlopen + 建会话。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeLoadCanalModel(JNIEnv *env, jclass clazz,
                                                            jlong handle, jstring so_path,
                                                            jstring model_path,
                                                            jstring model_name, jint threads,
                                                            jdouble threshold) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));

    JniStr so(env, so_path);
    JniStr mp(env, model_path);
    JniStr mn(env, model_name);
    if (!so.c() || !mp.c()) return jsonStr(env, okErr(false, "soPath/modelPath 为空"));

    std::string err;
    const bool ok = s->canalOrt.load(so.c(), mp.c(), (int) threads, err);
    s->canalReady = ok;
    if (ok) {
        s->canalModelName = mn.c() ? mn.c() : "canal_cnn";
        s->canalThreshold = (double) threshold;
        int dim[3];
        if (s->canalOrt.inputDim3(dim)) {
            Json j = okErr(true, "");
            j.set("runtimeSo", Json::makeString(s->canalOrt.usedSo()));
            j.set("runtimeInfo", Json::makeString(s->canalOrt.info()));
            j.set("modelPath", Json::makeString(mp.c()));
            j.set("inputName", Json::makeString(s->canalOrt.inputName()));
            j.set("outputName", Json::makeString(s->canalOrt.outputName()));
            Json d = Json::makeArray();
            for (int a = 0; a < 3; ++a) d.push(Json::makeNumber(dim[a]));
            j.set("modelDim", d);
            j.set("inChannels", Json::makeNumber(s->canalOrt.inputChannels()));
            j.set("outChannels", Json::makeNumber(s->canalOrt.outputChannels()));
            j.set("expectChannels", Json::makeNumber(AiConst::IN_CHANNELS));
            j.set("threshold", Json::makeNumber(s->canalThreshold));
            LOGD("loadCanalModel ok: %s threshold=%.3f", s->canalOrt.info().c_str(),
                 s->canalThreshold);
            return jsonStr(env, j);
        }
        // 通道数不匹配时把会话卸掉：留着它只会产出无意义掩膜
        s->canalOrt.unload();
        s->canalReady = false;
        err = "模型输入通道 " + std::to_string(s->canalOrt.inputChannels())
              + " != core 特征通道 " + std::to_string(AiConst::IN_CHANNELS);
    }
    LOGE("loadCanalModel failed: %s", err.c_str());
    return jsonStr(env, okErr(false, err));
}

/** nativeReleaseCanalModel(handle) */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeReleaseCanalModel(JNIEnv *env, jclass clazz,
                                                               jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    s->canalOrt.unload();
    s->canalReady = false;
    s->canalParityFeat.clear();
    s->canalParityProb.clear();
    LOGD("releaseCanalModel: 下颌管 ORT 会话已释放");
}

/** nativeCanalReady(handle): UI 决定"下颌管"按钮是否可点 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalReady(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    return (s && s->canalReady && s->mgr.hasVolume()) ? JNI_TRUE : JNI_FALSE;
}

/**
 * nativeRunCanal(handle, threshold, keepParity)
 *
 * 推理 -> 掩膜 -> NerveAutoTracker -> NervePath。返回 JSON 除了 AiResult 的
 * 取证字段（耗时/掩膜统计），还带 tracks（逐条折线点，主机脚本 AC-08-2 用它
 * 与 canal.py 的 centerlines 逐点比）与 adopted（进了会话的条数）。
 *
 * threshold<=0 时用 nativeLoadCanalModel 记下的标定值 —— 保留一个显式入参是
 * 为了真机上做"阈值敏感性"取证（同一模型不同阈值各跑一次，看追出几条）。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeRunCanal(JNIEnv *env, jclass clazz, jlong handle,
                                                      jdouble threshold, jboolean keep_parity) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));
    if (!s->mgr.hasVolume()) return jsonStr(env, okErr(false, "尚未绑定体数据，请先加载序列"));
    if (!s->canalReady) return jsonStr(env, okErr(false, "下颌管模型未加载"));

    double thr = (double) threshold;
    if (thr <= 0.0 || thr >= 1.0) thr = s->canalThreshold;

    std::vector<float> tmpFeat, tmpProb;
    std::vector<float> *fp = keep_parity ? &s->canalParityFeat : &tmpFeat;
    std::vector<float> *pp = keep_parity ? &s->canalParityProb : &tmpProb;
    AiEngine::runSegment(s->mgr.volume(), s->canalOrt, thr, s->canalRes, fp, pp);
    s->canalRes.modelName = s->canalModelName;

    Json j = aiResultToJson(s->canalRes);
    j.set("thresholdUsed", Json::makeNumber(thr));
    j.set("feedsGrading", Json::makeBool(s->mgr.autoNerveFeedsGrading()));
    if (s->canalRes.ok) {
        const int adopted = s->mgr.adoptCanalTracks(s->canalRes);
        s->invalidate();      // 神经管折线进叠加层，缓存必须重算
        const std::vector<NerveTrack> &tr = s->mgr.canalTracks();
        j.set("tracks", NerveAutoTracker::tracksJson(tr));
        j.set("adopted", Json::makeNumber(adopted));
        j.set("autoNerveCount", Json::makeNumber(s->mgr.autoNerveCount()));
        j.set("gradingNerveCount",
              Json::makeNumber((long long) s->mgr.nervesForGrading().size()));
        // 手工条数用带符号减法算：autoNerveCount 与 nervePaths() 是两处状态，
        // 万一归档后不一致，size_t 相减会回绕成一个巨大的数写进 JSON。
        const int total = (int) s->mgr.nervePaths().size();
        const int manual = total > s->mgr.autoNerveCount()
                           ? total - s->mgr.autoNerveCount() : 0;
        j.set("manualNerveCount", Json::makeNumber(manual));
        j.set("trackSummary", Json::makeString(
                NerveAutoTracker::summaryText(tr, thr, allRadiusMeasured(tr))));
    }
    // 注意：AiResult 里这个字段叫 toothVoxels（一期只有牙时定下的名字），对下颌管
    // 通道它装的就是 label==1 的"管腔体素数"。不改名是为了不牵动 aiResultToJson /
    // parity 脚本 / 已归档 planJson 的字段口径，这里用局部名把语义说清。
    const long long canalVoxels = s->canalRes.toothVoxels;
    LOGD("runCanal -> ok=%d canalVoxels=%lld thr=%.3f tracks=%d total=%.0fms "
         "(prep %.0f / infer %.0f / post %.0f)",
         s->canalRes.ok ? 1 : 0, canalVoxels, thr,
         s->mgr.autoNerveCount(), s->canalRes.totalMs, s->canalRes.prepMs,
         s->canalRes.inferMs, s->canalRes.postMs);
    return jsonStr(env, j);
}

/** nativeCanalStatus(handle): 不重算，只把当前下颌管结果与折线序列化出去 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalStatus(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));
    Json j = aiResultToJson(s->canalRes);
    j.set("runtimeReady", Json::makeBool(s->canalReady));
    j.set("runtimeInfo", Json::makeString(
            s->canalReady ? s->canalOrt.info() : std::string("未加载")));
    j.set("thresholdCalibrated", Json::makeNumber(s->canalThreshold));
    const std::vector<NerveTrack> &tr = s->mgr.canalTracks();
    j.set("tracks", NerveAutoTracker::tracksJson(tr));
    j.set("autoNerveCount", Json::makeNumber(s->mgr.autoNerveCount()));
    j.set("feedsGrading", Json::makeBool(s->mgr.autoNerveFeedsGrading()));
    j.set("gradingNerveCount", Json::makeNumber((long long) s->mgr.nervesForGrading().size()));
    j.set("trackSummary", Json::makeString(NerveAutoTracker::summaryText(
            tr, s->canalThreshold, allRadiusMeasured(tr))));
    return jsonStr(env, j);
}

/**
 * nativeCanalClear(handle): 移除自动下颌管折线（手描神经管一条都不动）。
 * 身份判定用名字前缀（AiConst::CANAL_AUTO_NOTE），与 R-06 自动记录的
 * AUTO_NOTE 同一套口径 —— 不新增 NervePath 字段，归档 planJson 因此不需要迁移。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalClear(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));
    const int removed = s->mgr.clearCanalTracks();
    s->canalRes = AiResult();
    s->canalParityFeat.clear();
    s->canalParityProb.clear();
    s->invalidate();
    Json j = okErr(true, "");
    j.set("removed", Json::makeNumber(removed));
    j.set("remainingNerves", Json::makeNumber(s->mgr.nervePaths().size()));
    LOGD("canalClear: 移除 %d 条自动下颌管，剩 %zu 条神经管",
         removed, s->mgr.nervePaths().size());
    return jsonStr(env, j);
}

/**
 * nativeCanalFeedGrading(handle, feed): 自动下颌管是否进 S-05/AI-03 判级。
 *
 * 默认 false（关闭时自动管道照样绘制、归档、进报告，只是不改种植体颜色）。
 * 这个开关的存在理由写在 MeasurementManager::setAutoNerveFeedsGrading 的注释里：
 * 主机准入门禁 MISSED，16 轴探针实测"换成自动管道后神经距离会偏 7.8mm 平均 /
 * 18.2mm 最大"，而红档判据只有 2.0mm。UI 上它是一个需要医生主动勾选的动作。
 */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalFeedGrading(JNIEnv *env, jclass clazz,
                                                              jlong handle, jboolean feed) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    s->mgr.setAutoNerveFeedsGrading(feed == JNI_TRUE);
    s->invalidate();
    LOGD("canalFeedGrading = %d（判级实际使用的神经管 %zu 条）", feed ? 1 : 0,
         s->mgr.nervesForGrading().size());
}

/** nativeCanalFeedsGrading(handle): 当前开关状态（UI 回显，避免文案与等级不一致） */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalFeedsGrading(JNIEnv *env, jclass clazz,
                                                               jlong handle) {
    MeasureSession *s = sessionOf(handle);
    return (s && s->mgr.autoNerveFeedsGrading()) ? JNI_TRUE : JNI_FALSE;
}

/** nativeCanalDumpParity(handle, path): AC-08 的下颌管版（feat/prob/label/inst） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalDumpParity(JNIEnv *env, jclass clazz,
                                                             jlong handle, jstring path) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return jsonStr(env, okErr(false, "会话句柄无效"));
    JniStr p(env, path);
    if (!p.c()) return jsonStr(env, okErr(false, "path 为空"));
    std::string err;
    const long long bytes = AiEngine::dumpParity(s->canalRes, s->canalParityFeat,
                                                 s->canalParityProb, p.c(), err);
    Json j = okErr(bytes >= 0, err);
    if (bytes >= 0) {
        j.set("bytes", Json::makeNumber((double) bytes));
        j.set("featCount", Json::makeNumber((double) s->canalParityFeat.size()));
        j.set("probCount", Json::makeNumber((double) s->canalParityProb.size()));
        j.set("path", Json::makeString(p.c()));
        LOGD("canalDumpParity ok: %lld bytes -> %s", bytes, p.c());
    } else {
        LOGE("canalDumpParity failed: %s", err.c_str());
    }
    return jsonStr(env, j);
}

// =============================================================================
// 原生方法注册表
//
// 与 measure-native-lib.cpp 的 kMeasureMethods 等五张表同一套约定：本模块
// 依赖 RegisterNatives 而不是"按名字自动解析"，因此表里的签名字符串必须与
// Kotlin object AiJni 的 external fun 逐字符一致（写错只会让本类注册失败，
// 而 Android 上 FindClass/RegisterNatives 失败是静默的 —— 表现是"点 AI 按钮
// 抛 UnsatisfiedLinkError"，所以注册结果必须打日志）。
//
// 这张表放在本文件而不是 measure-native-lib.cpp 里，是为了让 AI 层的 JNI
// 面完全自包含：删掉 ai-native-lib.cpp 时，测量功能不需要跟着改。
// =============================================================================

static const char *const kAiClass = "com/wangyao/cbctmeasure/jni/AiJni";

static const JNINativeMethod kAiMethods[] = {
        {"nativeLoadModel",       "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;I)"
                                  "Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeLoadModel},
        {"nativeReleaseModel",    "(J)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeReleaseModel},
        {"nativeAiReady",         "(J)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiReady},
        {"nativeRunSegment",      "(JDZ)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeRunSegment},
        {"nativeAiStatus",        "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiStatus},
        {"nativeAiClear",         "(J)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiClear},
        {"nativeAiOverlayVisible", "(JZ)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiOverlayVisible},
        {"nativeAiAutoMeasure",   "(JZ)I",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiAutoMeasure},
        {"nativeAiRecommend",     "(JDI)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiRecommend},
        {"nativeAiPickInstance",  "(JDDD)I",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiPickInstance},
        {"nativeAiDumpParity",    "(JLjava/lang/String;)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeAiDumpParity},
        // ---- AI-02 下颌管（P-01）：签名必须与 Kotlin object AiJni 逐字符一致 ----
        {"nativeLoadCanalModel",  "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;ID)"
                                  "Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeLoadCanalModel},
        {"nativeReleaseCanalModel", "(J)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeReleaseCanalModel},
        {"nativeCanalReady",      "(J)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalReady},
        {"nativeRunCanal",        "(JDZ)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeRunCanal},
        {"nativeCanalStatus",     "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalStatus},
        {"nativeCanalClear",      "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalClear},
        {"nativeCanalFeedGrading", "(JZ)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalFeedGrading},
        {"nativeCanalFeedsGrading", "(J)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalFeedsGrading},
        {"nativeCanalDumpParity", "(JLjava/lang/String;)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AiJni_nativeCanalDumpParity},
};

/**
 * 由 measure-native-lib.cpp 的 JNI_OnLoad 调用。
 *
 * 返回 <0 时调用方**只告警不失败**：AI 是 Phase 2 增量能力，AiJni 类缺失
 * （例如上层裁剪掉了 Kotlin 侧）不应该让整个测量库加载失败。
 */
extern "C" int CbctMeasureRegisterAiNatives(JNIEnv *env) {
    jclass clazz = env->FindClass(kAiClass);
    if (!clazz) {
        LOGW("FindClass failed: %s（AI 层不可用，测量功能不受影响）", kAiClass);
        env->ExceptionClear();
        return -1;
    }
    const int rc = env->RegisterNatives(
            clazz, kAiMethods, (int) (sizeof(kAiMethods) / sizeof(kAiMethods[0])));
    env->DeleteLocalRef(clazz);
    if (rc < 0) {
        LOGE("RegisterNatives failed for %s（签名与 Kotlin external fun 不一致）", kAiClass);
        env->ExceptionClear();
        return -1;
    }
    LOGD("registered %zu natives for %s",
         sizeof(kAiMethods) / sizeof(kAiMethods[0]), kAiClass);
    return 0;
}
