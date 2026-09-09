#include <jni.h>
#include <vector>
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

static cv::Mat normalizeTo8u(const cv::Mat &mat) {
    cv::Mat out8u;
    double mn = 0.0, mx = 0.0;
    cv::minMaxLoc(mat, &mn, &mx);
    const double span = std::max(1e-7, mx - mn);
    mat.convertTo(out8u, CV_8U, 255.0 / span, -mn * 255.0 / span);
    return out8u;
}

/**
 * 调窗映射逻辑：将高动态范围的原始数据(HU)线性映射到 8-bit 可视化空间。
 */
static cv::Mat windowTo8u(const cv::Mat &mat, int windowMethod) {
    double minV = 0.0, maxV = 0.0;
    cv::minMaxLoc(mat, &minV, &maxV);

    int nBins = (windowMethod == 6) ? 500 : 256;
    std::vector<int> hist;
    const std::vector<int> *histPtr = nullptr;

    // 为了让直方图统计更准确，先进行自动人体 ROI 裁剪
    cv::Rect roi(0, 0, mat.cols, mat.rows);
    if (mat.depth() == CV_32F) {
        roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(mat, -600.0f, 5, 1000, 10);
    }

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
 * 算子分发中心：根据 Kotlin 传来的操作 ID 列表，依次执行对应的 OpenCV 图像处理。
 */
static cv::Mat dispatchOps(cv::Mat mat, const jint *pOps, jsize opsCount,
                           const jdouble *pParams, jsize paramsCount) {
    int pIdx = 0;
    for (int i = 0; i < opsCount; ++i) {
        const int opId = pOps[i];
        auto need = [&](int n) -> bool {
            return (pIdx + n <= paramsCount);
        };

        switch (static_cast<CTPreprocess::Op>(opId)) {
            case CTPreprocess::Op::BILATERAL: {
                if (!need(3)) break;
                const int d = (int) pParams[pIdx++];
                const double sigmaColor = pParams[pIdx++];
                const double sigmaSpace = pParams[pIdx++];
                mat = CTPreprocess::DenoiseBilateral(mat, d, sigmaColor, sigmaSpace);
                break;
            }
            case CTPreprocess::Op::CLAHE: {
                if (!need(3)) break;
                const double c = pParams[pIdx++];
                const int tx = (int) pParams[pIdx++];
                const int ty = (int) pParams[pIdx++];
                if (mat.depth() == CV_32F) {
                    double mn, mx;
                    cv::minMaxLoc(mat, &mn, &mx);
                    const double span = std::max(1e-7, mx - mn);
                    cv::Mat tmp8u;
                    mat.convertTo(tmp8u, CV_8U, 255.0 / span, -mn * 255.0 / span);
                    tmp8u = CTPreprocess::EnhanceCLAHE(tmp8u, c, cv::Size(tx, ty));
                    tmp8u.convertTo(mat, CV_32F, span / 255.0, mn);
                } else {
                    mat = CTPreprocess::EnhanceCLAHE(mat, c, cv::Size(tx, ty));
                }
                break;
            }
            case CTPreprocess::Op::HU_CONVERT: {
                if (!need(2)) break;
                const float slope = (float) pParams[pIdx++];
                const float intercept = (float) pParams[pIdx++];
                mat = CTPreprocess::ConvertRawToHU(mat, slope, intercept);
                break;
            }
            case CTPreprocess::Op::TAILOR: {
                if (!need(4)) break;
                const int minArea = (int) pParams[pIdx++];
                // skip sobel (pParams[pIdx++]) as it's for XrayProcessor
                pIdx++;
                const int morph = (int) pParams[pIdx++];
                // skip otsu (pParams[pIdx++])
                pIdx++;

                if (mat.depth() == CV_32F) {
                    cv::Rect roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(mat, -600.0f, morph,
                                                                           minArea, 20);
                    if (roi.width > 0 && roi.height > 0) mat = mat(roi).clone();
                }
                break;
            }
            case CTPreprocess::Op::INVERT_LUT:
                mat = CTPreprocess::EnhanceInvertLut(mat);
                break;
            case CTPreprocess::Op::FEATURE_SHARPEN: {
                if (!need(2)) break;
                const double sigma = pParams[pIdx++];
                const double strength = pParams[pIdx++];
                mat = CTPreprocess::SharpenUSM(mat, sigma, strength);
                break;
            }
            default:
                break;
        }
    }
    return mat;
}

// =============================================================================
// JNI 方法实现
// =============================================================================

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
    mat = dispatchOps(mat, pOps, opsCount, pParams, paramsCount);

    int maxCount = std::min(static_cast<int>(nMethods), static_cast<int>(outDisplaysLen));
    for (int mi = 0; mi < maxCount; ++mi) {
        const int method = pMethods[mi];
        cv::Mat out8u = (method == -1) ? normalizeTo8u(mat) : windowTo8u(mat, method);
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
            double hMin, hMax;
            cv::minMaxLoc(mat, &hMin, &hMax);
            int nBins = (windowMethod == 6) ? 500 : 256;
            std::vector<int> hist;
            const std::vector<int> *histPtr = nullptr;

            cv::Rect roi(0, 0, mat.cols, mat.rows);
            if (mat.depth() == CV_32F) {
                roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(mat, -600.0f, 5, 1000, 10);
            }
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
    if (info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {
        src = cv::Mat(info.height, info.width, CV_8UC4, pixels);
    } else if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
        src = cv::Mat(info.height, info.width, CV_8UC2, pixels);
    } else {
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

    cv::Mat dst(info.height, info.width, CV_8UC4, newPixels);
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
    if (info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {
        src = cv::Mat(info.height, info.width, CV_8UC4, pixels);
    } else if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
        src = cv::Mat(info.height, info.width, CV_8UC2, pixels);
    } else {
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

    cv::Mat dst(rotated.rows, rotated.cols, CV_8UC4, newPixels);
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
    if (info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {
        src = cv::Mat(info.height, info.width, CV_8UC4, pixels);
    } else if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
        src = cv::Mat(info.height, info.width, CV_8UC2, pixels);
    } else {
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

    cv::Mat dst(height, width, CV_8UC4, newPixels);
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
