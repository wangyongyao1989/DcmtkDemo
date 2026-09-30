#include <jni.h>
#include <android/log.h>

#include <cstdint>
#include <string>
#include <vector>

// =============================================================================
// CBCT 测量与手术规划 JNI 入口（PRD 附录 B 的 MeasureJni / RoiJni /
// SurgeryPlanJni / AnnotationJni / ReportJni 五个类）。
//
// 本文件是业务代码进入 Native 的唯一门：只做
//   JNI 类型 -> C++ 类型 -> 调用 core/ -> C++ 结果 -> JSON / double[]
// 四件事，任何数值计算都不在这里发生（保证 core 可主机侧离线单测，
// 也保证 Kotlin 崩溃栈与 C++ 栈可以一一对应到 core 的函数名）。
//
// 数据负载约定（见 MeasureJniHelper.h 顶部注释）：
//   - 结构体（ROI / 种植体 / 标注）以 JSON 字符串双向传递，字段名单点
//     来源于 MeasurementManager / AnnotationStore 的 public static 映射；
//   - 叠加层顶点用 double[]（每帧重投影，走文本序列化不划算）；
//   - 会话指针以 long 传递，Kotlin 侧负责生命周期。
// =============================================================================

#include "include/MeasureJniHelper.h"
#include "include/MeasureMath.h"
#include "include/MeasurePicker.h"
#include "include/SrReport.h"

#define TAG "CbctMeasureJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace MeasureJniHelper;

static const char *const kMeasureClass = "com/wangyao/cbctmeasure/jni/MeasureJni";
static const char *const kRoiClass = "com/wangyao/cbctmeasure/jni/RoiJni";
static const char *const kPlanClass = "com/wangyao/cbctmeasure/jni/SurgeryPlanJni";
static const char *const kAnnoClass = "com/wangyao/cbctmeasure/jni/AnnotationJni";
static const char *const kReportClass = "com/wangyao/cbctmeasure/jni/ReportJni";

namespace {

    /** double[] (3*n) -> 世界坐标点列 */
    std::vector<Vec3> pointsFrom(JNIEnv *env, jdoubleArray arr) {
        std::vector<Vec3> out;
        if (!arr) return out;
        const jsize n = env->GetArrayLength(arr);
        if (n < 3 || n % 3 != 0) {
            LOGW("points array length must be a multiple of 3, got %d", (int) n);
            return out;
        }
        jdouble *v = env->GetDoubleArrayElements(arr, nullptr);
        if (!v) return out;
        out.reserve((size_t) n / 3);
        for (jsize i = 0; i < n; i += 3) out.push_back(Vec3(v[i], v[i + 1], v[i + 2]));
        env->ReleaseDoubleArrayElements(arr, v, JNI_ABORT);
        return out;
    }

    /** 点列 -> double[] (3*n) */
    jdoubleArray pointsTo(JNIEnv *env, const std::vector<Vec3> &pts) {
        std::vector<double> flat;
        flat.reserve(pts.size() * 3);
        for (size_t i = 0; i < pts.size(); ++i) {
            flat.push_back(pts[i].x);
            flat.push_back(pts[i].y);
            flat.push_back(pts[i].z);
        }
        return toJDoubleArray(env, flat);
    }

    Json parseArg(JNIEnv *env, jstring json, bool *ok) {
        JniStr s(env, json);
        if (ok) *ok = false;
        if (!s.c()) return Json::makeNull();
        Json j = Json::parse(s.c(), ok);
        if (ok && *ok == false) LOGE("request json parse failed: %s", s.c());
        return j;
    }

}   // namespace

// 便于书写的宏：Json -> jstring
#define JSON_STR(env, j) toJString((env), (j).dump(false))

// =============================================================================
// MeasureJni —— 会话、测量项、拾取、叠加层
// =============================================================================

/**
 * createSession(volumePtr): 创建测量会话并绑定 :cbctdeal 的体数据。
 * volumePtr 来自 CbctJni.loadSeries()，0 表示只建会话不绑数据
 * （允许：标注与自由曲线在预览阶段仍可工作，统计类测量会返回"待重算"）。
 */
extern "C" JNIEXPORT jlong JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_createSession(JNIEnv *env, jclass clazz,
                                                          jlong volume_ptr) {
    MeasureSession *s = new MeasureSession((const CbctVolume *) (intptr_t) volume_ptr);
    LOGD("createSession: handle=%p volume=%p", (void *) s, (void *) (intptr_t) volume_ptr);
    return (jlong) (intptr_t) s;
}

/** destroySession(handle): 释放会话（必须在 CbctJni.releaseVolume 之前调用） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_destroySession(JNIEnv *env, jclass clazz,
                                                           jlong handle) {
    MeasureSession *s = (MeasureSession *) (intptr_t) handle;
    if (!s) return;
    LOGD("destroySession: %p (%zu measures, %zu rois, %zu implants, %zu annotations)",
         (void *) s, s->mgr.measures().size(), s->mgr.rois().size(),
         s->mgr.implants().size(), s->annos.count());
    delete s;
}

/** bindVolume(handle, volumePtr): 重新解析序列后把会话挂到新的体数据上 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_bindVolume(JNIEnv *env, jclass clazz, jlong handle,
                                                       jlong volume_ptr) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    s->mgr.bindVolume((const CbctVolume *) (intptr_t) volume_ptr);
    // 换数据后所有派生量都失效：ROI 统计、种植体安全、按 ROI 的测量
    s->mgr.recomputePlan();
    for (size_t i = 0; i < s->mgr.measures().size(); ++i) {
        s->mgr.recalcMeasure(s->mgr.measures()[i].id);
    }
    s->invalidate();
    return JNI_TRUE;
}

/**
 * addMeasure(handle, type, points, roiId, name, color, note): 返回新记录 id（<=0 失败）
 * type 见 model/MeasureEnums.kt（与 core 的 MeasureType 同值）。
 */
extern "C" JNIEXPORT jint JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_addMeasure(JNIEnv *env, jclass clazz, jlong handle,
                                                       jint type, jdoubleArray points, jint roi_id,
                                                       jstring name, jint color, jstring note) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return 0;
    JniStr n(env, name);
    JniStr t(env, note);
    const std::vector<Vec3> pts = pointsFrom(env, points);
    const MeasureRecord rec = s->mgr.addMeasure(type, pts, roi_id,
                                                n.c() ? n.c() : "", (unsigned int) color,
                                                t.c() ? t.c() : "");
    s->invalidate();
    return rec.id;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_recalcMeasure(JNIEnv *env, jclass clazz, jlong handle,
                                                          jint id) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->mgr.recalcMeasure(id);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_removeMeasure(JNIEnv *env, jclass clazz, jlong handle,
                                                          jint id) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->mgr.removeMeasure(id);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_renameMeasure(JNIEnv *env, jclass clazz, jlong handle,
                                                          jint id, jstring name) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    JniStr n(env, name);
    const bool ok = s->mgr.renameMeasure(id, n.c() ? n.c() : "");
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_setMeasureNote(JNIEnv *env, jclass clazz, jlong handle,
                                                           jint id, jstring note) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    JniStr t(env, note);
    const bool ok = s->mgr.setMeasureNote(id, t.c() ? t.c() : "");
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_setMeasureVisible(JNIEnv *env, jclass clazz,
                                                              jlong handle, jint id,
                                                              jboolean visible) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->mgr.setMeasureVisible(id, visible == JNI_TRUE);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_clearMeasures(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    s->mgr.clearMeasures();
    s->invalidate();
}

/** dumpRecords(handle): PRD 8.4 的测量文件内容（records + rois），Kotlin 负责落盘 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_dumpRecords(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "{}");
    return JSON_STR(env, s->mgr.measuresJson());
}

/** dumpPlan(handle): PRD 8.4 的方案文件内容（implants + nervePaths） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_dumpPlan(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "{}");
    return JSON_STR(env, s->mgr.planJson());
}

/** restoreRecords(handle, json): 读回测量文件（内部会按当前体数据重算数值） */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_restoreRecords(JNIEnv *env, jclass clazz, jlong handle,
                                                           jstring json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    bool ok = false;
    const Json j = parseArg(env, json, &ok);
    if (!ok || !j.isObject()) return JNI_FALSE;
    const bool loaded = s->mgr.measuresFromJson(j);
    s->invalidate();
    return loaded ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_restorePlan(JNIEnv *env, jclass clazz, jlong handle,
                                                        jstring json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    bool ok = false;
    const Json j = parseArg(env, json, &ok);
    if (!ok || !j.isObject()) return JNI_FALSE;
    const bool loaded = s->mgr.planFromJson(j);
    s->invalidate();
    return loaded ? JNI_TRUE : JNI_FALSE;
}

/** dumpVolumeInfo(handle): 体数据概况（状态栏、报告页眉、SR 患者信息核对） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_dumpVolumeInfo(JNIEnv *env, jclass clazz,
                                                           jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "{\"valid\":false}");
    return JSON_STR(env, volumeInfoToJson(s->mgr));
}

/** summaryText(handle): 会话摘要（PRD 5.1.4 的"结果列表摘要"） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_summaryText(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "");
    return toJString(env, s->mgr.summaryText());
}

/**
 * pick(handle, reqJson): 屏幕射线 -> 世界点（PRD 5.1.3 点按取点）。
 * reqJson = {"ray":[ox,oy,oz,dx,dy,dz],"mode":0..4,
 *            "huThreshold":200,"skip":0,"lo":200,"hi":3000,"plane":0,"position":0,"roiId":0,
 *            "tMin":0,"tMax":0}
 *   mode 0=骨面 1=穿透(第 skip 个骨界面) 2=HU 区间(lo/hi) 3=MPR 平面(plane/position)
 *        4=ROI 表面(roiId) 5=神经管折线(nerveId/toleranceMm)
 * tMin/tMax 只对 mode 0 生效：把骨面搜索限制在沿射线的该深度区间内（拖动描记锁层）。
 * 字段语义见 core/MeasurePicker.h 的 PickRequest。
 * 返回值见 MeasureJniHelper::pickResultToJson。
 * ray 由 :cbctdeal 的 CbctVtkJni.displayToRay() 得到（vtkCoordinate 逆投影），
 * 因此拾取坐标与渲染坐标同源，这是不链接 VTK 仍能保持 <=0.1mm 精度的关键。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_pick(JNIEnv *env, jclass clazz, jlong handle,
                                                 jstring req_json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "{\"hit\":false,\"describe\":\"no session\"}");
    bool ok = false;
    const Json req = parseArg(env, req_json, &ok);
    if (!ok || !req.isObject()) {
        return toJString(env, "{\"hit\":false,\"describe\":\"bad request\"}");
    }
    const Json &ray = req.at("ray");
    if (ray.size() < 6) {
        return toJString(env, "{\"hit\":false,\"describe\":\"ray needs 6 numbers\"}");
    }
    const Vec3 o(ray[0].asNumber(), ray[1].asNumber(), ray[2].asNumber());
    const Vec3 d(ray[3].asNumber(), ray[4].asNumber(), ray[5].asNumber());

    PickRequest preq;
    preq.mode = req.at("mode").asInt(0);
    preq.huThreshold = req.at("huThreshold").asNumber(200.0);
    preq.skip = req.at("skip").asInt(0);
    preq.huLo = req.at("lo").asNumber(200.0);
    preq.huHi = req.at("hi").asNumber(3000.0);
    preq.plane = req.at("plane").asInt(MP_AXIAL);
    preq.planePosition = req.at("position").asInt(0);
    preq.roiId = req.at("roiId").asInt(0);
    preq.nerveId = req.at("nerveId").asInt(0);
    preq.toleranceMm = req.at("toleranceMm").asNumber(2.0);
    // 深度窗口（拖动描记锁层用）：0/0 = 不限，保持"第一个骨面"的原语义
    preq.tMinMm = req.at("tMin").asNumber(0.0);
    preq.tMaxMm = req.at("tMax").asNumber(0.0);

    const PickResult r = s->mgr.pickByRay(o, d, preq);
    return JSON_STR(env, pickResultToJson(r));
}

/** snapToVoxel(handle, x, y, z): 吸附到最近体素中心（消除拖动抖动） */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_snapToVoxel(JNIEnv *env, jclass clazz, jlong handle,
                                                        jdouble x, jdouble y, jdouble z) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return nullptr;
    const Vec3 p = s->mgr.snapToVoxelCenter(Vec3(x, y, z));
    const std::vector<Vec3> one(1, p);
    return pointsTo(env, one);
}

/**
 * overlayJson(handle, plane, position, includePixel): 叠加层图元描述。
 * 图元里的 from/count 是 overlayPoints() 的下标区间（单位：点，不是 double）。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_overlayJson(JNIEnv *env, jclass clazz, jlong handle,
                                                        jint plane, jint position,
                                                        jboolean include_pixel) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "{\"primCount\":0,\"prims\":[]}");
    s->refreshOverlay(plane, position, include_pixel == JNI_TRUE);
    return toJString(env, s->primsJson);
}

/** overlayPoints(handle): 叠加层全部顶点（世界 mm，扁平 3*n），交给 projectPoints 投影 */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_overlayPoints(JNIEnv *env, jclass clazz,
                                                          jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJDoubleArray(env, std::vector<double>());
    // refresh 已在 overlayJson 里做过；这里没有新数据时只走缓存
    if (s->dirty) s->refreshOverlay(s->cachePlane, s->cachePosition, s->cacheIncludePixel);
    return toJDoubleArray(env, s->worldPoints);
}

/** overlayVersion(handle): 叠加层内容版本号（Kotlin 端据此跳过重复投影） */
extern "C" JNIEXPORT jlong JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_overlayVersion(JNIEnv *env, jclass clazz,
                                                           jlong handle) {
    MeasureSession *s = sessionOf(handle);
    return s ? (jlong) s->serial : 0;
}

/** tissueName(hu): HU -> 组织类型（M-07；无会话的纯函数） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_MeasureJni_tissueName(JNIEnv *env, jclass clazz, jdouble hu) {
    return toJString(env, VolumeRef::tissueName((float) hu));
}

// =============================================================================
// RoiJni —— R-01 ~ R-05
// =============================================================================

/** addRoi(handle, roiJson): 返回新 ROI id（<=0 失败） */
extern "C" JNIEXPORT jint JNICALL
Java_com_wangyao_cbctmeasure_jni_RoiJni_addRoi(JNIEnv *env, jclass clazz, jlong handle,
                                               jstring roi_json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return 0;
    bool ok = false;
    const Json j = parseArg(env, roi_json, &ok);
    if (!ok || !j.isObject()) return 0;
    RoiDef roi;
    MeasurementManager::roiFromJson(j, roi);
    const int id = s->mgr.addRoi(roi);
    s->invalidate();
    return id;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_RoiJni_updateRoi(JNIEnv *env, jclass clazz, jlong handle,
                                                  jstring roi_json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    bool ok = false;
    const Json j = parseArg(env, roi_json, &ok);
    if (!ok || !j.isObject()) return JNI_FALSE;
    RoiDef roi;
    MeasurementManager::roiFromJson(j, roi);
    const bool updated = s->mgr.updateRoi(roi);
    s->invalidate();
    return updated ? JNI_TRUE : JNI_FALSE;
}

/**
 * roiPatch(handle, id, json): 只改 json 里出现的字段（滑杆/数值输入框实时编辑用）。
 * 直接 updateRoi 要求 Kotlin 回传完整对象，任何漏字段都会被 roiFromJson 的
 * 默认值覆盖 —— 这是 UI 侧最容易踩的坑，所以这里以现存 ROI 为底再合并。
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_RoiJni_roiPatch(JNIEnv *env, jclass clazz, jlong handle, jint id,
                                                 jstring patch_json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const RoiDef *cur = s->mgr.findRoi(id);
    if (!cur) return JNI_FALSE;
    bool ok = false;
    const Json patch = parseArg(env, patch_json, &ok);
    if (!ok) return JNI_FALSE;
    // 先按现存对象序列化，再用 patch 覆盖同名字段，最后整体解析回结构体。
    // Json 只做对象/数组读写、不暴露键枚举，因此这里用已知字段白名单；
    // RoiDef 新增字段时必须同步这张表（漏加只会在"编辑后该字段回落到默认值"时暴露）。
    Json base = MeasurementManager::roiToJson(*cur);
    base.set("id", Json::makeNumber(id));
    static const char *const kKeys[] = {"type", "name", "color", "visible", "opacity",
                                        "huMin", "huMax", "boxMin", "boxMax",
                                        "planeOrigin", "planeNormal", "sphereCenter",
                                        "sphereRadius", "childA", "childB", "childC",
                                        "opAB", "opAC", "polygon", "plane", "planePosition"};
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
        const std::string key(kKeys[i]);
        if (patch.has(key)) base.set(key, patch.at(key));
    }
    RoiDef merged;
    MeasurementManager::roiFromJson(base, merged);
    const bool updated = s->mgr.updateRoi(merged);
    s->invalidate();
    return updated ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_RoiJni_removeRoi(JNIEnv *env, jclass clazz, jlong handle, jint id) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->mgr.removeRoi(id);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_RoiJni_setRoiVisible(JNIEnv *env, jclass clazz, jlong handle,
                                                      jint id, jboolean visible) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->mgr.setRoiVisible(id, visible == JNI_TRUE);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

/** statRoi(handle, id): R-01~R-05 的量化结果（体积 cm3 / 面积 cm2 / HU 统计） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_RoiJni_statRoi(JNIEnv *env, jclass clazz, jlong handle, jint id) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "{\"ok\":false,\"error\":\"no session\"}");
    return JSON_STR(env, roiStatsToJson(s->mgr.statRoi(id)));
}

/** dominantHuRange(handle): [lo,hi] 供 :cbctdeal 的隔离显示（无阈值 ROI 返回 null） */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_wangyao_cbctmeasure_jni_RoiJni_dominantHuRange(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return nullptr;
    double lo = 0, hi = 0;
    if (!s->mgr.dominantHuRange(lo, hi)) return nullptr;
    std::vector<double> out;
    out.push_back(lo);
    out.push_back(hi);
    return toJDoubleArray(env, out);
}

/** dumpRois(handle): 单列 ROI 数组（UI 列表用；持久化仍走 dumpRecords） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_RoiJni_dumpRois(JNIEnv *env, jclass clazz, jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "[]");
    Json arr = Json::makeArray();
    const std::vector<RoiDef> &rois = s->mgr.rois();
    for (size_t i = 0; i < rois.size(); ++i) arr.push(MeasurementManager::roiToJson(rois[i]));
    return JSON_STR(env, arr);
}

// =============================================================================
// SurgeryPlanJni —— S-01 ~ S-06
// =============================================================================

extern "C" JNIEXPORT jint JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_addImplant(JNIEnv *env, jclass clazz, jlong handle,
                                                           jstring implant_json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return 0;
    bool ok = false;
    const Json j = parseArg(env, implant_json, &ok);
    if (!ok || !j.isObject()) return 0;
    Implant im;
    MeasurementManager::implantFromJson(j, im);
    const int id = s->mgr.addImplant(im);
    s->invalidate();
    return id;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_updateImplant(JNIEnv *env, jclass clazz,
                                                              jlong handle, jstring implant_json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    bool ok = false;
    const Json j = parseArg(env, implant_json, &ok);
    if (!ok || !j.isObject()) return JNI_FALSE;
    Implant im;
    MeasurementManager::implantFromJson(j, im);
    const bool updated = s->mgr.updateImplant(im);
    s->invalidate();
    return updated ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_removeImplant(JNIEnv *env, jclass clazz,
                                                              jlong handle, jint id) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->mgr.removeImplant(id);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

/** setImplantVisible(handle, id, visible)：方案列表的显示开关 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_setImplantVisible(JNIEnv *env, jclass clazz,
                                                                  jlong handle, jint id,
                                                                  jboolean visible) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const Implant *cur = s->mgr.findImplant(id);
    if (!cur) return JNI_FALSE;
    Implant im = *cur;
    im.visible = (visible == JNI_TRUE);
    const bool ok = s->mgr.updateImplant(im);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

/** recomputePlan(handle): 参数批量改动后一次性重算（保证桩体互相间距正确） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_recomputePlan(JNIEnv *env, jclass clazz,
                                                              jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    s->mgr.recomputePlan();
    s->invalidate();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_addNervePath(JNIEnv *env, jclass clazz,
                                                             jlong handle, jstring name,
                                                             jint color, jdouble radius_mm) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return 0;
    JniStr n(env, name);
    NervePath p;
    p.name = n.c() ? n.c() : "";
    p.color = (unsigned int) color;
    p.radiusMm = radius_mm;
    const int id = s->mgr.addNervePath(p);
    s->invalidate();
    return id;
}

/** appendNervePoint(handle, id, x, y, z): 神经管逐层描记（S-05 依据，M 态手势） */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_appendNervePoint(JNIEnv *env, jclass clazz,
                                                                 jlong handle, jint id,
                                                                 jdouble x, jdouble y,
                                                                 jdouble z) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->mgr.appendNervePoint(id, Vec3(x, y, z));
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_removeNervePoint(JNIEnv *env, jclass clazz,
                                                                 jlong handle, jint id,
                                                                 jint index) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const std::vector<NervePath> &paths = s->mgr.nervePaths();
    for (size_t i = 0; i < paths.size(); ++i) {
        if (paths[i].id != id) continue;
        NervePath p = paths[i];
        if (index < 0 || (size_t) index >= p.points.size()) return JNI_FALSE;
        p.points.erase(p.points.begin() + (long) index);
        const bool ok = s->mgr.updateNervePath(p);
        s->invalidate();
        return ok ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_removeNervePath(JNIEnv *env, jclass clazz,
                                                                jlong handle, jint id) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->mgr.removeNervePath(id);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_setNerveVisible(JNIEnv *env, jclass clazz,
                                                                jlong handle, jint id,
                                                                jboolean visible) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const std::vector<NervePath> &paths = s->mgr.nervePaths();
    for (size_t i = 0; i < paths.size(); ++i) {
        if (paths[i].id != id) continue;
        NervePath p = paths[i];
        p.visible = (visible == JNI_TRUE);
        const bool ok = s->mgr.updateNervePath(p);
        s->invalidate();
        return ok ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_clearPlan(JNIEnv *env, jclass clazz,
                                                          jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    while (!s->mgr.implants().empty()) s->mgr.removeImplant(s->mgr.implants().front().id);
    while (!s->mgr.nervePaths().empty()) s->mgr.removeNervePath(s->mgr.nervePaths().front().id);
    s->invalidate();
}

/**
 * safetyThresholds(): [骨高, 骨宽, 神经距离, 间距, 临界系数]
 * 与 ImplantPlanner 的常量同源，UI 文案与滑杆边界从这里取，避免两处写死。
 */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_safetyThresholds(JNIEnv *env, jclass clazz) {
    std::vector<double> out;
    out.push_back(ImplantPlanner::kMinBoneHeightMm);
    out.push_back(ImplantPlanner::kMinBoneWidthMm);
    out.push_back(ImplantPlanner::kMinNerveDistMm);
    out.push_back(ImplantPlanner::kMinSpacingMm);
    out.push_back(ImplantPlanner::kMarginalFactor);
    return toJDoubleArray(env, out);
}

// =============================================================================
// AnnotationJni —— A-01 ~ A-07
// =============================================================================

extern "C" JNIEXPORT jint JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_addAnnotation(JNIEnv *env, jclass clazz,
                                                             jlong handle, jstring anno_json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return 0;
    bool ok = false;
    const Json j = parseArg(env, anno_json, &ok);
    if (!ok || !j.isObject()) return 0;
    Annotation a;
    AnnotationStore::fromJsonItem(j, a);
    const int id = s->annos.add(a);
    s->invalidate();
    return id;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_updateAnnotation(JNIEnv *env, jclass clazz,
                                                                 jlong handle,
                                                                 jstring anno_json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    bool ok = false;
    const Json j = parseArg(env, anno_json, &ok);
    if (!ok || !j.isObject()) return JNI_FALSE;
    Annotation a;
    AnnotationStore::fromJsonItem(j, a);
    if (a.id <= 0) return JNI_FALSE;
    const bool updated = s->annos.update(a);
    s->invalidate();
    return updated ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_removeAnnotation(JNIEnv *env, jclass clazz,
                                                                jlong handle, jint id) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->annos.remove(id);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_setAnnotationText(JNIEnv *env, jclass clazz,
                                                                 jlong handle, jint id,
                                                                 jstring text) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    JniStr t(env, text);
    const bool ok = s->annos.setText(id, t.c() ? t.c() : "");
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_setAnnotationColor(JNIEnv *env, jclass clazz,
                                                                  jlong handle, jint id,
                                                                  jint color) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->annos.setColor(id, (unsigned int) color);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_setAnnotationVisible(JNIEnv *env, jclass clazz,
                                                                    jlong handle, jint id,
                                                                    jboolean visible) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    const bool ok = s->annos.setVisible(id, visible == JNI_TRUE);
    s->invalidate();
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_dumpAnnotations(JNIEnv *env, jclass clazz,
                                                               jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return toJString(env, "{}");
    return JSON_STR(env, s->annos.toJson());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_loadAnnotations(JNIEnv *env, jclass clazz,
                                                               jlong handle, jstring json) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return JNI_FALSE;
    bool ok = false;
    const Json j = parseArg(env, json, &ok);
    if (!ok || !j.isObject()) return JNI_FALSE;
    const bool loaded = s->annos.fromJson(j);
    s->invalidate();
    return loaded ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctmeasure_jni_AnnotationJni_clearAnnotations(JNIEnv *env, jclass clazz,
                                                                jlong handle) {
    MeasureSession *s = sessionOf(handle);
    if (!s) return;
    s->annos.clear();
    s->invalidate();
}

// =============================================================================
// ReportJni —— DICOM SR（PRD 5.5 / AC-09）
//
// PDF 报告不需要 JNI：Android 侧用 android.graphics.pdf.PdfDocument 直接排版，
// 图像证据用 CbctVtkJni.captureFrame() 取帧（见模块 README 的实现说明）。
// =============================================================================

/**
 * initSrDictionary(dictPath): 为本 .so 注入 DICOM 数据字典。
 * 交叉编译的 DCMTK 未内置私有字典，且本模块的 dcmDataDict 是独立副本
 * （不与 libcbct_native.so 共享），因此必须单独注入。
 * 返回空串表示成功，否则为错误文本（Kotlin 侧直接展示）。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_ReportJni_initSrDictionary(JNIEnv *env, jclass clazz,
                                                            jstring dict_path) {
    JniStr p(env, dict_path);
    std::string err;
    if (!SrReport::initDictionary(p.c() ? p.c() : "", err)) {
        LOGE("initSrDictionary failed: %s", err.c_str());
        return toJString(env, err);
    }
    return toJString(env, "");
}

/**
 * exportSr(handle, requestJson, outPath): 生成 Comprehensive SR 并落盘。
 * requestJson = {"operatorName":"...","description":"...","institution":"...",
 *                "patientName":"...","patientID":"...", ...}（缺省字段取体数据元数据）
 * 返回 {"ok":bool,"sopUid":"...","error":"..."}。
 * items / implants / nervePaths 全部由会话直接给出，Kotlin 不需要二次组装，
 * 避免"界面上的数值"与"导出文件里的数值"来自两处计算。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_ReportJni_exportSr(JNIEnv *env, jclass clazz, jlong handle,
                                                    jstring request_json, jstring out_path) {
    Json resp = Json::makeObject();
    resp.set("ok", Json::makeBool(false));
    resp.set("sopUid", Json::makeString(""));
    resp.set("error", Json::makeString(""));

    MeasureSession *s = sessionOf(handle);
    JniStr outp(env, out_path);
    if (!s || !outp.c()) {
        resp.set("error", Json::makeString(s ? "outPath 为空" : "会话不存在"));
        return JSON_STR(env, resp);
    }
    bool ok = false;
    const Json req = parseArg(env, request_json, &ok);
    SrPatientInfo info;
    fillPatientInfo(s->mgr, ok ? req : Json::makeNull(), info);

    std::vector<SrMeasureItem> items;
    srItemsFromRecords(s->mgr, items);

    std::string sopUid, err;
    const bool written = SrReport::writeSrFile(
            outp.c(), info, items, s->mgr.implants(), s->mgr.nervePaths(),
            req.at("operatorName").asString("CBCT MEASURE USER"),
            req.at("description").asString("CBCT 测量与手术规划报告"),
            sopUid, err);
    resp.set("ok", Json::makeBool(written));
    resp.set("sopUid", Json::makeString(sopUid));
    resp.set("error", Json::makeString(err));
    LOGD("exportSr -> %s ok=%d sop=%s items=%zu", outp.c(), (int) written, sopUid.c_str(),
         items.size());
    return JSON_STR(env, resp);
}

/**
 * verifySr(path): 现场自校验（AC-09 的可执行部分）。
 * 等价于第三方用 dsr2xml 打开一次：loadFile + DSRDocument::read（严格模式）+ print，
 * 返回 {"ok":bool,"docType":"...","summary":"...","error":"..."}。
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_ReportJni_verifySr(JNIEnv *env, jclass clazz, jstring path) {
    Json resp = Json::makeObject();
    JniStr p(env, path);
    std::string docType, summary, err;
    const bool ok = SrReport::verifySrFile(p.c() ? p.c() : "", docType, summary, err);
    resp.set("ok", Json::makeBool(ok));
    resp.set("docType", Json::makeString(docType));
    resp.set("summary", Json::makeString(summary));
    resp.set("error", Json::makeString(err));
    return JSON_STR(env, resp);
}

/** newSopUid(): 新建 SOP Instance UID（报告归档 / C-STORE 用） */
extern "C" JNIEXPORT jstring JNICALL
Java_com_wangyao_cbctmeasure_jni_ReportJni_newSopUid(JNIEnv *env, jclass clazz) {
    return toJString(env, SrReport::newSopInstanceUid());
}

// =============================================================================
// JNI 注册（沿用 :cbctdeal / :dcmtk 的动态注册风格：加载期即暴露签名错误，
// 比运行到才报 UnsatisfiedLinkError 更好定位）
// =============================================================================

static const JNINativeMethod kMeasureMethods[] = {
        {"createSession",      "(J)J",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_createSession},
        {"destroySession",     "(J)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_destroySession},
        {"bindVolume",         "(JJ)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_bindVolume},
        // points 是 double[]（世界坐标 3*n），签名必须是 [D；写成 [I 会让本类 RegisterNatives 全失败
        {"addMeasure",         "(JI[DILjava/lang/String;ILjava/lang/String;)I",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_addMeasure},
        {"recalcMeasure",      "(JI)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_recalcMeasure},
        {"removeMeasure",      "(JI)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_removeMeasure},
        {"renameMeasure",      "(JILjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_renameMeasure},
        {"setMeasureNote",     "(JILjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_setMeasureNote},
        {"setMeasureVisible",  "(JIZ)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_setMeasureVisible},
        {"clearMeasures",      "(J)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_clearMeasures},
        {"dumpRecords",        "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_dumpRecords},
        {"dumpPlan",           "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_dumpPlan},
        {"restoreRecords",     "(JLjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_restoreRecords},
        {"restorePlan",        "(JLjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_restorePlan},
        {"dumpVolumeInfo",     "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_dumpVolumeInfo},
        {"summaryText",        "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_summaryText},
        {"pick",               "(JLjava/lang/String;)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_pick},
        {"snapToVoxel",        "(JDDD)[D",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_snapToVoxel},
        {"overlayJson",        "(JIIZ)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_overlayJson},
        {"overlayPoints",      "(J)[D",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_overlayPoints},
        {"overlayVersion",     "(J)J",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_overlayVersion},
        {"tissueName",         "(D)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_MeasureJni_tissueName},
};

static const JNINativeMethod kRoiMethods[] = {
        {"addRoi",           "(JLjava/lang/String;)I",
                (void *) Java_com_wangyao_cbctmeasure_jni_RoiJni_addRoi},
        {"updateRoi",        "(JLjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_RoiJni_updateRoi},
        {"roiPatch",         "(JILjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_RoiJni_roiPatch},
        {"removeRoi",        "(JI)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_RoiJni_removeRoi},
        {"setRoiVisible",    "(JIZ)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_RoiJni_setRoiVisible},
        {"statRoi",          "(JI)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_RoiJni_statRoi},
        {"dominantHuRange",  "(J)[D",
                (void *) Java_com_wangyao_cbctmeasure_jni_RoiJni_dominantHuRange},
        {"dumpRois",         "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_RoiJni_dumpRois},
};

static const JNINativeMethod kPlanMethods[] = {
        {"addImplant",       "(JLjava/lang/String;)I",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_addImplant},
        {"updateImplant",    "(JLjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_updateImplant},
        {"removeImplant",    "(JI)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_removeImplant},
        {"setImplantVisible","(JIZ)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_setImplantVisible},
        {"recomputePlan",    "(J)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_recomputePlan},
        {"addNervePath",     "(JLjava/lang/String;ID)I",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_addNervePath},
        {"appendNervePoint", "(JIDDD)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_appendNervePoint},
        {"removeNervePoint", "(JII)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_removeNervePoint},
        {"removeNervePath",  "(JI)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_removeNervePath},
        {"setNerveVisible",  "(JIZ)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_setNerveVisible},
        {"clearPlan",        "(J)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_clearPlan},
        {"safetyThresholds", "()[D",
                (void *) Java_com_wangyao_cbctmeasure_jni_SurgeryPlanJni_safetyThresholds},
};

static const JNINativeMethod kAnnoMethods[] = {
        {"addAnnotation",       "(JLjava/lang/String;)I",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_addAnnotation},
        {"updateAnnotation",    "(JLjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_updateAnnotation},
        {"removeAnnotation",    "(JI)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_removeAnnotation},
        {"setAnnotationText",   "(JILjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_setAnnotationText},
        {"setAnnotationColor",  "(JII)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_setAnnotationColor},
        {"setAnnotationVisible","(JIZ)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_setAnnotationVisible},
        {"dumpAnnotations",     "(J)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_dumpAnnotations},
        {"loadAnnotations",     "(JLjava/lang/String;)Z",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_loadAnnotations},
        {"clearAnnotations",    "(J)V",
                (void *) Java_com_wangyao_cbctmeasure_jni_AnnotationJni_clearAnnotations},
};

static const JNINativeMethod kReportMethods[] = {
        {"initSrDictionary", "(Ljava/lang/String;)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_ReportJni_initSrDictionary},
        {"exportSr",         "(JLjava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_ReportJni_exportSr},
        {"verifySr",         "(Ljava/lang/String;)Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_ReportJni_verifySr},
        {"newSopUid",        "()Ljava/lang/String;",
                (void *) Java_com_wangyao_cbctmeasure_jni_ReportJni_newSopUid},
};

static int registerClass(JNIEnv *env, const char *name,
                         const JNINativeMethod *methods, size_t count) {
    jclass clazz = env->FindClass(name);
    if (!clazz) {
        LOGE("FindClass failed: %s（Kotlin 类名或包名与注册表不一致）", name);
        env->ExceptionClear();
        return -1;
    }
    if (env->RegisterNatives(clazz, methods, (int) count) < 0) {
        LOGE("RegisterNatives failed for %s", name);
        env->DeleteLocalRef(clazz);
        return -1;
    }
    env->DeleteLocalRef(clazz);
    LOGD("registered %zu natives for %s", count, name);
    return 0;
}

extern "C" jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    if (registerClass(env, kMeasureClass, kMeasureMethods,
                      sizeof(kMeasureMethods) / sizeof(kMeasureMethods[0])) < 0) return JNI_ERR;
    if (registerClass(env, kRoiClass, kRoiMethods,
                      sizeof(kRoiMethods) / sizeof(kRoiMethods[0])) < 0) return JNI_ERR;
    if (registerClass(env, kPlanClass, kPlanMethods,
                      sizeof(kPlanMethods) / sizeof(kPlanMethods[0])) < 0) return JNI_ERR;
    if (registerClass(env, kAnnoClass, kAnnoMethods,
                      sizeof(kAnnoMethods) / sizeof(kAnnoMethods[0])) < 0) return JNI_ERR;
    if (registerClass(env, kReportClass, kReportMethods,
                      sizeof(kReportMethods) / sizeof(kReportMethods[0])) < 0) return JNI_ERR;

    LOGD("cbct_measure JNI loaded（5 类 %zu 个原生方法）",
         sizeof(kMeasureMethods) / sizeof(kMeasureMethods[0]) +
         sizeof(kRoiMethods) / sizeof(kRoiMethods[0]) +
         sizeof(kPlanMethods) / sizeof(kPlanMethods[0]) +
         sizeof(kAnnoMethods) / sizeof(kAnnoMethods[0]) +
         sizeof(kReportMethods) / sizeof(kReportMethods[0]));
    return JNI_VERSION_1_6;
}
