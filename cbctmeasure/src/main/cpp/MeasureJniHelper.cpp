#include "include/MeasureJniHelper.h"

#include <android/log.h>

#include <cstring>
#include <errno.h>

// JNI 桥接层辅助实现：只做"JNI 类型 <-> core 类型"的转换与叠加层缓存，
// 不做任何测量计算（计算全部在 core/ 里，可主机侧离线单测）。

#define TAG "CbctMeasureJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// =============================================================================
// NDK 系统 stub（与 JNI 无关，但 DCMTK 静态库链接必需，沿用 :cbctdeal 的做法）
// =============================================================================
extern "C" char *getlogin() { return (char *) "android"; }

extern "C" int getlogin_r(char *buf, size_t bufsize) {
    const char *user = "android";
    if (strlen(user) >= bufsize) return ERANGE;
    strcpy(buf, user);
    return 0;
}

namespace MeasureJniHelper {

    JniStr::JniStr(JNIEnv *env, jstring str) : env_(env), jstr_(str), c_(nullptr) {
        if (jstr_) c_ = env_->GetStringUTFChars(jstr_, nullptr);
    }

    JniStr::~JniStr() {
        if (c_) env_->ReleaseStringUTFChars(jstr_, c_);
    }

    jstring SafeNewStringUTF(JNIEnv *env, const char *text) {
        if (!text) return nullptr;
        jsize len = (jsize) strlen(text);
        jbyteArray bytes = env->NewByteArray(len);
        if (!bytes) return nullptr;
        env->SetByteArrayRegion(bytes, 0, len, (const jbyte *) text);
        jstring encoding = env->NewStringUTF("UTF-8");
        jclass strClass = env->FindClass("java/lang/String");
        jmethodID ctor = env->GetMethodID(strClass, "<init>", "([BLjava/lang/String;)V");
        jstring result = nullptr;
        if (encoding && strClass && ctor) {
            result = (jstring) env->NewObject(strClass, ctor, bytes, encoding);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                result = env->NewStringUTF(text);   // 退化为宽松版，非法字节被替换
            }
        }
        if (bytes) env->DeleteLocalRef(bytes);
        if (encoding) env->DeleteLocalRef(encoding);
        if (strClass) env->DeleteLocalRef(strClass);
        return result;
    }

    jstring toJString(JNIEnv *env, const std::string &text) {
        return SafeNewStringUTF(env, text.c_str());
    }

    jdoubleArray toJDoubleArray(JNIEnv *env, const std::vector<double> &values) {
        jdoubleArray arr = env->NewDoubleArray((jsize) values.size());
        if (arr && !values.empty()) {
            env->SetDoubleArrayRegion(arr, 0, (jsize) values.size(), values.data());
        }
        return arr;
    }

    // -------------------------------------------------------------------------
    // 叠加层：图元描述 + 扁平世界坐标点
    // -------------------------------------------------------------------------

    void serializeOverlay(const std::vector<OverlayPrim> &prims,
                          std::string &outJson,
                          std::vector<double> &outPoints) {
        outPoints.clear();
        Json root = Json::makeObject();
        Json arr = Json::makeArray();
        for (size_t i = 0; i < prims.size(); ++i) {
            const OverlayPrim &p = prims[i];
            const int from = (int) (outPoints.size() / 3);
            for (size_t k = 0; k < p.world.size(); ++k) {
                outPoints.push_back(p.world[k].x);
                outPoints.push_back(p.world[k].y);
                outPoints.push_back(p.world[k].z);
            }
            Json j = Json::makeObject();
            j.set("kind", Json::makeNumber(p.kind));
            // 颜色按无符号 0xAARRGGBB 输出；Kotlin 侧 optLong(...).toInt() 还原
            j.set("color", Json::makeNumber((double) (unsigned int) p.color));
            j.set("width", Json::makeNumber(p.widthPx));
            j.set("fill", Json::makeBool(p.fill));
            j.set("arrow", Json::makeNumber(p.arrowHead));
            j.set("label", Json::makeNumber(p.labelAnchored));
            j.set("text", Json::makeString(p.text));
            j.set("from", Json::makeNumber(from));
            j.set("count", Json::makeNumber((double) p.world.size()));
            // 选中高亮用：Kotlin 用 (owner, ownerId) 精确匹配，不再靠 text 子串
            j.set("owner", Json::makeNumber(p.ownerKind));
            j.set("ownerId", Json::makeNumber(p.ownerId));
            arr.push(j);
        }
        root.set("primCount", Json::makeNumber((double) prims.size()));
        root.set("pointCount", Json::makeNumber((double) (outPoints.size() / 3)));
        root.set("prims", arr);
        outJson = root.dump(false);
    }

    long long MeasureSession::refreshOverlay(int plane, int position, bool includePixel) {
        const bool argsChanged = plane != cachePlane || position != cachePosition
                                 || includePixel != cacheIncludePixel;
        if (dirty || argsChanged) {
            std::vector<OverlayPrim> prims;
            mgr.buildOverlay(prims);
            // 种植体/神经计划在 buildPlanOverlay 里（PRD 5.3）：不调用它就画不出植入体，
            // 这是"叠加层只显示测量线"那类问题的根因，务必三个来源都构建。
            mgr.buildPlanOverlay(prims);
            annos.buildOverlay(prims, plane, position, includePixel);
            serializeOverlay(prims, primsJson, worldPoints);
            cachePlane = plane;
            cachePosition = position;
            cacheIncludePixel = includePixel;
            dirty = false;
            ++serial;
        }
        return serial;
    }

    MeasureSession *sessionOf(jlong handle) {
        MeasureSession *s = (MeasureSession *) (intptr_t) handle;
        if (!s) LOGE("session handle is 0（会话已销毁或从未创建）");
        return s;
    }

    // -------------------------------------------------------------------------
    // 结果对象 -> JSON
    // -------------------------------------------------------------------------

    Json roiStatsToJson(const RoiStats &s) {
        Json j = Json::makeObject();
        j.set("ok", Json::makeBool(s.ok));
        j.set("error", Json::makeString(s.error));
        j.set("coverageVoxels", Json::makeNumber(s.coverageVoxels));
        j.set("fullVoxels", Json::makeNumber((double) s.fullVoxels));
        j.set("scannedVoxels", Json::makeNumber((double) s.scannedVoxels));
        j.set("elapsedMs", Json::makeNumber((double) s.elapsedMs));
        j.set("voxelMm3", Json::makeNumber(s.voxelMm3));
        j.set("volumeMm3", Json::makeNumber(s.volumeMm3));
        j.set("volumeCm3", Json::makeNumber(s.volumeCm3));
        j.set("meanHu", Json::makeNumber(s.meanHu));
        j.set("sdHu", Json::makeNumber(s.sdHu));
        j.set("minHu", Json::makeNumber(s.minHu));
        j.set("maxHu", Json::makeNumber(s.maxHu));
        j.set("hasHu", Json::makeBool(s.hasHu));
        j.set("areaMm2", Json::makeNumber(s.areaMm2));
        j.set("areaCm2", Json::makeNumber(s.areaCm2));
        return j;
    }

    Json pickResultToJson(const PickResult &r) {
        Json j = Json::makeObject();
        j.set("hit", Json::makeBool(r.hit));
        Json p = Json::makeArray();
        p.push(Json::makeNumber(r.point.x));
        p.push(Json::makeNumber(r.point.y));
        p.push(Json::makeNumber(r.point.z));
        j.set("point", p);
        Json vc = Json::makeArray();
        vc.push(Json::makeNumber(r.voxelCenter.x));
        vc.push(Json::makeNumber(r.voxelCenter.y));
        vc.push(Json::makeNumber(r.voxelCenter.z));
        j.set("voxelCenter", vc);
        Json idx = Json::makeArray();
        idx.push(Json::makeNumber(r.index[0]));
        idx.push(Json::makeNumber(r.index[1]));
        idx.push(Json::makeNumber(r.index[2]));
        j.set("index", idx);
        j.set("hu", Json::makeNumber(r.hu));
        j.set("distanceMm", Json::makeNumber(r.distanceMm));
        j.set("describe", Json::makeString(r.describe));
        return j;
    }

    Json volumeInfoToJson(const MeasurementManager &mgr) {
        const VolumeRef &v = mgr.volume();
        Json j = Json::makeObject();
        j.set("valid", Json::makeBool(v.valid()));
        if (!v.valid()) return j;
        j.set("width", Json::makeNumber(v.width()));
        j.set("height", Json::makeNumber(v.height()));
        j.set("depth", Json::makeNumber(v.depth()));
        j.set("spacingX", Json::makeNumber(v.spacingX()));
        j.set("spacingY", Json::makeNumber(v.spacingY()));
        j.set("spacingZ", Json::makeNumber(v.spacingZ()));
        j.set("voxelVolumeMm3", Json::makeNumber(v.voxelVolumeMm3()));
        j.set("maxDiagonalMm", Json::makeNumber(v.maxDiagonalMm()));
        Vec3 lo, hi;
        v.boundsMin(lo);
        v.boundsMax(hi);
        Json mn = Json::makeArray();
        mn.push(Json::makeNumber(lo.x));
        mn.push(Json::makeNumber(lo.y));
        mn.push(Json::makeNumber(lo.z));
        j.set("boundsMin", mn);
        Json mx = Json::makeArray();
        mx.push(Json::makeNumber(hi.x));
        mx.push(Json::makeNumber(hi.y));
        mx.push(Json::makeNumber(hi.z));
        j.set("boundsMax", mx);
        j.set("studyInstanceUID", Json::makeString(v.studyInstanceUID()));
        j.set("seriesInstanceUID", Json::makeString(v.seriesInstanceUID()));
        j.set("patientName", Json::makeString(v.patientName()));
        j.set("patientID", Json::makeString(v.patientID()));
        j.set("patientSex", Json::makeString(v.patientSex()));
        j.set("patientBirthDate", Json::makeString(v.patientBirthDate()));
        j.set("studyDate", Json::makeString(v.studyDate()));
        j.set("seriesDescription", Json::makeString(v.seriesDescription()));
        j.set("manufacturer", Json::makeString(v.manufacturer()));
        j.set("firstSlicePath", Json::makeString(v.firstSlicePath()));
        return j;
    }

    void fillPatientInfo(const MeasurementManager &mgr, const Json &req, SrPatientInfo &out) {
        const VolumeRef &v = mgr.volume();
        // req 里给了就用 req（医生可以在导出前改名/补编号），否则回落到 DICOM 元数据。
        // 无体数据（异常会话）时 VolumeRef 的各访问器返回空串，SrReport 会自行兜底。
        out.patientName = req.at("patientName").asString(v.patientName());
        out.patientID = req.at("patientID").asString(v.patientID());
        out.patientBirthDate = req.at("patientBirthDate").asString(v.patientBirthDate());
        out.sex = req.at("sex").asString(v.patientSex());
        out.studyUID = req.at("studyUID").asString(v.studyInstanceUID());
        out.seriesUID = req.at("seriesUID").asString(v.seriesInstanceUID());
        out.studyDate = req.at("studyDate").asString(v.studyDate());
        out.studyDescription = req.at("studyDescription").asString(v.seriesDescription());
        out.institution = req.at("institution").asString(v.manufacturer());
        out.refSopClassUID = req.at("refSopClassUID").asString();
        out.refSopUID = req.at("refSopUID").asString();
        out.refSeriesUID = req.at("refSeriesUID").asString(out.seriesUID);
        out.refImagePath = req.at("refImagePath").asString(v.firstSlicePath());
    }

    void srItemsFromRecords(const MeasurementManager &mgr, std::vector<SrMeasureItem> &out) {
        out.clear();
        const std::vector<MeasureRecord> &recs = mgr.measures();
        out.reserve(recs.size());
        for (size_t i = 0; i < recs.size(); ++i) {
            const MeasureRecord &r = recs[i];
            SrMeasureItem item;
            item.concept = SrReport::conceptFor(r.type);
            item.name = r.name;
            item.value = r.value;
            item.unit = r.unit;
            item.note = r.note;
            out.push_back(item);
        }
    }

}   // namespace MeasureJniHelper
