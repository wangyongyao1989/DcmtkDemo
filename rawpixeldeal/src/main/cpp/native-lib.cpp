// =============================================================================
// rawpixeldeal native bridge
// 仅用于打通 Kotlin -> JNI -> OpenCV C++ 的最小调用链。
// 业务实现细节（图像增强、滤波、阈值等）后续在本文件中扩展。
// =============================================================================
#include <jni.h>
#include <string>
#include <vector>
#include <cstring>

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
// 3) 把 assets 中的"原始像素数据缓冲"（Data610.bin / Data622.bin）经
//    OpenCV 处理后输出为可直接填入 Android Bitmap.ARGB_8888 的 RGBA 字节。
//
//    流程（与"做人工筛查"的需求对齐）：
//      src16 bytes -> cv::Mat(CV_16UC1) -> min/max 归一化到 8-bit
//                  -> 可选 CLAHE(对比度增强) -> 可选中心裁剪(去除空白边框)
//                  -> COLOR_GRAY2RGBA -> jbyteArray  (R,G,B,A 内存布局，
//                     与 Android Bitmap.copyPixelsFromBuffer 兼容)
//
//    参数：
//      width, height    原图宽高（像素）
//      bitDepth         8 或 16
//      srcBytes         原始字节，little-endian
//      cropLeft/Top/Right/Bottom  四周要裁掉的像素数（>=0），传 0 表示不裁
//      enableClahe      1=做 CLAHE，0=不做
//      clipLimit        CLAHE 的 clipLimit（仅 enableClahe=1 时生效，<=0 走默认 2.0）
//      tileSize         CLAHE 的 tile 边长（仅 enableClahe=1 时生效，<=0 走默认 8）
//
//    返回：
//      jbyteArray，长度 = outW * outH * 4（RGBA8888）。失败返回 nullptr。
//      数组前 8 个 int 作为 [outW, outH, srcMin, srcMax, srcMean/10, cropLeft, cropTop, cropRight]
//      一并放在返回值最前面？——这里为简化，返回一个独立的 IntArray 头信息 + RGBA bytes
//      通过 out 参数 jintArray head（长度 4）回传 [outW, outH, srcMin, srcMax]。
// =============================================================================
static jbyteArray native_processRawToRgba(JNIEnv *env, jclass clazz,
                                          jint width, jint height,
                                          jint bitDepth,
                                          jbyteArray srcBytes,
                                          jint cropLeft, jint cropTop,
                                          jint cropRight, jint cropBottom,
                                          jint enableClahe,
                                          jdouble clipLimit, jint tileSize,
                                          jintArray head) {
    if (width <= 0 || height <= 0 || srcBytes == nullptr) {
        LOGE("processRawToRgba: invalid args w=%d h=%d src=%p",
             width, height, srcBytes);
        return nullptr;
    }
    if (bitDepth != 8 && bitDepth != 16) {
        LOGE("processRawToRgba: unsupported bitDepth=%d (only 8/16)", bitDepth);
        return nullptr;
    }

    const int channels = (bitDepth == 16) ? 2 : 1;
    const jsize src_len = env->GetArrayLength(srcBytes);
    const size_t expected = (size_t) width * (size_t) height * (size_t) channels;
    if ((size_t) src_len < expected) {
        LOGE("processRawToRgba: src length %d < expected %zu", src_len, expected);
        return nullptr;
    }

    // 1) 拷到本地缓冲（保留 little-endian 原样）
    std::vector<unsigned char> raw(expected);
    env->GetByteArrayRegion(srcBytes, 0, (jsize) expected,
                            reinterpret_cast<jbyte *>(raw.data()));

    // 2) 构造输入 Mat
    cv::Mat src;
    if (bitDepth == 16) {
        src = cv::Mat(height, width, CV_16UC1, raw.data());
    } else {
        src = cv::Mat(height, width, CV_8UC1, raw.data());
    }

    // 3) 8-bit 视图
    cv::Mat gray8;
    if (bitDepth == 16) {
        // min/max 归一化到 0~255，便于人工筛查观察全动态范围
        double minV = 0.0, maxV = 0.0;
        cv::minMaxLoc(src, &minV, &maxV);
        // OpenCV 4.x 用 convertTo 带掩码做线性拉伸
        src.convertTo(gray8, CV_8UC1,
                      255.0 / std::max(1.0, (maxV - minV)),
                      -minV * 255.0 / std::max(1.0, (maxV - minV)));
        LOGI("processRawToRgba: 16-bit min=%.1f max=%.1f -> 8-bit", minV, maxV);

        // 回写 head（srcMin, srcMax, etc.），由 Kotlin 端做文字展示
        if (head != nullptr && env->GetArrayLength(head) >= 4) {
            jint hOut[4] = {
                    (jint) minV,
                    (jint) maxV,
                    (jint) src.cols,
                    (jint) src.rows,
            };
            env->SetIntArrayRegion(head, 0, 4, hOut);
        }
    } else {
        gray8 = src.clone();
        if (head != nullptr && env->GetArrayLength(head) >= 4) {
            jint hOut[4] = {0, 255, width, height};
            env->SetIntArrayRegion(head, 0, 4, hOut);
        }
    }

    // 4) 可选 CLAHE：增强局部对比，便于观察组织结构
    if (enableClahe != 0) {
        double clip = (clipLimit > 0.0) ? clipLimit : 2.0;
        int tile = (tileSize > 0) ? tileSize : 8;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clip, cv::Size(tile, tile));
        cv::Mat enhanced;
        clahe->apply(gray8, enhanced);
        gray8 = enhanced;
        LOGI("processRawToRgba: CLAHE clip=%.2f tile=%d", clip, tile);
    }

    // 5) 可选中心裁剪（按四周要裁掉的像素数）。一般用来去掉传感器空白边。
    int cl = std::max(0, cropLeft);
    int ct = std::max(0, cropTop);
    int cr = std::max(0, cropRight);
    int cb = std::max(0, cropBottom);
    if (cl + cr >= gray8.cols || ct + cb >= gray8.rows) {
        LOGE("processRawToRgba: crop too large w=%d h=%d cl=%d cr=%d ct=%d cb=%d",
             gray8.cols, gray8.rows, cl, cr, ct, cb);
        return nullptr;
    }
    cv::Mat cropped;
    if (cl > 0 || ct > 0 || cr > 0 || cb > 0) {
        cv::Rect roi(cl, ct, gray8.cols - cl - cr, gray8.rows - ct - cb);
        cropped = gray8(roi).clone();
        LOGI("processRawToRgba: crop LTRB=%d,%d,%d,%d -> %dx%d",
             cl, ct, cr, cb, cropped.cols, cropped.rows);
    } else {
        cropped = gray8;
    }

    // 6) 灰度 -> RGBA（与 Android Bitmap ARGB_8888 内存布局一致：R,G,B,A）
    cv::Mat rgba;
    cv::cvtColor(cropped, rgba, cv::COLOR_GRAY2RGBA);

    // 7) 拷贝到 jbyteArray 返回
    const size_t outBytes = (size_t) rgba.total() * rgba.elemSize();
    jbyteArray out = env->NewByteArray((jsize) outBytes);
    if (out == nullptr) return nullptr;
    env->SetByteArrayRegion(out, 0, (jsize) outBytes,
                            reinterpret_cast<const jbyte *>(rgba.data));
    return out;
}

// =============================================================================
// 4) 一个独立的纯 JNI 探针：不经过 OpenCV，仅确认 JNI 注册链通畅。
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
        {"processRawToRgba",
                "(III[BIIIIIDI[I)[B",
                (void *) native_processRawToRgba},
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
