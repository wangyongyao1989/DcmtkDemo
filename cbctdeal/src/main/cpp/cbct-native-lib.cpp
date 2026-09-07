#include <jni.h>
#include <string.h>
#include <errno.h>

#include <android/bitmap.h>
#include <android/log.h>

#include <string>
#include <vector>

// JNI 桥接层：仅做 JNI 类型 <-> C++ 类型转换，业务全部在
// CbctSeriesParser（纯 C++，可独立复用/测试）。DCMTK 静态库复用
// dcmtk 模块下的预编译产物（见 CMakeLists.txt），与 dcmtk 模块的
// Kotlin 层保持零依赖。
#include "include/CbctSeriesParser.h"

#define TAG "CbctNative"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Stubs for missing NDK symbols（与 dcmtk 模块 native-lib.cpp 保持一致）
extern "C" char *getlogin() { return (char *) "android"; }
extern "C" int getlogin_r(char *buf, size_t bufsize) {
    const char *user = "android";
    if (strlen(user) >= bufsize) return ERANGE;
    strcpy(buf, user);
    return 0;
}

// =============================================================================
// 辅助工具（本模块自包含，不跨模块引用头文件）
// =============================================================================

/** RAII jstring -> const char* */
class JniStr {
public:
    JniStr(JNIEnv *env, jstring str) : env_(env), jstr_(str), c_(nullptr) {
        if (jstr_) c_ = env_->GetStringUTFChars(jstr_, nullptr);
    }
    ~JniStr() {
        if (c_) env_->ReleaseStringUTFChars(jstr_, c_);
    }
    JniStr(const JniStr &) = delete;
    JniStr &operator=(const JniStr &) = delete;
    const char *c() const { return c_; }

private:
    JNIEnv *env_;
    jstring jstr_;
    const char *c_;
};

/** 容错版 NewStringUTF：用 Java 侧 UTF-8 解码替换非法字节，避免崩溃 */
static jstring SafeNewStringUTF(JNIEnv *env, const char *text) {
    if (!text) return nullptr;
    jsize len = (jsize) strlen(text);
    jbyteArray bytes = env->NewByteArray(len);
    env->SetByteArrayRegion(bytes, 0, len, (const jbyte *) text);
    jstring encoding = env->NewStringUTF("UTF-8");
    jclass strClass = env->FindClass("java/lang/String");
    jmethodID ctor = env->GetMethodID(strClass, "<init>", "([BLjava/lang/String;)V");
    jstring result = (jstring) env->NewObject(strClass, ctor, bytes, encoding);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(encoding);
    env->DeleteLocalRef(strClass);
    return result;
}

/** 用 RGBA8888 数据创建 android.graphics.Bitmap */
static jobject createRgbaBitmap(JNIEnv *env, int width, int height,
                                const std::vector<uint8_t> &rgba) {
    jclass bmpCls = env->FindClass("android/graphics/Bitmap");
    jmethodID createBmp = env->GetStaticMethodID(
            bmpCls, "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jclass cfgCls = env->FindClass("android/graphics/Bitmap$Config");
    jfieldID argbField = env->GetStaticFieldID(cfgCls, "ARGB_8888",
                                               "Landroid/graphics/Bitmap$Config;");
    jobject config = env->GetStaticObjectField(cfgCls, argbField);
    jobject bitmap = env->CallStaticObjectMethod(bmpCls, createBmp, width, height, config);
    env->DeleteLocalRef(config);
    if (!bitmap) {
        LOGE("createRgbaBitmap: createBitmap failed");
        return nullptr;
    }
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) {
        LOGE("createRgbaBitmap: getInfo failed");
        return bitmap;
    }
    void *pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) {
        LOGE("createRgbaBitmap: lockPixels failed");
        return bitmap;
    }
    memcpy(pixels, rgba.data(), rgba.size());
    AndroidBitmap_unlockPixels(env, bitmap);
    return bitmap;
}

/**
 * Java 侧进度回调适配器。
 * 解析在 native 线程池中执行，回调可能来自任意工作线程：
 * 通过 JavaVM AttachCurrentThread 按需附加，全局引用保证对象生命周期。
 */
class JniProgress {
public:
    JniProgress(JNIEnv *env, jobject callback) : vm_(nullptr), ref_(nullptr) {
        if (!callback) return;
        if (env->GetJavaVM(&vm_) == JNI_OK && vm_) {
            ref_ = env->NewGlobalRef(callback);
        }
    }
    ~JniProgress() {
        if (ref_ && vm_) {
            JNIEnv *env = nullptr;
            bool attached = false;
            if (vm_->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
                if (vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
                else env = nullptr;
            }
            if (env) env->DeleteGlobalRef(ref_);
            if (attached) vm_->DetachCurrentThread();
            ref_ = nullptr;
        }
    }
    JniProgress(const JniProgress &) = delete;
    JniProgress &operator=(const JniProgress &) = delete;

    void invoke(size_t current, size_t total) {
        if (!ref_ || !vm_) return;
        JNIEnv *env = nullptr;
        bool attached = false;
        if (vm_->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
            if (vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
            else return;
        }
        jclass cls = env->GetObjectClass(ref_);
        if (cls) {
            jmethodID mid = env->GetMethodID(cls, "onProgress", "(JJ)V");
            if (mid) {
                env->CallVoidMethod(ref_, mid, (jlong) current, (jlong) total);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            env->DeleteLocalRef(cls);
        }
        if (attached) vm_->DetachCurrentThread();
    }

private:
    JavaVM *vm_;
    jobject ref_;
};

// =============================================================================
// JNI 方法：Java com.wangyao.cbctdeal.jni.CbctJni
// =============================================================================

static const char *const kClassName = "com/wangyao/cbctdeal/jni/CbctJni";

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
static jobject buildMetaMap(JNIEnv *env, const CbctVolume *vol) {
    jclass mapClass = env->FindClass("java/util/HashMap");
    jmethodID mapInit = env->GetMethodID(mapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(mapClass, mapInit);
    jmethodID put = env->GetMethodID(mapClass, "put",
                                     "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    auto putStr = [&](const char *k, const std::string &v) {
        jstring key = SafeNewStringUTF(env, k);
        jstring val = SafeNewStringUTF(env, v.c_str());
        env->CallObjectMethod(hashMap, put, key, val);
        env->DeleteLocalRef(key);
        env->DeleteLocalRef(val);
    };
    char buf[64];
    putStr("patientName", vol->patientName);
    putStr("patientID", vol->patientID);
    putStr("patientSex", vol->patientSex);
    putStr("patientBirthDate", vol->patientBirthDate);
    putStr("studyDate", vol->studyDate);
    putStr("modality", vol->modality);
    putStr("manufacturer", vol->manufacturer);

    snprintf(buf, sizeof(buf), "%d", vol->width);
    putStr("width", buf);
    snprintf(buf, sizeof(buf), "%d", vol->height);
    putStr("height", buf);
    snprintf(buf, sizeof(buf), "%d", vol->depth);
    putStr("depth", buf);
    snprintf(buf, sizeof(buf), "%d", vol->sliceCount);
    putStr("sliceCount", buf);
    snprintf(buf, sizeof(buf), "%d", vol->skippedFiles);
    putStr("skippedFiles", buf);
    snprintf(buf, sizeof(buf), "%lld", vol->elapsedMs);
    putStr("elapsedMs", buf);

    snprintf(buf, sizeof(buf), "%.6f", vol->spacingX);
    putStr("spacingX", buf);
    snprintf(buf, sizeof(buf), "%.6f", vol->spacingY);
    putStr("spacingY", buf);
    snprintf(buf, sizeof(buf), "%.6f", vol->spacingZ);
    putStr("spacingZ", buf);
    snprintf(buf, sizeof(buf), "%.6f", vol->slope);
    putStr("slope", buf);
    snprintf(buf, sizeof(buf), "%.6f", vol->intercept);
    putStr("intercept", buf);
    snprintf(buf, sizeof(buf), "%d", vol->pixelRepresentation);
    putStr("pixelRepresentation", buf);
    snprintf(buf, sizeof(buf), "%.2f", vol->windowWidth);
    putStr("windowWidth", buf);
    snprintf(buf, sizeof(buf), "%.2f", vol->windowCenter);
    putStr("windowCenter", buf);
    snprintf(buf, sizeof(buf), "%.3f", vol->zMin);
    putStr("zMin", buf);
    snprintf(buf, sizeof(buf), "%.3f", vol->zMax);
    putStr("zMax", buf);
    return hashMap;
}

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

// JNI Registration（沿用 dcmtk 模块的动态注册风格）
static const JNINativeMethod kMethods[] = {
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

extern "C" jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass clazz = env->FindClass(kClassName);
    if (clazz == nullptr) return JNI_ERR;

    if (env->RegisterNatives(clazz, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
