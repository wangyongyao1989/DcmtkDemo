#include <jni.h>
#include <vector>
#include <climits>
#include <android/log.h>
#include <android/bitmap.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "include/MedicalCTPreprocess.h"
#include "include/JniHelper.h"
#include "include/CtSeriesProcessor.h"

#define TAG "RawPixelDealJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// =============================================================================
// 辅助与核心逻辑
// =============================================================================

// 把已 lockPixels 的 Bitmap 内存包成 cv::Mat。必须显式传入 AndroidBitmapInfo.stride：
// Android Bitmap 的行末可能有填充，用默认紧凑步长会导致整幅图像逐行错位。
static bool wrapBitmapMat(const AndroidBitmapInfo &info, void *pixels, cv::Mat &out) {
    if (info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {
        out = cv::Mat(info.height, info.width, CV_8UC4, pixels, info.stride);
        return true;
    }
    if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
        out = cv::Mat(info.height, info.width, CV_8UC2, pixels, info.stride);
        return true;
    }
    return false;
}

// 目标 Bitmap 同理；getInfo 失败时返回空 Mat，调用方据此放弃写入。
static cv::Mat wrapBitmapDst(JNIEnv *env, jobject bitmap, void *pixels) {
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) return cv::Mat();
    return cv::Mat(info.height, info.width, CV_8UC4, pixels, info.stride);
}

/**
 * 统计/调窗用的数据横轴范围。
 *
 * 32F HU 域必须用百分位而不是绝对 min/max：后处理（尤其是不锐化掩模）会在 float HU
 * 上制造 ±30 万量级的过冲离群点，绝对跨度会让 500 个 bin 每个宽达 1400 HU，
 * 全部组织挤进 1~2 个 bin，波峰检测彻底失效，窗宽退化成一整段跨度（实测 W≈51897），
 * 显示效果就是灰白一片。
 */
static void histogramRange(const cv::Mat &mat, const cv::Rect &roi,
                           double &minV, double &maxV) {
    cv::minMaxLoc(mat, &minV, &maxV);
    if (mat.depth() != CV_32F || roi.area() <= 0) return;
    std::vector<cv::Mat> slices = {const_cast<cv::Mat &>(mat)};
    float pMin = 0.0f, pMax = 0.0f;
    CtSeriesProcessor::computePercentileHu(slices, roi, 8, 0.5, 99.5, pMin, pMax);
    if (pMax > pMin) {
        minV = pMin;
        maxV = pMax;
    }
}

static cv::Mat normalizeTo8u(const cv::Mat &mat) {
    cv::Mat out8u;
    double mn = 0.0, mx = 0.0;
    histogramRange(mat, cv::Rect(0, 0, mat.cols, mat.rows), mn, mx);
    const double span = std::max(1e-7, mx - mn);
    mat.convertTo(out8u, CV_8U, 255.0 / span, -mn * 255.0 / span);
    return out8u;
}

/**
 * 调窗映射逻辑：将高动态范围的原始数据(HU)线性映射到 8-bit 可视化空间。
 */
static cv::Mat windowTo8u(const cv::Mat &mat, int windowMethod) {
    const int nBins = (windowMethod == 6) ? 500 : 256;
    std::vector<int> hist;
    const std::vector<int> *histPtr = nullptr;

    // 为了让直方图统计更准确，先进行自动人体 ROI 裁剪
    cv::Rect roi(0, 0, mat.cols, mat.rows);
    if (mat.depth() == CV_32F) {
        roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(mat, -600.0f, 5, 1000, 10);
    }

    double minV = 0.0, maxV = 0.0;
    histogramRange(mat, roi, minV, maxV);

    // 统计 ROI 区域内的直方图
    std::vector<cv::Mat> slices = {const_cast<cv::Mat &>(mat)};
    CtSeriesProcessor::aggregateSeriesHistogram(slices, roi, minV, maxV, nBins, 1, hist);
    histPtr = &hist;

    // 调用核心算法计算窗宽窗位
    double c = 127.5, w = 255.0;
    CtSeriesProcessor::pickWindowCenterWidth(windowMethod, minV, maxV, histPtr, nBins, c, w);
    if (w < 1.0) w = 1.0;

    // 执行最终的线性映射转换
    return CtSeriesProcessor::applyWindow8u(mat, c, w, 0);
}

/**
 * 算子执行器：所有分发路径共用一份实现。
 * params 指向该算子在平铺参数数组中的起始位置，个数见 opParamCount。
 */
static cv::Mat runOp(cv::Mat mat, int opId, const jdouble *params) {
    switch (static_cast<CTPreprocess::Op>(opId)) {
        case CTPreprocess::Op::BILATERAL: {
            const int d = (int) params[0];
            const double sigmaColor = params[1];
            const double sigmaSpace = params[2];
            return CTPreprocess::DenoiseBilateral(mat, d, sigmaColor, sigmaSpace);
        }
        case CTPreprocess::Op::CLAHE: {
            const double c = params[0];
            const int tx = (int) params[1];
            const int ty = (int) params[2];
            if (mat.depth() == CV_32F) {
                double mn, mx;
                cv::minMaxLoc(mat, &mn, &mx);
                const double span = std::max(1e-7, mx - mn);
                cv::Mat tmp8u;
                mat.convertTo(tmp8u, CV_8U, 255.0 / span, -mn * 255.0 / span);
                tmp8u = CTPreprocess::EnhanceCLAHE(tmp8u, c, cv::Size(tx, ty));
                tmp8u.convertTo(mat, CV_32F, span / 255.0, mn);
                return mat;
            }
            return CTPreprocess::EnhanceCLAHE(mat, c, cv::Size(tx, ty));
        }
        case CTPreprocess::Op::HU_CONVERT: {
            const float slope = (float) params[0];
            const float intercept = (float) params[1];
            return CTPreprocess::ConvertRawToHU(mat, slope, intercept);
        }
        case CTPreprocess::Op::TAILOR: {
            const int minArea = (int) params[0];
            // params[1] 是 XrayProcessor 用的 sobel，params[3] 是 otsu，此处占位消费
            const int morph = (int) params[2];
            if (mat.depth() == CV_32F) {
                cv::Rect roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(mat, -600.0f, morph,
                                                                       minArea, 20);
                if (roi.width > 0 && roi.height > 0) mat = mat(roi).clone();
            }
            return mat;
        }
        case CTPreprocess::Op::INVERT_LUT:
            return CTPreprocess::EnhanceInvertLut(mat);
        case CTPreprocess::Op::FEATURE_SHARPEN: {
            const double sigma = params[0];
            const double strength = params[1];
            return CTPreprocess::SharpenUSM(mat, sigma, strength);
        }
        default:
            return mat;
    }
}

/** Kotlin 按算子出现顺序把参数平铺成一个数组，任何分发路径都必须原样消费。 */
static int opParamCount(int opId) {
    switch (static_cast<CTPreprocess::Op>(opId)) {
        case CTPreprocess::Op::BILATERAL: return 3;
        case CTPreprocess::Op::CLAHE: return 3;
        case CTPreprocess::Op::HU_CONVERT: return 2;
        case CTPreprocess::Op::TAILOR: return 4;
        case CTPreprocess::Op::FEATURE_SHARPEN: return 2;
        case CTPreprocess::Op::INVERT_LUT: return 0;
        default: return 0;
    }
}

/** 显示域算子：只能在 8-bit 调窗结果上执行。 */
static bool isDisplayOp(int opId) {
    switch (static_cast<CTPreprocess::Op>(opId)) {
        case CTPreprocess::Op::CLAHE:
        case CTPreprocess::Op::FEATURE_SHARPEN:
        case CTPreprocess::Op::INVERT_LUT:
            return true;
        default:
            return false;
    }
}

struct DisplayOp {
    int opId;
    std::vector<double> params;
};

/** 顺序执行全部算子（写入 DICOM 的路径沿用此语义：像素数据里带处理后效果）。 */
static cv::Mat dispatchOps(cv::Mat mat, const jint *pOps, jsize opsCount,
                           const jdouble *pParams, jsize paramsCount) {
    int pIdx = 0;
    for (int i = 0; i < opsCount; ++i) {
        const int opId = pOps[i];
        const int n = opParamCount(opId);
        if (pIdx + n > paramsCount) continue;   // 参数不足：跳过该算子且不消费
        mat = runOp(mat, opId, pParams + pIdx);
        pIdx += n;
    }
    return mat;
}

/**
 * 只执行数据域算子（HU 校正 / 裁剪 / 双边去噪），显示域算子按原顺序记录下来，
 * 留到调窗之后再套用到 8-bit 图上。
 *
 * CLAHE 是直方图均衡，只能在显示域做：以前它在 HU 域执行时要先把 65535 的跨度
 * 压进 256 级（每级 257 HU），均衡的对象其实是量化噪声，均衡完再乘回 HU 跨度，
 * 低频信息被整体抹掉——左右两图都变成"浮雕/高频"图就是这个原因。
 */
static cv::Mat dispatchDataOps(cv::Mat mat, const jint *pOps, jsize opsCount,
                               const jdouble *pParams, jsize paramsCount,
                               std::vector<DisplayOp> &displayOps) {
    displayOps.clear();
    int pIdx = 0;
    for (int i = 0; i < opsCount; ++i) {
        const int opId = pOps[i];
        const int n = opParamCount(opId);
        if (pIdx + n > paramsCount) continue;
        if (isDisplayOp(opId)) {
            displayOps.push_back({opId, std::vector<double>(pParams + pIdx,
                                                            pParams + pIdx + n)});
        } else {
            mat = runOp(mat, opId, pParams + pIdx);
        }
        pIdx += n;
    }
    return mat;
}

static cv::Mat applyDisplayOps(cv::Mat img8u, const std::vector<DisplayOp> &displayOps) {
    for (const DisplayOp &op: displayOps) {
        img8u = runOp(img8u, op.opId, op.params.data());
    }
    return img8u;
}

// =============================================================================
// JNI 方法实现
// =============================================================================

/**
 * 校验原始缓冲区确实覆盖 w*h 个像素。
 *
 * 必须校验：W/H 来自 UI 的手工输入框，和所选 asset 的实际像素数没有任何联动
 * （默认值 1112x1740 就超过了 FT11.raw 的 1112x1700）。LoadRawPixelBuffer 直接
 * 用 (rows, cols) 包住这块内存，长度不足时 OpenCV 会越界读 Java 堆。
 * 每个像素固定 2 字节：LoadRawPixelBuffer 只按 isUint16 决定 16U/16S，从不按
 * bitDepth 走 8-bit 分支，所以选 8-bit 时同样会读超一倍，一并拦下。
 */
static bool checkRawBufferLen(JNIEnv *env, jbyteArray rawBuf, jint w, jint h) {
    const long long need = static_cast<long long>(w) * static_cast<long long>(h) * 2;
    const jsize have = env->GetArrayLength(rawBuf);
    if (w > 0 && h > 0 && need <= static_cast<long long>(INT_MAX) && have >= need) {
        return true;
    }
    LOGE("raw buffer size mismatch: w=%d h=%d need=%lld bytes, got %d bytes", w, h, need, have);
    return false;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_processMedicalCTCompareWindows(JNIEnv *env,
                                                                                 jclass,
                                                                                 jbyteArray rawBuf,
                                                                                 jint w, jint h,
                                                                                 jint depth,
                                                                                 jboolean big,
                                                                                 jboolean isU16,
                                                                                 jintArray ops,
                                                                                 jdoubleArray params,
                                                                                 jintArray windowMethods,
                                                                                 jobjectArray outDisplays,
                                                                                 jintArray outInfo,
                                                                                 jdoubleArray outHuRange) {
    if (rawBuf == nullptr || ops == nullptr || params == nullptr || 
        windowMethods == nullptr || outDisplays == nullptr) return;
    if (!checkRawBufferLen(env, rawBuf, w, h)) return;
    jsize nMethods = env->GetArrayLength(windowMethods);
    jsize outDisplaysLen = env->GetArrayLength(outDisplays);
    if (nMethods <= 0 || outDisplaysLen <= 0) return;

    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    jint *pOps = env->GetIntArrayElements(ops, nullptr);
    jdouble *pParams = env->GetDoubleArrayElements(params, nullptr);
    jint *pMethods = env->GetIntArrayElements(windowMethods, nullptr);
    jsize opsCount = env->GetArrayLength(ops);
    jsize paramsCount = env->GetArrayLength(params);

    cv::Mat mat = CTPreprocess::LoadRawPixelBuffer(pRaw, h, w, isU16, 0, big);
    std::vector<DisplayOp> displayOps;
    mat = dispatchDataOps(mat, pOps, opsCount, pParams, paramsCount, displayOps);

    int maxCount = std::min(static_cast<int>(nMethods), static_cast<int>(outDisplaysLen));
    for (int mi = 0; mi < maxCount; ++mi) {
        const int method = pMethods[mi];
        cv::Mat out8u = (method == -1) ? normalizeTo8u(mat) : windowTo8u(mat, method);
        out8u = applyDisplayOps(out8u, displayOps);
        jbyteArray rgba;
        JniHelper::gray8uToRgbaJBytes(env, out8u, rgba);
        env->SetObjectArrayElement(outDisplays, mi, rgba);
        env->DeleteLocalRef(rgba);
    }

    if (outInfo && env->GetArrayLength(outInfo) >= 2) {
        jint info[2] = {mat.cols, mat.rows};
        env->SetIntArrayRegion(outInfo, 0, 2, info);
    }
    if (outHuRange && env->GetArrayLength(outHuRange) >= 2) {
        double mn, mx;
        cv::minMaxLoc(mat, &mn, &mx);
        jdouble range[2] = {(jdouble) mn, (jdouble) mx};
        env->SetDoubleArrayRegion(outHuRange, 0, 2, range);
    }

    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    env->ReleaseIntArrayElements(ops, pOps, JNI_ABORT);
    env->ReleaseDoubleArrayElements(params, pParams, JNI_ABORT);
    env->ReleaseIntArrayElements(windowMethods, pMethods, JNI_ABORT);
}

/**
 * 获取经过处理（如裁剪）后的 16-bit 原始像素。
 * 为 writeDcmFile 提供大端序字节（根据 dcmtk/DicomFileIO.cpp 的读取原则）。
 */
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_getProcessedRawPixels(JNIEnv *env, jclass,
                                                                        jbyteArray rawBuf, jint w,
                                                                        jint h,
                                                                        jint depth, jboolean big,
                                                                        jboolean isU16,
                                                                        jintArray ops,
                                                                        jdoubleArray params,
                                                                        jint windowMethod,
                                                                        jintArray info) {
    if (rawBuf == nullptr || ops == nullptr || params == nullptr) return nullptr;
    if (!checkRawBufferLen(env, rawBuf, w, h)) return nullptr;
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    jint *pOps = env->GetIntArrayElements(ops, nullptr);
    jdouble *pParams = env->GetDoubleArrayElements(params, nullptr);
    jsize opsCount = env->GetArrayLength(ops);
    const jsize paramsCount = env->GetArrayLength(params);

    // 1) 加载与处理
    cv::Mat mat = CTPreprocess::LoadRawPixelBuffer(pRaw, h, w, isU16, 0, big);
    mat = dispatchOps(mat, pOps, opsCount, pParams, paramsCount);

    if (mat.empty()) {
        env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
        env->ReleaseIntArrayElements(ops, pOps, JNI_ABORT);
        env->ReleaseDoubleArrayElements(params, pParams, JNI_ABORT);
        return nullptr;
    }

    // 2) 统一转为 16-bit (如果 mat 是 float/HU)
    cv::Mat mat16;
    if (mat.depth() == CV_32F) {
        // 修复：HU 域数据含有负值 (-1024)，直接 convertTo CV_16U 会截断负值。
        // 我们通过 +1024 将其平移到无符号区间 [0, 4000+]。
        // 配合 DCMTK 写入的 RescaleIntercept = -1024，查看器可以还原回原始 HU。
        cv::Mat shifted;
        cv::add(mat, cv::Scalar(1024.0), shifted);
        shifted.convertTo(mat16, CV_16U);
    } else {
        mat.convertTo(mat16, CV_16U);
    }

    // 3) 根据 DicomFileIO.cpp 的 writeDcmFileFull 原则：需要提供大端序字节流。
    int total = mat16.rows * mat16.cols;
    jsize byteCount = static_cast<jsize>(total * 2);
    jbyteArray res = env->NewByteArray(byteCount);
    std::vector<uint8_t> bigEndianBuf(byteCount);

    const ushort *ptr = mat16.ptr<ushort>();
    for (int i = 0; i < total; ++i) {
        ushort val = ptr[i];
        bigEndianBuf[2 * i] = static_cast<uint8_t>(val >> 8);
        bigEndianBuf[2 * i + 1] = static_cast<uint8_t>(val & 0xFF);
    }
    env->SetByteArrayRegion(res, 0, byteCount,
                            reinterpret_cast<const jbyte *>(bigEndianBuf.data()));

    // 4) 输出扩展信息：[outW, outH, maxVal, winCenter*10, winWidth*10]
    if (info && env->GetArrayLength(info) >= 5) {
        jint *pI = env->GetIntArrayElements(info, nullptr);
        pI[0] = mat16.cols;
        pI[1] = mat16.rows;

        double mn, mx;
        cv::minMaxLoc(mat16, &mn, &mx);
        pI[2] = (int) mx; // largestImagePixelValue

        double c = 0, winW = 0;
        if (windowMethod >= 0) {
            int nBins = (windowMethod == 6) ? 500 : 256;
            std::vector<int> hist;
            const std::vector<int> *histPtr = nullptr;

            cv::Rect roi(0, 0, mat.cols, mat.rows);
            if (mat.depth() == CV_32F) {
                roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(mat, -600.0f, 5, 1000, 10);
            }
            double hMin, hMax;
            histogramRange(mat, roi, hMin, hMax);
            std::vector<cv::Mat> slices = {mat};
            CtSeriesProcessor::aggregateSeriesHistogram(slices, roi, hMin, hMax, nBins, 1, hist);
            histPtr = &hist;

            CtSeriesProcessor::pickWindowCenterWidth(windowMethod, hMin, hMax, histPtr, nBins, c,
                                                     winW);
        } else {
            // fallback if no method
            c = (mn + mx) * 0.5 - 1024.0;
            winW = (mx - mn);
        }
        pI[3] = (int) (c * 10);
        pI[4] = (int) (winW * 10);

        env->ReleaseIntArrayElements(info, pI, 0);
    }

    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    env->ReleaseIntArrayElements(ops, pOps, JNI_ABORT);
    env->ReleaseDoubleArrayElements(params, pParams, JNI_ABORT);
    return res;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_processImage(JNIEnv *env, jclass, jobject bitmap,
                                                               jdouble contrast, jdouble brightness,
                                                               jdouble sharpen,
                                                               jboolean invert, jboolean falseColor,
                                                               jboolean relief,
                                                               jdouble min, jdouble max) {
    if (bitmap == nullptr) return nullptr;

    AndroidBitmapInfo info;
    void *pixels;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) return nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) return nullptr;

    cv::Mat src;
    if (!wrapBitmapMat(info, pixels, src)) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return nullptr;
    }

    cv::Mat processed = CTPreprocess::ImageProcessor::process(src, contrast, brightness, sharpen,
                                                              invert, falseColor, relief, min, max);

    if (processed.empty()) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return nullptr;
    }

    jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
    jmethodID createBitmapMethodID = env->GetStaticMethodID(bitmapClass, "createBitmap",
                                                            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
    jfieldID argb8888FieldID = env->GetStaticFieldID(configClass, "ARGB_8888",
                                                     "Landroid/graphics/Bitmap$Config;");
    jobject argb8888Config = env->GetStaticObjectField(configClass, argb8888FieldID);

    jobject newBitmap = env->CallStaticObjectMethod(bitmapClass, createBitmapMethodID,
                                                    (jint) info.width, (jint) info.height,
                                                    argb8888Config);

    void *newPixels;
    if (AndroidBitmap_lockPixels(env, newBitmap, &newPixels) < 0) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return nullptr;
    }

    cv::Mat dst = wrapBitmapDst(env, newBitmap, newPixels);
    if (dst.empty()) {
        AndroidBitmap_unlockPixels(env, newBitmap);
        AndroidBitmap_unlockPixels(env, bitmap);
        return nullptr;
    }
    if (processed.channels() == 1) {
        cv::cvtColor(processed, dst, cv::COLOR_GRAY2RGBA);
    } else if (processed.channels() == 3) {
        cv::cvtColor(processed, dst, cv::COLOR_BGR2RGBA);
    } else if (processed.channels() == 4) {
        processed.copyTo(dst);
    }

    AndroidBitmap_unlockPixels(env, newBitmap);
    AndroidBitmap_unlockPixels(env, bitmap);
    return newBitmap;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyRotation(JNIEnv *env, jclass, jobject bitmap,
                                                                jdouble angle) {
    if (bitmap == nullptr) return nullptr;

    AndroidBitmapInfo info;
    void *pixels;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) return nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) return nullptr;

    cv::Mat src;
    if (!wrapBitmapMat(info, pixels, src)) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return nullptr;
    }

    cv::Mat rotated = CTPreprocess::ImageProcessor::applyRotation(src, angle);
    AndroidBitmap_unlockPixels(env, bitmap);

    if (rotated.empty()) return nullptr;

    jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
    jmethodID createBitmapMethodID = env->GetStaticMethodID(bitmapClass, "createBitmap",
                                                            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
    jfieldID argb8888FieldID = env->GetStaticFieldID(configClass, "ARGB_8888",
                                                     "Landroid/graphics/Bitmap$Config;");
    jobject argb8888Config = env->GetStaticObjectField(configClass, argb8888FieldID);

    jobject newBitmap = env->CallStaticObjectMethod(bitmapClass, createBitmapMethodID,
                                                    (jint) rotated.cols, (jint) rotated.rows,
                                                    argb8888Config);

    void *newPixels;
    if (AndroidBitmap_lockPixels(env, newBitmap, &newPixels) < 0) return nullptr;

    cv::Mat dst = wrapBitmapDst(env, newBitmap, newPixels);
    if (dst.empty()) {
        AndroidBitmap_unlockPixels(env, newBitmap);
        return nullptr;
    }
    if (rotated.channels() == 1) {
        cv::cvtColor(rotated, dst, cv::COLOR_GRAY2RGBA);
    } else if (rotated.channels() == 2) {
        cv::cvtColor(rotated, dst, cv::COLOR_BGR5652RGBA);
    } else if (rotated.channels() == 3) {
        cv::cvtColor(rotated, dst, cv::COLOR_BGR2RGBA);
    } else if (rotated.channels() == 4) {
        rotated.copyTo(dst);
    }

    AndroidBitmap_unlockPixels(env, newBitmap);
    return newBitmap;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_releaseMat(JNIEnv *env, jclass, jlong matAddr) {
    cv::Mat *mat = reinterpret_cast<cv::Mat *>(matAddr);
    if (mat) {
        delete mat;
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_convertToGrayScale(JNIEnv *env, jclass,
                                                                     jobject bitmap) {
    if (bitmap == nullptr) return 0;
    AndroidBitmapInfo info;
    void *pixels;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) return 0;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) return 0;

    cv::Mat src;
    if (!wrapBitmapMat(info, pixels, src)) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return 0;
    }

    cv::Mat gray = CTPreprocess::ImageProcessor::convertToGrayScale(src);
    AndroidBitmap_unlockPixels(env, bitmap);

    cv::Mat *resMat = new cv::Mat(gray);
    return reinterpret_cast<jlong>(resMat);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_appBrightnessContrast(JNIEnv *env, jclass,
                                                                        jlong matAddr,
                                                                        jdouble contrast,
                                                                        jdouble brightness,
                                                                        jdouble min, jdouble max) {
    cv::Mat *mat = reinterpret_cast<cv::Mat *>(matAddr);
    if (!mat) return 0;
    cv::Mat result = CTPreprocess::ImageProcessor::appBrightnessContrast(*mat, contrast, brightness,
                                                                         min, max);
    cv::Mat *resMat = new cv::Mat(result);
    return reinterpret_cast<jlong>(resMat);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applySharpen(JNIEnv *env, jclass, jlong matAddr,
                                                               jdouble sharpen, jdouble min,
                                                               jdouble max) {
    cv::Mat *mat = reinterpret_cast<cv::Mat *>(matAddr);
    if (!mat) return 0;
    cv::Mat result = CTPreprocess::ImageProcessor::applySharpen(*mat, sharpen, min, max);
    cv::Mat *resMat = new cv::Mat(result);
    return reinterpret_cast<jlong>(resMat);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyInvertedColor(JNIEnv *env, jclass,
                                                                     jlong matAddr,
                                                                     jboolean invert) {
    cv::Mat *mat = reinterpret_cast<cv::Mat *>(matAddr);
    if (!mat) return 0;
    cv::Mat result = CTPreprocess::ImageProcessor::applyInvertedColor(*mat, invert);
    cv::Mat *resMat = new cv::Mat(result);
    return reinterpret_cast<jlong>(resMat);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyFalseColor(JNIEnv *env, jclass,
                                                                  jlong matAddr,
                                                                  jboolean falseColor) {
    cv::Mat *mat = reinterpret_cast<cv::Mat *>(matAddr);
    if (!mat) return 0;
    cv::Mat result = CTPreprocess::ImageProcessor::applyFalseColor(*mat, falseColor);
    cv::Mat *resMat = new cv::Mat(result);
    return reinterpret_cast<jlong>(resMat);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyRotationMat(JNIEnv *env, jclass,
                                                                   jlong matAddr, jdouble angle) {
    cv::Mat *mat = reinterpret_cast<cv::Mat *>(matAddr);
    if (!mat || mat->empty()) return 0;
    cv::Mat result = CTPreprocess::ImageProcessor::applyRotation(*mat, angle);
    cv::Mat *resMat = new cv::Mat(result);
    return reinterpret_cast<jlong>(resMat);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyEmbossingEffect(JNIEnv *env, jclass,
                                                                       jlong matAddr,
                                                                       jboolean embossed) {
    cv::Mat *mat = reinterpret_cast<cv::Mat *>(matAddr);
    if (!mat) return 0;
    cv::Mat result = CTPreprocess::ImageProcessor::applyEmbossingEffect(*mat, embossed);
    cv::Mat *resMat = new cv::Mat(result);
    return reinterpret_cast<jlong>(resMat);
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_example_rawpixeldeal_jni_RawPixelDealJni_convertMatToBitmap(JNIEnv *env, jclass,
                                                                     jlong matAddr, jint width,
                                                                     jint height) {
    cv::Mat *mat = reinterpret_cast<cv::Mat *>(matAddr);
    if (!mat || mat->empty()) return nullptr;

    jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
    jmethodID createBitmapMethodID = env->GetStaticMethodID(bitmapClass, "createBitmap",
                                                            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
    jfieldID argb8888FieldID = env->GetStaticFieldID(configClass, "ARGB_8888",
                                                     "Landroid/graphics/Bitmap$Config;");
    jobject argb8888Config = env->GetStaticObjectField(configClass, argb8888FieldID);

    jobject newBitmap = env->CallStaticObjectMethod(bitmapClass, createBitmapMethodID,
                                                    (jint) width, (jint) height,
                                                    argb8888Config);

    void *newPixels;
    if (AndroidBitmap_lockPixels(env, newBitmap, &newPixels) < 0) return nullptr;

    cv::Mat dst = wrapBitmapDst(env, newBitmap, newPixels);
    if (dst.empty() || dst.size() != mat->size()) {
        AndroidBitmap_unlockPixels(env, newBitmap);
        return nullptr;
    }
    if (mat->channels() == 1) {
        cv::cvtColor(*mat, dst, cv::COLOR_GRAY2RGBA);
    } else if (mat->channels() == 3) {
        cv::cvtColor(*mat, dst, cv::COLOR_BGR2RGBA);
    } else if (mat->channels() == 4) {
        mat->copyTo(dst);
    }

    AndroidBitmap_unlockPixels(env, newBitmap);
    return newBitmap;
}


// =============================================================================
// JNI 注册
// =============================================================================
static const char *const kClassName = "com/example/rawpixeldeal/jni/RawPixelDealJni";
static const JNINativeMethod kMethods[] = {
        {"processMedicalCTCompareWindows",
                                  "([BIIIZZ[I[D[I[[B[I[D)V",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_processMedicalCTCompareWindows},
        {"getProcessedRawPixels",
                                  "([BIIIZZ[I[DI[I)[B",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_getProcessedRawPixels},
        {"processImage",
                                  "(Landroid/graphics/Bitmap;DDDZZZDD)Landroid/graphics/Bitmap;",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_processImage},
        {"applyRotation",
                                  "(Landroid/graphics/Bitmap;D)Landroid/graphics/Bitmap;",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyRotation},
        {"convertToGrayScale",    "(Landroid/graphics/Bitmap;)J",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_convertToGrayScale},
        {"appBrightnessContrast", "(JDDDD)J",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_appBrightnessContrast},
        {"applySharpen",          "(JDDD)J",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applySharpen},
        {"applyInvertedColor",    "(JZ)J",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyInvertedColor},
        {"applyFalseColor",       "(JZ)J",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyFalseColor},
        {"applyRotationMat",      "(JD)J",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyRotationMat},
        {"applyEmbossingEffect",  "(JZ)J",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_applyEmbossingEffect},
        {"convertMatToBitmap",    "(JII)Landroid/graphics/Bitmap;",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_convertMatToBitmap},
        {"releaseMat",            "(J)V",
                (void *) Java_com_example_rawpixeldeal_jni_RawPixelDealJni_releaseMat},
};

extern "C" jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass clz = env->FindClass(kClassName);
    if (!clz) return JNI_ERR;
    if (env->RegisterNatives(clz, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}
