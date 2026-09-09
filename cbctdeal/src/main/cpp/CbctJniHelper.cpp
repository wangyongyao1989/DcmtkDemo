#include "include/CbctJniHelper.h"
#include <android/bitmap.h>
#include <android/log.h>
#include <cstring>
#include <errno.h>

#define TAG "CbctJniHelper"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// =============================================================================
// NDK 系统 Stub（与 JNI 无关，但 DCMTK 链接必需）
// =============================================================================
extern "C" char *getlogin() { return (char *) "android"; }
extern "C" int getlogin_r(char *buf, size_t bufsize) {
    const char *user = "android";
    if (strlen(user) >= bufsize) return ERANGE;
    strcpy(buf, user);
    return 0;
}

namespace CbctJniHelper {

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

    jobject createRgbaBitmap(JNIEnv *env, int width, int height,
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

    JniProgress::JniProgress(JNIEnv *env, jobject callback) : vm_(nullptr), ref_(nullptr) {
        if (!callback) return;
        if (env->GetJavaVM(&vm_) == JNI_OK && vm_) {
            ref_ = env->NewGlobalRef(callback);
        }
    }

    JniProgress::~JniProgress() {
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

    void JniProgress::invoke(size_t current, size_t total) {
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

    jobject buildMetaMap(JNIEnv *env, const CbctVolume *vol) {
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

}
