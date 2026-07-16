// =============================================================================
// rawpixeldeal native bridge
// 仅用于打通 Kotlin -> JNI -> OpenCV C++ 的最小调用链。
// 业务实现细节（图像增强、滤波、阈值等）后续在本文件中扩展。
// =============================================================================
#include <jni.h>
#include <string>
#include <vector>

#include <android/log.h>

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>

#define TAG "RawPixelDealJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// =============================================================================
// 1) 返回 OpenCV 版本号：验证 SO 与头文件版本一致、链接 OK
// =============================================================================
static jstring native_getOpenCVVersion(JNIEnv *env, jclass clazz) {
    const char *ver = cv::getVersionString().c_str();
    LOGI("OpenCV version: %s", ver);
    return env->NewStringUTF(ver);
}

// =============================================================================
// 2) 接受 Kotlin 传入的灰度原始像素（宽*高），构建 cv::Mat，做一次高斯模糊，
//    把结果再拷贝回 byte[] 交给 Kotlin。流程：byte[] -> Mat -> blur -> byte[]
//    同时返回前 N 个通道的像素和，方便在 Kotlin 端做"非全 0"校验。
// =============================================================================
static jintArray native_processRawGrayPixels(JNIEnv *env, jclass clazz,
                                             jint width, jint height,
                                             jbyteArray src_gray) {
    if (width <= 0 || height <= 0 || src_gray == nullptr) {
        LOGE("processRawGrayPixels: invalid args w=%d h=%d src=%p",
             width, height, src_gray);
        return nullptr;
    }

    jsize src_len = env->GetArrayLength(src_gray);
    const int expected = width * height;
    if (src_len < expected) {
        LOGE("processRawGrayPixels: src length %d < expected %d", src_len, expected);
        return nullptr;
    }

    // 1) 把 Java byte[] 拷贝到 C++ 临时缓冲
    std::vector<jbyte> jbuf(expected);
    env->GetByteArrayRegion(src_gray, 0, expected, jbuf.data());

    // 2) 构造输入 cv::Mat（CV_8UC1 灰度）
    cv::Mat src(height, width, CV_8UC1, jbuf.data());
    cv::Mat dst;

    // 3) 真正调用 OpenCV：3x3 高斯模糊
    cv::GaussianBlur(src, dst, cv::Size(3, 3), 0.0, 0.0,
                     cv::BORDER_REPLICATE);

    // 4) 计算 dst 像素总和 + 前 4 个像素值，作为最小校验信息回传
    double sum = cv::sum(dst)[0];
    jint head0 = dst.at<uchar>(0, 0);
    jint head1 = (width > 1) ? dst.at<uchar>(0, 1) : 0;
    jint head2 = (width > 2) ? dst.at<uchar>(0, 2) : 0;
    jint head3 = (height > 1 && width > 0) ? dst.at<uchar>(1, 0) : 0;

    // 5) 回填处理后的灰度像素到原 byte[] 末尾之后：先复用原 buffer 的前 expected 字节
    //    （这里用 memcpy 模拟"原数组里得到处理结果"的常见用法）
    std::memcpy(jbuf.data(), dst.data, expected);
    env->SetByteArrayRegion(src_gray, 0, expected, jbuf.data());

    LOGI("processRawGrayPixels: %dx%d, dst[0,0..1,0]=%d,%d,%d,%d, sum=%.1f",
         width, height, head0, head1, head2, head3, sum);

    jint ret[5] = {head0, head1, head2, head3, (jint) sum};
    jintArray out = env->NewIntArray(5);
    env->SetIntArrayRegion(out, 0, 5, ret);
    return out;
}

// =============================================================================
// 3) 一个独立的纯 JNI 探针：不经过 OpenCV，仅确认 JNI 注册链通畅。
//    常用于在加载 OpenCV 失败时，先确认 native-lib.cpp 自己能跑。
// =============================================================================
static jstring native_stringFromJNI(JNIEnv *env, jclass clazz) {
    LOGI("RawPixelDeal native-lib loaded");
    return env->NewStringUTF("Hello from rawpixeldeal native (OpenCV linked)");
}

// =============================================================================
// JNI Registration
// =============================================================================
static const char *const kClassName = "com/example/rawpixeldeal/jni/RawPixelDealJni";

static const JNINativeMethod kMethods[] = {
        {"stringFromJNI",
                "()Ljava/lang/String;",
                (void *) native_stringFromJNI},
        {"getOpenCVVersion",
                "()Ljava/lang/String;",
                (void *) native_getOpenCVVersion},
        {"processRawGrayPixels",
                "(II[B)[I",
                (void *) native_processRawGrayPixels},
};

extern "C" jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass clazz = env->FindClass(kClassName);
    if (clazz == nullptr) return JNI_ERR;

    if (env->RegisterNatives(clazz, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0) {
        return JNI_ERR;
    }

    LOGI("RawPixelDealJni native methods registered");
    return JNI_VERSION_1_6;
}
