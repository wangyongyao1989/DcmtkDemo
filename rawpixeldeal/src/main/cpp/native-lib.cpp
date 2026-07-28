#include <jni.h>
#include <string>
#include <vector>
#include <cstring>
#include <android/log.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "include/MedicalCTPreprocess.h"
#include "include/JniHelper.h"
#include "include/CtSeriesProcessor.h"
#include "include/XrayProcessor.h"

#define TAG "RawPixelDealJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// =============================================================================
// 1) 辅助接口
// =============================================================================
static jstring native_getOpenCVVersion(JNIEnv *env, jclass) {
    return env->NewStringUTF(cv::getVersionString().c_str());
}

static jstring native_stringFromJNI(JNIEnv *env, jclass) {
    return env->NewStringUTF("Hello from rawpixeldeal native (Refactored)");
}

// =============================================================================
// 2) 灰度验证链路
// =============================================================================
static jintArray
native_processRawGrayPixels(JNIEnv *env, jclass, jint width, jint height, jbyteArray src_gray) {
    if (width <= 0 || height <= 0 || src_gray == nullptr) return nullptr;
    jsize len = env->GetArrayLength(src_gray);
    if (len < width * height) return nullptr;

    std::vector<uint8_t> buf;
    JniHelper::copyJByteArray(env, src_gray, buf);

    cv::Mat src(height, width, CV_8UC1, buf.data());
    cv::Mat dst;
    cv::GaussianBlur(src, dst, cv::Size(3, 3), 0.0);

    env->SetByteArrayRegion(src_gray, 0, width * height, reinterpret_cast<jbyte *>(dst.data));

    jint ret[5] = {dst.at<uchar>(0, 0), (width > 1 ? dst.at<uchar>(0, 1) : 0),
                   (jint) cv::sum(dst)[0]};
    jintArray out = env->NewIntArray(5);
    env->SetIntArrayRegion(out, 0, 5, ret);
    return out;
}

// =============================================================================
// 3) Assets Raw 转 RGBA (单图)
// =============================================================================
static jbyteArray
native_processRawToRgba(JNIEnv *env, jclass, jint width, jint height, jint bitDepth,
                        jbyteArray srcBytes, jint cropL, jint cropT, jint cropR, jint cropB,
                        jint enableClahe, jdouble clipLimit, jint tileSize, jintArray head) {
    std::vector<uint8_t> raw;
    if (!JniHelper::copyJByteArray(env, srcBytes, raw)) return nullptr;

    cv::Mat src = JniHelper::wrapRawMat(raw.data(), width, height, bitDepth, 0);
    if (src.empty()) return nullptr;

    cv::Mat gray8;
    double minV, maxV;
    cv::minMaxLoc(src, &minV, &maxV);
    src.convertTo(gray8, CV_8UC1, 255.0 / std::max(1.0, maxV - minV),
                  -minV * 255.0 / std::max(1.0, maxV - minV));

    if (enableClahe) {
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clipLimit > 0 ? clipLimit : 2.0,
                                                   cv::Size(tileSize, tileSize));
        clahe->apply(gray8, gray8);
    }

    cv::Rect roi(cropL, cropT, std::max(1, width - cropL - cropR),
                 std::max(1, height - cropT - cropB));
    cv::Mat cropped = gray8(roi).clone();

    jbyteArray out;
    if (!JniHelper::gray8uToRgbaJBytes(env, cropped, out)) return nullptr;

    if (head && env->GetArrayLength(head) >= 4) {
        jint hOut[4] = {(jint) minV, (jint) maxV, cropped.cols, cropped.rows};
        env->SetIntArrayRegion(head, 0, 4, hOut);
    }
    return out;
}

// =============================================================================
// 4) CT 序列处理管线
// =============================================================================
static jboolean
native_processCtSeries(JNIEnv *env, jclass, jobjectArray rawBuffers,
                       jint width, jint height,
                       jint bits, jint pixelSigned, jdouble slope, jdouble intercept,
                       jint photometric,
                       jfloat bodyThr, jint cropMargin, jint morphSize, jint minArea, jint nBins,
                       jdouble n0, jdouble n1, jint stride, jint enableBilateral, jint bilateralD,
                       jdouble sigmaC, jdouble sigmaS, jfloat clipLow, jfloat clipHigh,
                       jint autoSign, jfloat pLow, jfloat pHigh, jint enableFallback,
                       jdouble fbC, jdouble fbW, jint enableDClahe, jdouble dClaheClip,
                       jint dClaheTile, jint cropFirst,
                       jobjectArray outDisplays, jdoubleArray outWS, jintArray outCrop,
                       jintArray outHist, jintArray outFlags) {
    jsize nSlices = env->GetArrayLength(rawBuffers);
    std::vector<std::vector<uint8_t>> rawData(nSlices);
    std::vector<cv::Mat> huMats;

    for (jsize s = 0; s < nSlices; ++s) {
        jbyteArray jb = (jbyteArray) env->GetObjectArrayElement(rawBuffers, s);
        JniHelper::copyJByteArray(env, jb, rawData[s]);
        env->DeleteLocalRef(jb);
        cv::Mat sv = JniHelper::wrapRawMat(rawData[s].data(), width, height, bits, pixelSigned);
        huMats.push_back(CtSeriesProcessor::toHu(sv, slope, intercept));
    }

    cv::Rect cropRect;
    int pickIdx = nSlices / 2;
    cropRect = CtSeriesProcessor::tryAutoCropBodyRoiEx(huMats[pickIdx], bodyThr, morphSize, minArea,
                                                       cropMargin);

    for (auto &hu: huMats) {
        cv::Mat roi = hu(cropRect);
        cv::Mat opt = CtSeriesProcessor::optimizeHu(roi, enableBilateral, bilateralD, sigmaC,
                                                    sigmaS, clipLow, clipHigh);
        opt.copyTo(roi);
    }

    float gMin, gMax;
    CtSeriesProcessor::computePercentileHu(huMats, cropRect, stride, pLow, pHigh, gMin, gMax);

    std::vector<int> hist;
    CtSeriesProcessor::aggregateSeriesHistogram(huMats, cropRect, gMin, gMax, nBins, stride, hist);

    CtSeriesProcessor::AdaptiveWindowResult aw;
    aw.hBins = (gMax - gMin) / nBins;
    CtSeriesProcessor::computeAdaptiveWindow(hist, nBins, n0, n1, aw);

    double finalC = aw.c, finalW = aw.w;
    if (enableFallback) {
        double fC, fW;
        bool used;
        CtSeriesProcessor::pickDefaultWindow(gMin, gMax,
                                             CtSeriesProcessor::computeHistogramStats(hist), fbC,
                                             fbW, fC, fW, used);
        if (used) {
            finalC = fC;
            finalW = fW;
        }
    }

    for (jsize s = 0; s < nSlices; ++s) {
        cv::Mat disp = CtSeriesProcessor::applyWindow8u(huMats[s](cropRect), finalC, finalW,
                                                        photometric);
        disp = CtSeriesProcessor::applyDisplayClahe(disp, enableDClahe, dClaheClip, dClaheTile);
        jbyteArray rgba;
        JniHelper::gray8uToRgbaJBytes(env, disp, rgba);
        env->SetObjectArrayElement(outDisplays, s, rgba);
        env->DeleteLocalRef(rgba);
    }

    if (outWS) {
        double ws[11] = {finalC, finalW, (double) gMin, (double) gMax, aw.hBins, aw.t0, aw.t1,
                         (double) aw.b};
        env->SetDoubleArrayRegion(outWS, 0, 11, ws);
    }
    if (outCrop) {
        jint co[10] = {cropRect.x, cropRect.y, cropRect.width, cropRect.height, cropRect.width,
                       cropRect.height, nSlices, (jint) (n0 * 1e6), (jint) (n1 * 1e6), 1};
        env->SetIntArrayRegion(outCrop, 0, 10, co);
    }
    if (outHist)
        env->SetIntArrayRegion(outHist, 0, std::min((int) env->GetArrayLength(outHist),
                                                    (int) hist.size()),
                               reinterpret_cast<const jint *>(hist.data()));
    return JNI_TRUE;
}

// =============================================================================
// 5) X-ray 裁剪
// =============================================================================
static jbyteArray
native_tailorImage(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h, jint bits, jint sign,
                   jboolean bigEndian, jint minArea, jboolean sobel, jint morph, jdouble otsuLow,
                   jbyteArray outCropped, jintArray outInfo) {
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    if (pRaw == nullptr) return nullptr;

    // 改进：使用 LoadRawPixelBuffer 处理字节序，确保裁剪算子工作在正确的值域
    cv::Mat sv = CTPreprocess::LoadRawPixelBuffer(pRaw, h, w, sign == 0, 0, bigEndian);

    int outX, outY;
    double angle;
    bool ok;
    cv::Mat cropped = XrayProcessor::tailor(sv, minArea,
                                            sobel, morph,
                                            otsuLow, outX,
                                            outY, angle,
                                            ok);

    if (!ok) {
        env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
        return XrayProcessor::buildFullImageResult(env, sv, outInfo);
    }

    // 如果是处理大端数据，则在输出前转回大端，以保持与后续 Pipeline 逻辑的一致性
    if (bigEndian) {
        ushort *p = cropped.ptr<ushort>();
        int total = cropped.rows * cropped.cols;
        for (int i = 0; i < total; i++) {
            ushort val = p[i];
            p[i] = (val >> 8) | (val << 8);
        }
    }

    jbyteArray outBytes = env->NewByteArray(
            static_cast<jsize>(cropped.total() * cropped.elemSize()));
    env->SetByteArrayRegion(outBytes, 0, static_cast<jsize>(cropped.total() * cropped.elemSize()),
                            reinterpret_cast<const jbyte *>(cropped.data));

    if (outInfo) {
        jint info[6] = {outX, outY, cropped.cols, cropped.rows, (jint) (angle * 1000), 1};
        env->SetIntArrayRegion(outInfo, 0, 6, info);
    }

    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    return outBytes;
}

// =============================================================================
// 6) Invert LUTs
// =============================================================================
static jbyteArray
native_invertLut(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h, jint bits, jint sign,
                 jboolean bigEndian) {
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    if (pRaw == nullptr) return nullptr;

    // 改进：使用 LoadRawPixelBuffer 处理字节序
    cv::Mat mat = CTPreprocess::LoadRawPixelBuffer(pRaw, h, w, sign == 0, 0, bigEndian);
    cv::Mat inverted = CTPreprocess::EnhanceInvertLut(mat);

    // 如果是处理大端数据，则在输出前转回大端，以保持与后续 Pipeline 兼容
    if (bigEndian) {
        ushort *p = inverted.ptr<ushort>();
        int total = inverted.rows * inverted.cols;
        for (int i = 0; i < total; i++) {
            ushort val = p[i];
            p[i] = (val >> 8) | (val << 8);
        }
    }

    jsize byteCount = static_cast<jsize>(inverted.total() * inverted.elemSize());
    jbyteArray outBytes = env->NewByteArray(byteCount);
    env->SetByteArrayRegion(outBytes, 0, byteCount, reinterpret_cast<const jbyte *>(inverted.data));

    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    return outBytes;
}

// =============================================================================
// 7) 通用医学预处理
// =============================================================================

/**
 * 把 16-bit / float Mat 归一化到 0..255 的 8-bit Mat（无调窗时的默认显示）。
 * 内部用 min-max 线性缩放 + 1e-7 防 0 除。
 */
static cv::Mat normalizeTo8u(const cv::Mat &mat) {
    cv::Mat out8u;
    double mn = 0.0, mx = 0.0;
    cv::minMaxLoc(mat, &mn, &mx);
    const double span = std::max(1e-7, mx - mn);
    mat.convertTo(out8u, CV_8U, 255.0 / span, -mn * 255.0 / span);
    return out8u;
}

/**
 * 调窗（含 DEFAULT/MIN_MAX/72/BIMODAL/ADAPTIVE/HIST_TYPE）的统一入口。
 * 内部委托给 CtSeriesProcessor::pickWindowCenterWidth，避免与
 * CTTailorInvertWindowPipeline 里另一份重复实现漂移。
 */
static cv::Mat windowTo8u(const cv::Mat &mat, int windowMethod) {
    double minV = 0.0, maxV = 0.0;
    cv::minMaxLoc(mat, &minV, &maxV);

    int nBins = (windowMethod == 6) ? 500 : 256;
    std::vector<int> hist;
    const std::vector<int> *histPtr = nullptr;
    if (windowMethod == 1 || windowMethod == 2 || windowMethod == 3 || windowMethod == 6) {
        // 改进建议：调窗统计应排除空气背景干扰。
        // 如果当前 Mat 是 HU 域 (float)，先自动识别主体 ROI。
        cv::Rect roi(0, 0, mat.cols, mat.rows);
        if (mat.depth() == CV_32F) {
            roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(mat, -600.0f, 5, 1000, 10);
            LOGD("windowTo8u: Auto ROI for stats: [%d, %d, %dx%d]", roi.x, roi.y, roi.width, roi.height);
        }

        std::vector<cv::Mat> slices = {const_cast<cv::Mat &>(mat)};
        CtSeriesProcessor::aggregateSeriesHistogram(
                slices, roi,
                minV, maxV, nBins, 1, hist);
        histPtr = &hist;
    }

    double c = 127.5, w = 255.0;
    CtSeriesProcessor::pickWindowCenterWidth(windowMethod, minV, maxV, histPtr, nBins, c, w);
    if (w < 1.0) w = 1.0;
    return CtSeriesProcessor::applyWindow8u(mat, c, w, /*photometric=*/0);
}

/**
 * P3-fix (问题5): 公共算子派发函数，消除 native_processMedicalCT 与
 * native_processMedicalCTCompareWindows 之间约 120 行重复 switch-case。
 *
 * 同时包含以下改进：
 * - 问题7: GLOBAL_EQUALIZE / CLAHE 在 HU 域 (CV_32FC1) 时，
 *   先记录 HU 范围 → 临时映射 8-bit → 执行增强 → 反映射回 HU 域，
 *   避免静默精度降级。
 * - 问题8: TAILOR 在 HU 域时委托 CtSeriesProcessor::tryAutoCropBodyRoiEx，
 *   而非 XrayProcessor::tailor（后者为 X 光设计，不适用于 CT HU 数据）。
 */
static cv::Mat dispatchOps(cv::Mat mat, const jint *pOps, jsize opsCount,
                           const jdouble *pParams, jsize paramsCount) {
    int pIdx = 0;
    for (int i = 0; i < opsCount; ++i) {
        const int opId = pOps[i];
        auto need = [&](int n) -> bool {
            if (pIdx + n > paramsCount) {
                LOGE("dispatchOps: param underflow at op=%d idx=%d need=%d total=%d",
                     opId, pIdx, n, paramsCount);
                return false;
            }
            return true;
        };

        switch (static_cast<CTPreprocess::Op>(opId)) {
            case CTPreprocess::Op::GAUSSIAN: {
                if (!need(2)) goto done;
                const int kernel = (int) pParams[pIdx++];
                const double sigma = pParams[pIdx++];
                mat = CTPreprocess::DenoiseGaussian(mat, kernel, sigma);
                break;
            }
            case CTPreprocess::Op::MEDIAN: {
                if (!need(1)) goto done;
                const int kernel = (int) pParams[pIdx++];
                mat = CTPreprocess::DenoiseMedian(mat, kernel);
                break;
            }
            case CTPreprocess::Op::BILATERAL: {
                if (!need(3)) goto done;
                const int d = (int) pParams[pIdx++];
                const double sigmaColor = pParams[pIdx++];
                const double sigmaSpace = pParams[pIdx++];
                mat = CTPreprocess::DenoiseBilateral(mat, d, sigmaColor, sigmaSpace);
                break;
            }
            case CTPreprocess::Op::FFT: {
                if (!need(1)) goto done;
                const float radius = (float) pParams[pIdx++];
                mat = CTPreprocess::DenoiseFrequencyFFT(mat, radius);
                break;
            }
            case CTPreprocess::Op::RESAMPLE_SIZE: {
                if (!need(3)) goto done;
                const int tw = (int) pParams[pIdx++];
                const int th = (int) pParams[pIdx++];
                const bool isUp = pParams[pIdx++] > 0.5;
                mat = CTPreprocess::ResampleImage(mat, tw, th, isUp);
                break;
            }
            case CTPreprocess::Op::RESAMPLE_SCALE: {
                if (!need(2)) goto done;
                const float sx = (float) pParams[pIdx++];
                const float sy = (float) pParams[pIdx++];
                mat = CTPreprocess::ResampleByScale(mat, sx, sy);
                break;
            }
            case CTPreprocess::Op::GLOBAL_EQUALIZE: {
                // P1-fix (问题7): HU 域数据做均衡化时保留精度
                if (mat.depth() == CV_32F) {
                    double mn, mx;
                    cv::minMaxLoc(mat, &mn, &mx);
                    const double span = std::max(1e-7, mx - mn);
                    cv::Mat tmp8u;
                    mat.convertTo(tmp8u, CV_8U, 255.0 / span, -mn * 255.0 / span);
                    tmp8u = CTPreprocess::EnhanceGlobalEqualize(tmp8u);
                    tmp8u.convertTo(mat, CV_32F, span / 255.0, mn);
                } else {
                    if (mat.depth() != CV_8U) mat = normalizeTo8u(mat);
                    mat = CTPreprocess::EnhanceGlobalEqualize(mat);
                }
                break;
            }
            case CTPreprocess::Op::CLAHE: {
                if (!need(3)) goto done;
                const double c = pParams[pIdx++];
                const int tx = (int) pParams[pIdx++];
                const int ty = (int) pParams[pIdx++];
                // P1-fix (问题7): HU 域数据做 CLAHE 时保留精度
                if (mat.depth() == CV_32F) {
                    double mn, mx;
                    cv::minMaxLoc(mat, &mn, &mx);
                    const double span = std::max(1e-7, mx - mn);
                    cv::Mat tmp8u;
                    mat.convertTo(tmp8u, CV_8U, 255.0 / span, -mn * 255.0 / span);
                    tmp8u = CTPreprocess::EnhanceCLAHE(tmp8u, c, cv::Size(tx, ty));
                    tmp8u.convertTo(mat, CV_32F, span / 255.0, mn);
                } else {
                    if (mat.depth() != CV_8U && mat.depth() != CV_16U) mat = normalizeTo8u(mat);
                    mat = CTPreprocess::EnhanceCLAHE(mat, c, cv::Size(tx, ty));
                }
                break;
            }
            case CTPreprocess::Op::CONTRAST_STRETCH:
                mat = CTPreprocess::EnhanceContrastStretch(mat);
                break;
            case CTPreprocess::Op::HU_CONVERT: {
                if (!need(2)) goto done;
                const float slope = (float) pParams[pIdx++];
                const float intercept = (float) pParams[pIdx++];
                mat = CTPreprocess::ConvertRawToHU(mat, slope, intercept);
                break;
            }
            case CTPreprocess::Op::TAILOR: {
                if (!need(4)) goto done;
                const int minArea = (int) pParams[pIdx++];
                const bool sobel = pParams[pIdx++] > 0.5;
                const int morph = (int) pParams[pIdx++];
                const double otsu = pParams[pIdx++];
                // P1-fix (问题8): HU 域数据使用 CT 专用裁剪而非 X 光裁剪
                if (mat.depth() == CV_32F) {
                    cv::Rect roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(
                            mat, -600.0f, morph, minArea, 20);
                    if (roi.width > 0 && roi.height > 0) {
                        mat = mat(roi).clone();
                    }
                } else {
                    int outX, outY;
                    double angle;
                    bool ok;
                    cv::Mat cropped = XrayProcessor::tailor(mat, minArea, sobel, morph,
                                                            otsu, outX, outY, angle, ok);
                    if (ok && !cropped.empty()) mat = cropped;
                }
                break;
            }
            case CTPreprocess::Op::INVERT_LUT:
                mat = CTPreprocess::EnhanceInvertLut(mat);
                break;
            case CTPreprocess::Op::FEATURE_SHARPEN: {
                if (!need(2)) goto done;
                const double sigma = pParams[pIdx++];
                const double strength = pParams[pIdx++];
                mat = CTPreprocess::SharpenUSM(mat, sigma, strength);
                break;
            }
            case CTPreprocess::Op::LOG_TRANSFORM:
                mat = CTPreprocess::LogTransform(mat);
                break;
            default:
                LOGW("dispatchOps: unknown op id=%d, skipped", opId);
                break;
        }
    }
    done:
    return mat;
}

/**
 * 通用医学预处理 (P1-6: 新增 jdoubleArray outHuRange，保留 srcMin/srcMax 浮点精度)
 *
 * @param outInfo    out，长度 4：[outW, outH, (int)srcMin, (int)srcMax] —— 向后兼容
 * @param outHuRange out，长度 2：[srcMin, srcMax]（double）—— 新增，保留 HU 浮点精度
 *                  可为 null（Kotlin 端若不关心可传 null）。
 */
static jbyteArray
native_processMedicalCT(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h, jint depth,
                        jboolean big, jboolean isU16,
                        jintArray ops, jdoubleArray params, jint windowMethod, jintArray info,
                        jdoubleArray outHuRange) {
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    jint *pOps = env->GetIntArrayElements(ops, nullptr);
    jdouble *pParams = env->GetDoubleArrayElements(params, nullptr);
    jsize opsCount = env->GetArrayLength(ops);
    const jsize paramsCount = env->GetArrayLength(params);

    cv::Mat mat = CTPreprocess::LoadRawPixelBuffer(pRaw, h, w, isU16, 0, big);
    // P3-fix (问题5): 委托公共 dispatchOps，消除重复 switch-case
    mat = dispatchOps(mat, pOps, opsCount, pParams, paramsCount);

    cv::Mat out8u;
    if (mat.depth() != CV_8U) {
        if (windowMethod != -1) {
            // 调窗（委托给 pickWindowCenterWidth + applyWindow8u）
            out8u = windowTo8u(mat, windowMethod);
        } else {
            // 默认 Min-Max 可视化
            out8u = normalizeTo8u(mat);
        }
    } else out8u = mat;

    jbyteArray res;
    JniHelper::gray8uToRgbaJBytes(env, out8u, res);

    if (info && env->GetArrayLength(info) >= 4) {
        jint *pI = env->GetIntArrayElements(info, nullptr);
        pI[0] = out8u.cols;
        pI[1] = out8u.rows;
        double mn, mx;
        cv::minMaxLoc(mat, &mn, &mx);
        pI[2] = (int) mn;
        pI[3] = (int) mx;
        env->ReleaseIntArrayElements(info, pI, 0);
    }
    // P1-6: 浮点 outHuRange（保留 HU 小数）
    if (outHuRange && env->GetArrayLength(outHuRange) >= 2) {
        double mn, mx;
        cv::minMaxLoc(mat, &mn, &mx);
        jdouble range[2] = {(jdouble) mn, (jdouble) mx};
        env->SetDoubleArrayRegion(outHuRange, 0, 2, range);
    }
    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    env->ReleaseIntArrayElements(ops, pOps, JNI_ABORT);
    env->ReleaseDoubleArrayElements(params, pParams, JNI_ABORT);
    return res;
}

static jbyteArray
native_processCTFullPipeline(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h, jint tw,
                             jint th, jfloat slope, jfloat intercept, jboolean bigEndian,
                             jboolean isU16, jintArray info, jdoubleArray outHuRange) {
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    cv::Mat resMat = CTPreprocess::CTFullPipeline(pRaw, h, w, tw, th, slope, intercept,
                                                  bigEndian, isU16);
    jbyteArray res;
    JniHelper::gray8uToRgbaJBytes(env, resMat, res);
    if (info && env->GetArrayLength(info) >= 4) {
        jint *pI = env->GetIntArrayElements(info, nullptr);
        pI[0] = resMat.cols;
        pI[1] = resMat.rows;
        double mn, mx;
        cv::minMaxLoc(resMat, &mn, &mx);
        pI[2] = (int) mn;
        pI[3] = (int) mx;
        env->ReleaseIntArrayElements(info, pI, 0);
    }
    // P1-6: 浮点 outHuRange
    if (outHuRange && env->GetArrayLength(outHuRange) >= 2) {
        double mn, mx;
        cv::minMaxLoc(resMat, &mn, &mx);
        jdouble range[2] = {(jdouble) mn, (jdouble) mx};
        env->SetDoubleArrayRegion(outHuRange, 0, 2, range);
    }
    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    return res;
}

static jbyteArray
native_processCTTailorInvertWindowPipeline(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h,
                                           jfloat slope, jfloat intercept, jboolean bigEndian,
                                           jint windowMethod, jintArray info) {
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    int outMin, outMax;
    cv::Mat resMat = CTPreprocess::CTTailorInvertWindowPipeline(pRaw, h, w, slope, intercept,
                                                                bigEndian, windowMethod,
                                                                outMin, outMax);
    jbyteArray res;
    JniHelper::gray8uToRgbaJBytes(env, resMat, res);

    if (info && env->GetArrayLength(info) >= 4) {
        jint *pI = env->GetIntArrayElements(info, nullptr);
        pI[0] = resMat.cols;
        pI[1] = resMat.rows;
        pI[2] = outMin;
        pI[3] = outMax;
        env->ReleaseIntArrayElements(info, pI, 0);
    }

    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    return res;
}

// =============================================================================
// 8) 多调窗对比：跑一遍重负载（裁剪/HU/去噪/重采样）+ N 次 8-bit 映射
// =============================================================================
/**
 * 设计目的：
 *  - 调窗对比场景下，Kotlin 端以前要调用 N 次 processMedicalCT，每次都重做
 *    裁剪/HU/双边滤波等重操作。
 *  - 这个 JNI 入口把重操作跑一次得到"预处理后 16-bit Mat"，然后只对 8-bit 映射
 *    做 N 次（min-max + 每个 windowMethod），显著降低 CPU 占用。
 *
 * 签名：
 *   raw bytes (16-bit), w, h, depth, bigEndian, isU16,
 *   ops int[], params double[],
 *   windowMethods int[]  // 调窗方法列表；至少 1 个；-1 表示 min-max
 *   outDisplays Array<ByteArray?>  // 出参，每项一张 RGBA8888
 *   outInfo int[]                  // 出参，前 2 元素 [outW, outH]；所有对比图尺寸相同
 *   outHuRange double[]            // P1-6: 出参，长度 2，[srcMin, srcMax] (HU 浮点)
 */
static void
native_processMedicalCTCompareWindows(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h,
                                      jint depth, jboolean big, jboolean isU16,
                                      jintArray ops, jdoubleArray params,
                                      jintArray windowMethods,
                                      jobjectArray outDisplays, jintArray outInfo,
                                      jdoubleArray outHuRange) {
    if (outDisplays == nullptr || windowMethods == nullptr) return;
    jsize nMethods = env->GetArrayLength(windowMethods);
    if (nMethods <= 0) return;
    if (env->GetArrayLength(outDisplays) < nMethods) return;

    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    jint *pOps = env->GetIntArrayElements(ops, nullptr);
    jdouble *pParams = env->GetDoubleArrayElements(params, nullptr);
    jint *pMethods = env->GetIntArrayElements(windowMethods, nullptr);
    jsize opsCount = env->GetArrayLength(ops);
    const jsize paramsCount = env->GetArrayLength(params);

    // 1) 跑重负载：委托公共 dispatchOps 执行算子链
    cv::Mat mat = CTPreprocess::LoadRawPixelBuffer(pRaw, h, w, isU16, 0, big);
    mat = dispatchOps(mat, pOps, opsCount, pParams, paramsCount);

    // 2) 共享的预处理结果已经拿到；现在按 method 列表逐个做 8-bit 映射
    for (int mi = 0; mi < nMethods; ++mi) {
        const int method = pMethods[mi];
        cv::Mat out8u;
        if (mat.depth() != CV_8U) {
            if (method == -1) {
                out8u = normalizeTo8u(mat);
            } else {
                out8u = windowTo8u(mat, method);
            }
        } else out8u = mat;

        jbyteArray rgba;
        JniHelper::gray8uToRgbaJBytes(env, out8u, rgba);
        env->SetObjectArrayElement(outDisplays, mi, rgba);
        env->DeleteLocalRef(rgba);
    }

    // 3) outInfo 写 [outW, outH]
    if (outInfo && env->GetArrayLength(outInfo) >= 2) {
        jint *pI = env->GetIntArrayElements(outInfo, nullptr);
        pI[0] = mat.cols;
        pI[1] = mat.rows;
        env->ReleaseIntArrayElements(outInfo, pI, 0);
    }
    // P1-6: 浮点 outHuRange（HU 域 min/max 保留小数）
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

// =============================================================================
// 9) 动态调窗：用户指定 (windowCenter, windowWidth) 而非算法索引
//    核心思路（参考 CSDN 博客 https://blog.csdn.net/u013598963/article/details/121023205）：
//    逐像素线性映射 (L-W/2,0) ~ (L+W/2,255)，saturate_cast 截断。
//    这里直接复用 CtSeriesProcessor::applyWindow8u 实现。
// =============================================================================
/**
 * @param windowCenter  窗位 C（HU 域）
 * @param windowWidth   窗宽 W（HU 域）
 * @param outInfo       out [outW, outH, srcMin(int), srcMax(int)]
 * @param outHuRange    out [srcMin(double), srcMax(double)]
 * @return RGBA8888 字节
 */
static jbyteArray
native_processMedicalCTCustomWindow(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h,
                                    jint depth, jboolean big, jboolean isU16,
                                    jintArray ops, jdoubleArray params,
                                    jdouble windowCenter, jdouble windowWidth,
                                    jintArray info, jdoubleArray outHuRange) {
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    jint *pOps = env->GetIntArrayElements(ops, nullptr);
    jdouble *pParams = env->GetDoubleArrayElements(params, nullptr);
    jsize opsCount = env->GetArrayLength(ops);
    const jsize paramsCount = env->GetArrayLength(params);

    cv::Mat mat = CTPreprocess::LoadRawPixelBuffer(pRaw, h, w, isU16, 0, big);
    mat = dispatchOps(mat, pOps, opsCount, pParams, paramsCount);

    // 应用用户指定的窗宽窗位
    double c = windowCenter;
    double winW = windowWidth;
    if (winW < 1.0) winW = 1.0;
    LOGI("native_processMedicalCTCustomWindow: C=%.1f, W=%.1f", c, winW);

    cv::Mat out8u;
    if (mat.depth() != CV_8U) {
        out8u = CtSeriesProcessor::applyWindow8u(mat, c, winW, /*photometric=*/0);
    } else {
        out8u = mat;  // 8-bit 数据直接使用
    }

    jbyteArray res;
    JniHelper::gray8uToRgbaJBytes(env, out8u, res);

    if (info && env->GetArrayLength(info) >= 4) {
        jint *pI = env->GetIntArrayElements(info, nullptr);
        pI[0] = out8u.cols;
        pI[1] = out8u.rows;
        double mn, mx;
        cv::minMaxLoc(mat, &mn, &mx);
        pI[2] = (int) mn;
        pI[3] = (int) mx;
        env->ReleaseIntArrayElements(info, pI, 0);
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
    return res;
}

// =============================================================================
// JNI 注册
// =============================================================================
static const char *const kClassName = "com/example/rawpixeldeal/jni/RawPixelDealJni";
static const JNINativeMethod kMethods[] = {
        {"stringFromJNI",                       "()Ljava/lang/String;",
                (void *) native_stringFromJNI},
        {"getOpenCVVersion",                    "()Ljava/lang/String;",
                (void *) native_getOpenCVVersion},
        {"processRawGrayPixels",                "(II[B)[I",
                (void *) native_processRawGrayPixels},
        {"processRawToRgba",                    "(III[BIIIIIDI[I)[B",
                (void *) native_processRawToRgba},
        {"processCtSeries",                     "([[BIIIIDDIFIIIIDDIIIDDFFIFFIDDIDII[[B[D[I[I[I)Z",
                (void *) native_processCtSeries},
        {"tailorImage",                         "([BIIIIZIZID[B[I)[B",
                (void *) native_tailorImage},
        {"invertLut",                           "([BIIIIZ)[B",
                (void *) native_invertLut},
        {"processMedicalCT",                    "([BIIIZZ[I[DI[I[D)[B",
                (void *) native_processMedicalCT},
        {"processCTFullPipeline",               "([BIIIIFFZZ[I[D)[B",
                (void *) native_processCTFullPipeline},
        {"processCTTailorInvertWindowPipeline", "([BIIFFZI[I)[B",
                (void *) native_processCTTailorInvertWindowPipeline},
        {"processMedicalCTCompareWindows",      "([BIIIZZ[I[D[I[[B[I[D)V",
                (void *) native_processMedicalCTCompareWindows},
        {"processMedicalCTCustomWindow",          "([BIIIZZ[I[DDD[I[D)[B",
                (void *) native_processMedicalCTCustomWindow},
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
