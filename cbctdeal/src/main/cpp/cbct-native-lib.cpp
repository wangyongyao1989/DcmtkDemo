#include <jni.h>
#include <string.h>
#include <errno.h>

#include <android/bitmap.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <string>
#include <chrono>
#include <vector>

// JNI 桥接层：仅做 JNI 类型 <-> C++ 类型转换，业务全部在
// CbctSeriesParser（纯 C++，可独立复用/测试）。DCMTK 静态库复用
// dcmtk 模块下的预编译产物（见 CMakeLists.txt），与 dcmtk 模块的
// Kotlin 层保持零依赖。
#include "include/CbctSeriesParser.h"
#include "include/CbctVtkRenderer.h"
#include "include/CbctJniHelper.h"

// DCMTK 外部数据字典（交叉编译产物未内置私有字典，见 initDictionary）
#include "dcmtk/dcmdata/dcdict.h"

#define TAG "CbctNative"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace CbctJniHelper;

// =============================================================================
// JNI 方法：Java com.wangyao.cbctdeal.jni.CbctJni
// =============================================================================

static const char *const kClassName = "com/wangyao/cbctdeal/jni/CbctJni";

/**
 * CbctJni.initDictionary(dictPath): 注入外部 DICOM 数据字典（dicom.dic）。
 * 交叉编译的 DCMTK 静态库以 --without-private-dictionary 构建，
 * loadBuiltinDictionary 为空实现，JPEG/JPEG-LS 解压路径构造标准 tag
 * 时 VR 查询失败（"Tag not found in data dictionary"）。
 * 必须在 loadSeries 之前调用（引擎侧保证进程内一次）。
 */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctJni_initDictionary(JNIEnv *env, jclass clazz,
                                                     jstring dict_path) {
    JniStr p(env, dict_path);
    if (!p.c()) {
        LOGE("initDictionary: null path");
        return;
    }
    DcmDataDictionary &dict = dcmDataDict.wrlock();
    OFBool ok = dict.loadDictionary(p.c());
    dcmDataDict.wrunlock();
    if (ok) {
        LOGD("initDictionary: ok (%d entries), path=%s",
             (int) dcmDataDict.isDictionaryLoaded(), p.c());
    } else {
        LOGE("initDictionary: loadDictionary failed: %s", p.c());
    }
}

/**
 * CbctJni.loadSeries(dir, callback): 解析序列 -> Volume 指针（0 = 失败）
 */
extern "C" JNIEXPORT jlong JNICALL
Java_com_wangyao_cbctdeal_jni_CbctJni_loadSeries(JNIEnv *env, jclass clazz, jstring dir,
                                                jobject callback) {
    JniStr d(env, dir);
    JniProgress progress(env, callback);
    std::string err;
    CbctVolume *vol = CbctSeriesParser::loadSeries(
            d.c() ? d.c() : "",
            [&progress](size_t cur, size_t total) { progress.invoke(cur, total); },
            err);
    if (!vol) {
        LOGE("loadSeries failed: %s", err.c_str());
        return 0;
    }
    return (jlong) (intptr_t) vol;
}

/** 构建 Volume 元数据 flat map（由 Kotlin 侧组装为 CbctSeriesMeta） */
extern "C" JNIEXPORT jobject JNICALL
Java_com_wangyao_cbctdeal_jni_CbctJni_getVolumeMeta(JNIEnv *env, jclass clazz, jlong volume_ptr) {
    CbctVolume *vol = (CbctVolume *) (intptr_t) volume_ptr;
    if (!vol) return nullptr;
    return buildMetaMap(env, vol);
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_wangyao_cbctdeal_jni_CbctJni_extractAxialSlice(JNIEnv *env, jclass clazz,
                                                        jlong volume_ptr, jint slice_index,
                                                        jdouble ww, jdouble wc) {
    CbctVolume *vol = (CbctVolume *) (intptr_t) volume_ptr;
    if (!vol) return nullptr;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!CbctSeriesParser::extractAxial(vol, slice_index, ww, wc, rgba, w, h)) {
        return nullptr;
    }
    return createRgbaBitmap(env, w, h, rgba);
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_wangyao_cbctdeal_jni_CbctJni_extractMpr(JNIEnv *env, jclass clazz, jlong volume_ptr,
                                                jint plane, jint position,
                                                jdouble ww, jdouble wc) {
    CbctVolume *vol = (CbctVolume *) (intptr_t) volume_ptr;
    if (!vol) return nullptr;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!CbctSeriesParser::extractMpr(vol, (CbctSeriesParser::MprPlane) plane, position,
                                      ww, wc, rgba, w, h)) {
        return nullptr;
    }
    return createRgbaBitmap(env, w, h, rgba);
}

extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctJni_releaseVolume(JNIEnv *env, jclass clazz, jlong volume_ptr) {
    CbctVolume *vol = (CbctVolume *) (intptr_t) volume_ptr;
    if (vol) CbctSeriesParser::release(vol);
    LOGD("releaseVolume: %p released", (void *) vol);
}

// =============================================================================
// JNI 方法：Java com.wangyao.cbctdeal.jni.CbctVtkJni（VTK 三维渲染）
// =============================================================================

static const char *const kVtkClassName = "com/wangyao/cbctdeal/jni/CbctVtkJni";

/** 渲染器指针 <-> CbctVtkRenderer* 容错转换 */
static inline CbctVtkRenderer *asRenderer(jlong ptr) {
    return (ptr != 0) ? (CbctVtkRenderer *) (intptr_t) ptr : nullptr;
}

/**
 * CbctVtkJni.createRenderer(volumePtr): 基于 Volume 创建 VTK 渲染器（0 = 失败）。
 * 渲染器不接管 Volume 所有权：destroyRenderer 必须先于 releaseVolume 调用。
 */
extern "C" JNIEXPORT jlong JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_createRenderer(JNIEnv *env, jclass clazz,
                                                        jlong volume_ptr) {
    CbctVolume *vol = (CbctVolume *) (intptr_t) volume_ptr;
    CbctVtkRenderer *renderer = CbctVtkRenderer::create(vol);
    if (!renderer) {
        LOGE("createRenderer: failed");
        return 0;
    }
    return (jlong) (intptr_t) renderer;
}

/** Surface 可用：获取 ANativeWindow 并绑定 EGL 渲染窗口（引用计数由渲染器归还） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_onSurfaceCreated(JNIEnv *env, jclass clazz,
                                                          jlong renderer_ptr, jobject surface,
                                                          jint width, jint height) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (!renderer || !surface) return;
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);   // +1 引用
    if (!win) {
        LOGE("onSurfaceCreated: ANativeWindow_fromSurface failed");
        return;
    }
    renderer->onSurfaceCreated(win, width, height);
}

/** Surface 尺寸变化（旋转/分屏等） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_onSurfaceChanged(JNIEnv *env, jclass clazz,
                                                          jlong renderer_ptr, jint width,
                                                          jint height) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->onSurfaceChanged(width, height);
}

/** Surface 销毁：同步释放 EGL 资源并归还 ANativeWindow 引用（回调返回后 Surface 失效） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_onSurfaceDestroyed(JNIEnv *env, jclass clazz,
                                                            jlong renderer_ptr) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->onSurfaceDestroyed();
}

extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setRenderMode(JNIEnv *env, jclass clazz,
                                                       jlong renderer_ptr, jint mode) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->setRenderMode(mode);
}

extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setPlane(JNIEnv *env, jclass clazz, jlong renderer_ptr,
                                                  jint plane, jint position) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->setPlane(plane, position);
}

extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setWindowLevel(JNIEnv *env, jclass clazz,
                                                        jlong renderer_ptr, jdouble ww,
                                                        jdouble wc) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->setWindowLevel(ww, wc);
}

/** 单指滑动（VR 旋转 / MPR 平移），dx/dy 为像素位移 */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_rotate(JNIEnv *env, jclass clazz, jlong renderer_ptr,
                                                jdouble dx, jdouble dy) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->rotate(dx, dy);
}

/** 双指平移：模型位置偏移 */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_pan(JNIEnv *env, jclass clazz, jlong renderer_ptr,
                                             jdouble dx, jdouble dy) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->pan(dx, dy);
}

/** 双指捏合：factor>1 放大（相机靠近 / 平行缩放减小） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_zoom(JNIEnv *env, jclass clazz, jlong renderer_ptr,
                                              jdouble factor) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->zoom(factor);
}

extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_resetCamera(JNIEnv *env, jclass clazz,
                                                     jlong renderer_ptr) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->resetCamera();
}

/** 销毁渲染器（同步停渲染线程；必须在 releaseVolume 之前调用） */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_destroyRenderer(JNIEnv *env, jclass clazz,
                                                         jlong renderer_ptr) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) {
        renderer->destroy();   // 内部 delete this
        LOGD("destroyRenderer: %p destroyed", (void *) renderer);
    }
}


// =============================================================================
// 坐标内省与取图（供上层扩展模块叠加测量/标注图形；全部只读，不改渲染管线）
// =============================================================================

/**
 * CbctVtkJni.projectPoints(rendererPtr, DoubleArray xyz): DoubleArray?
 * 世界坐标(mm, 3*n) -> 显示坐标(像素、左上原点, 2*n)；渲染器未就绪返回 null。
 */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_projectPoints(JNIEnv *env, jclass clazz,
                                                       jlong renderer_ptr,
                                                       jdoubleArray xyz) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (!renderer || !xyz) return nullptr;
    const jsize n = env->GetArrayLength(xyz);
    if (n < 3 || n % 3 != 0) return nullptr;
    jdouble *in = env->GetDoubleArrayElements(xyz, nullptr);
    if (!in) return nullptr;
    std::vector<double> out((size_t) n / 3 * 2, 0.0);
    const bool ok = renderer->projectToDisplay(in, (int) (n / 3), out.data());
    env->ReleaseDoubleArrayElements(xyz, in, JNI_ABORT);
    if (!ok) return nullptr;
    jdoubleArray ret = env->NewDoubleArray((jsize) out.size());
    if (ret) env->SetDoubleArrayRegion(ret, 0, (jsize) out.size(), out.data());
    return ret;
}

/**
 * CbctVtkJni.displayToRay(rendererPtr, x, y): DoubleArray?
 * 显示坐标(像素、左上原点) -> 世界拾取射线 [ox,oy,oz,dx,dy,dz]（方向已单位化）。
 */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_displayToRay(JNIEnv *env, jclass clazz,
                                                      jlong renderer_ptr,
                                                      jdouble x, jdouble y) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (!renderer) return nullptr;
    double o[3] = {0, 0, 0}, d[3] = {0, 0, 0};
    if (!renderer->displayToRay(x, y, o, d)) return nullptr;
    const double out[6] = {o[0], o[1], o[2], d[0], d[1], d[2]};
    jdoubleArray ret = env->NewDoubleArray(6);
    if (ret) env->SetDoubleArrayRegion(ret, 0, 6, out);
    return ret;
}

/**
 * CbctVtkJni.displayToSliceWorld(rendererPtr, x, y): DoubleArray?
 * 显示坐标(像素、左上原点) -> 当前 MPR 切面上的世界点 [x,y,z]（mm）。
 * 非 MPR 模式或点落在切面之外返回 null（冠状/矢状面无法用射线-平面求交，见渲染器注释）。
 */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_displayToSliceWorld(JNIEnv *env, jclass clazz,
                                                             jlong renderer_ptr,
                                                             jdouble x, jdouble y) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (!renderer) return nullptr;
    double p[3] = {0, 0, 0};
    if (!renderer->displayToSliceWorld(x, y, p)) return nullptr;
    jdoubleArray ret = env->NewDoubleArray(3);
    if (ret) env->SetDoubleArrayRegion(ret, 0, 3, p);
    return ret;
}

/**
 * CbctVtkJni.getRenderSnapshot(rendererPtr): DoubleArray?
 * [0]mode [1]plane [2]position [3]w [4]h [5]ww [6]wc [7]parallelProjection
 */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_getRenderSnapshot(JNIEnv *env, jclass clazz,
                                                           jlong renderer_ptr) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (!renderer) return nullptr;
    double out[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (!renderer->getRenderSnapshot(out)) return nullptr;
    jdoubleArray ret = env->NewDoubleArray(8);
    if (ret) env->SetDoubleArrayRegion(ret, 0, 8, out);
    return ret;
}

/** CbctVtkJni.setVolumeVisible(rendererPtr, visible)：隔离显示时隐藏体数据/切面 */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setVolumeVisible(JNIEnv *env, jclass clazz,
                                                          jlong renderer_ptr,
                                                          jboolean visible) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->setVolumeVisible(visible == JNI_TRUE);
}

/** CbctVtkJni.setSegmentHuRange(rendererPtr, huMin, huMax, feather)：HU 阈值分割显示 */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setSegmentHuRange(JNIEnv *env, jclass clazz,
                                                           jlong renderer_ptr,
                                                           jdouble hu_min, jdouble hu_max,
                                                           jdouble feather) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->setSegmentHuRange(hu_min, hu_max, feather);
}

/** CbctVtkJni.resetSegmentHuRange(rendererPtr)：恢复模块默认骨窗不透明度曲线 */
extern "C" JNIEXPORT void JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_resetSegmentHuRange(JNIEnv *env, jclass clazz,
                                                             jlong renderer_ptr) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (renderer) renderer->resetSegmentHuRange();
}

/** CbctVtkJni.captureFrame(rendererPtr): Bitmap?  抓取当前帧（含 RGBA 行序翻转） */
extern "C" JNIEXPORT jobject JNICALL
Java_com_wangyao_cbctdeal_jni_CbctVtkJni_captureFrame(JNIEnv *env, jclass clazz,
                                                      jlong renderer_ptr) {
    CbctVtkRenderer *renderer = asRenderer(renderer_ptr);
    if (!renderer) return nullptr;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    const auto t0 = std::chrono::steady_clock::now();
    if (!renderer->captureFrame(rgba, w, h)) {
        LOGW("captureFrame: renderer not ready");
        return nullptr;
    }
    jobject bmp = createRgbaBitmap(env, w, h, rgba);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    LOGD("captureFrame: %dx%d in %lld ms", w, h, (long long) ms);
    return bmp;
}

// JNI Registration（沿用 dcmtk 模块的动态注册风格）
static const JNINativeMethod kMethods[] = {
        {"initDictionary",     "(Ljava/lang/String;)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctJni_initDictionary},
        {"loadSeries",         "(Ljava/lang/String;Ljava/lang/Object;)J",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctJni_loadSeries},
        {"getVolumeMeta",      "(J)Ljava/util/HashMap;",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctJni_getVolumeMeta},
        {"extractAxialSlice",  "(JIDD)Landroid/graphics/Bitmap;",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctJni_extractAxialSlice},
        {"extractMpr",         "(JIIDD)Landroid/graphics/Bitmap;",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctJni_extractMpr},
        {"releaseVolume",      "(J)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctJni_releaseVolume},
};

// CbctVtkJni 的 JNI 方法注册表（VTK 三维渲染）
static const JNINativeMethod kVtkMethods[] = {
        {"createRenderer",     "(J)J",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_createRenderer},
        {"onSurfaceCreated",   "(JLandroid/view/Surface;II)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_onSurfaceCreated},
        {"onSurfaceChanged",   "(JII)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_onSurfaceChanged},
        {"onSurfaceDestroyed", "(J)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_onSurfaceDestroyed},
        {"setRenderMode",      "(JI)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setRenderMode},
        {"setPlane",           "(JII)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setPlane},
        {"setWindowLevel",     "(JDD)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setWindowLevel},
        {"rotate",             "(JDD)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_rotate},
        {"pan",                "(JDD)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_pan},
        {"zoom",               "(JD)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_zoom},
        {"resetCamera",        "(J)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_resetCamera},
        {"destroyRenderer",    "(J)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_destroyRenderer},
        // double[] 的 JNI 签名是 "[D"，写成 "[Double;" 会让 RegisterNatives 整类失败，
        // 进而 JNI_OnLoad 返回 JNI_ERR —— 表现是 libcbct_native.so 根本加载不了（真机踩过）
        {"projectPoints",      "(J[D)[D",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_projectPoints},
        {"displayToRay",       "(JDD)[D",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_displayToRay},
        {"displayToSliceWorld", "(JDD)[D",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_displayToSliceWorld},
        {"getRenderSnapshot",  "(J)[D",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_getRenderSnapshot},
        {"setVolumeVisible",   "(JZ)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setVolumeVisible},
        {"setSegmentHuRange",  "(JDDD)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_setSegmentHuRange},
        {"resetSegmentHuRange","(J)V",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_resetSegmentHuRange},
        {"captureFrame",       "(J)Landroid/graphics/Bitmap;",
                (void *) Java_com_wangyao_cbctdeal_jni_CbctVtkJni_captureFrame},
};

extern "C" jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass clazz = env->FindClass(kClassName);
    if (clazz == nullptr) return JNI_ERR;

    if (env->RegisterNatives(clazz, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0) {
        return JNI_ERR;
    }

    jclass vtkClazz = env->FindClass(kVtkClassName);
    if (vtkClazz == nullptr) return JNI_ERR;

    if (env->RegisterNatives(vtkClazz, kVtkMethods,
                             sizeof(kVtkMethods) / sizeof(kVtkMethods[0])) < 0) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
