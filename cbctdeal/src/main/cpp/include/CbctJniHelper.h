#ifndef DCMTKDEMO_CBCTJNIHELPER_H
#define DCMTKDEMO_CBCTJNIHELPER_H

#include <jni.h>
#include <string>
#include <vector>
#include "CbctVolume.h"

/**
 * JNI 桥接辅助工具类。
 * 封装 JNI 类型转换、内存管理及 Java 对象构造，保持 JNI 入口层简洁。
 */
namespace CbctJniHelper {

    /** RAII jstring -> const char* */
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

    /** 容错版 NewStringUTF：处理非法 UTF-8 字节 */
    jstring SafeNewStringUTF(JNIEnv *env, const char *text);

    /** 用 RGBA8888 数据创建 android.graphics.Bitmap */
    jobject createRgbaBitmap(JNIEnv *env, int width, int height,
                            const std::vector<uint8_t> &rgba);

    /**
     * Java 侧进度回调适配器。
     * 支持跨线程回调（自动处理 Attach/DetachCurrentThread）。
     */
    class JniProgress {
    public:
        JniProgress(JNIEnv *env, jobject callback);
        ~JniProgress();
        JniProgress(const JniProgress &) = delete;
        JniProgress &operator=(const JniProgress &) = delete;

        void invoke(size_t current, size_t total);

    private:
        JavaVM *vm_;
        jobject ref_;
    };

    /** 将 C++ CbctVolume 元数据转换为 Java HashMap */
    jobject buildMetaMap(JNIEnv *env, const CbctVolume *vol);

}

#endif //DCMTKDEMO_CBCTJNIHELPER_H
