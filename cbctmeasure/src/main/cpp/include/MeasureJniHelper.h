#ifndef DCMTKDEMO_MEASUREJNIHELPER_H
#define DCMTKDEMO_MEASUREJNIHELPER_H

#include <jni.h>

#include <string>
#include <vector>

#include "include/AnnotationStore.h"
#include "include/MeasurementManager.h"
// SrReport.h 不含 DCMTK 类型（只声明 POD 结构体与 bool 接口），因此 JNI 层可以
// include 它来组装 SR 请求，而不会把 dcmdata/dcmsr 头文件拖进桥接层编译单元。
#include "include/SrReport.h"

/**
 * JNI 桥接层辅助工具（对应 PRD 7.3 的 JNI Bridge，风格沿用 :cbctdeal 的
 * CbctJniHelper）。
 *
 * 分层约束（与项目一致的三层结构）：
 *   - 只有本层与 measure-native-lib.cpp 允许出现 JNIEnv / jobject；
 *   - core/ 下的业务逻辑是纯 C++，看不见 JNI，也看不见 VTK / DCMTK 类型
 *     （SrReport.cpp 是唯一的例外：它只依赖 DCMTK，被本层以字符串调用）；
 *   - 跨语言负载一律用 JSON 文本（core 已自带 Json 读写器，字段名单点在
 *     MeasurementManager / AnnotationStore 的 public static 映射里），
 *     只有叠加层的顶点用 double[] 传（每帧都要重投影，走 JSON 会白白多一次
 *     文本序列化，而点数是几十个到上千个）。
 */
namespace MeasureJniHelper {

    /** RAII jstring -> const char*（null 安全） */
    class JniStr {
    public:
        JniStr(JNIEnv *env, jstring str);

        ~JniStr();

        JniStr(const JniStr &) = delete;

        JniStr &operator=(const JniStr &) = delete;

        const char *c() const { return c_; }

    private:
        JNIEnv *env_;
        jstring jstr_;
        const char *c_;
    };

    /** 容错版 NewStringUTF：走 java.lang.String(byte[], "UTF-8")，非法字节不崩 */
    jstring SafeNewStringUTF(JNIEnv *env, const char *text);

    /** std::string -> jstring（null 安全，返回 nullptr 表示入参为空指针语义） */
    jstring toJString(JNIEnv *env, const std::string &text);

    /** double 数组：std::vector<double> -> jdoubleArray */
    jdoubleArray toJDoubleArray(JNIEnv *env, const std::vector<double> &values);

    /**
     * 一次测量会话（PRD 7.3 的 MeasurementSession）。
     *
     * 生命周期：createSession() 由 Kotlin 侧 CbctMeasureViewModel 持有 handle，
     * destroySession() 必须先行；volumePtr 指向的体数据由 :cbctdeal 分配，
     * 本结构只读引用，销毁顺序同样是"先 destroySession 再 releaseVolume"。
     *
     * 叠加层缓存：Kotlin 每次重绘都会取 overlay，而会话数据只在用户操作时变化。
     * dirty + overlaySerial 让"数据未变"的帧只付一次指针拷贝的代价：
     * refreshOverlayIfNeeded() 重建缓存并把 serial 递增，overlayVersion()
     * 让 Kotlin 端可以跳过重复投影。plane/position 参与缓存 key，
     * 因为 A-06（截面标注）与 MPR 层位绑定，换层必须重算。
     */
    struct MeasureSession {
        explicit MeasureSession(const CbctVolume *volume) {
            mgr.bindVolume(volume);
        }

        MeasurementManager mgr;
        AnnotationStore annos;

        bool dirty = true;
        int cachePlane = -999;
        int cachePosition = -999999;
        bool cacheIncludePixel = false;
        long long serial = 0;
        std::string primsJson;
        std::vector<double> worldPoints;

        /** 任何写操作后调用：使缓存失效，下一次 overlay 取值时重建 */
        void invalidate() { dirty = true; }

        /** 重建叠加层缓存（必要时），返回当前 serial */
        long long refreshOverlay(int plane, int position, bool includePixel);
    };

    /** handle -> 会话指针（0 / 非法值返回 nullptr，并写日志） */
    MeasureSession *sessionOf(jlong handle);

    /**
     * 把叠加图元序列化成"描述 JSON + 扁平顶点数组"两部分。
     * 描述里每个图元带 from/count 下标，Kotlin 只需对整批点做一次
     * projectPoints()，再按 [from, from+count) 切片绘制。
     */
    void serializeOverlay(const std::vector<OverlayPrim> &prims,
                          std::string &outJson,
                          std::vector<double> &outPoints);

    /** RoiStats -> JSON（字段名与 RoiExtractor::RoiStats 一一对应） */
    Json roiStatsToJson(const RoiStats &s);

    /** PickResult -> JSON（hit=false 时只给出描述，便于 UI 提示"未命中"） */
    Json pickResultToJson(const PickResult &r);

    /** 体数据概况 -> JSON（尺寸/体素/包围盒/序列 UID，状态栏与报告共用） */
    Json volumeInfoToJson(const MeasurementManager &mgr);

    /**
     * SR 导出请求 JSON -> SrPatientInfo。
     * req 里给了就用 req（医生可在界面上改患者姓名/编号），否则取绑定体数据的
     * DICOM 元数据；refImagePath 恒为首个切片路径（writeSrFile 据此解析证据 SOP UID）。
     */
    void fillPatientInfo(const MeasurementManager &mgr, const Json &req, SrPatientInfo &out);

    /**
     * 会话内的测量记录 -> SR 内容项（PRD 5.5.2"测量结果"章节）。
     * 概念名由 SrReport::conceptFor(type) 给出，单位沿用记录里的 unit，
     * 由 SrReport::unitCodeFor 映射成 UCUM 码。
     */
    void srItemsFromRecords(const MeasurementManager &mgr, std::vector<SrMeasureItem> &out);

}   // namespace MeasureJniHelper

#endif //DCMTKDEMO_MEASUREJNIHELPER_H
