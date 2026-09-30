// AI-01 / AI-03 的 JNI 桥接（PRD 5.6，对应 Kotlin 侧 AiJni）。
//
// 分层约束与 measure-native-lib.cpp 完全一致：本文件只做
//   "取会话 -> 解析入参 -> 调 core/ai 的 C++ 接口 -> 结果转 JSON"，
// 不做任何数值计算，也不出现 ORT 类型（OrtEngine 把 ORT 藏在自己的 .cpp 里）。
// 因此"AI 崩了会不会带崩测量会话"这个问题在结构上就只有一个答案：
// 除非 JNI 层写错，否则不会 —— AI 的失败路径全部是返回 JSON 里的 ok=false。

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
