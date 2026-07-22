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
                   jint minArea, jboolean sobel, jint morph, jdouble otsuLow, jbyteArray outCropped,
                   jintArray outInfo) {
    std::vector<uint8_t> raw;
    if (!JniHelper::copyJByteArray(env, rawBuf, raw)) return nullptr;
    cv::Mat sv = JniHelper::wrapRawMat(raw.data(), w, h, bits, sign);

    int outX, outY;
    double angle;
    bool ok;
    cv::Mat cropped = XrayProcessor::tailor(sv, minArea,
                                            sobel, morph,
                                            otsuLow, outX,
                                            outY, angle,
                                            ok);
    if (!ok) return XrayProcessor::buildFullImageResult(env, sv, outInfo);

    jbyteArray outBytes = env->NewByteArray(
            static_cast<jsize>(cropped.total() * cropped.elemSize()));
    env->SetByteArrayRegion(outBytes, 0, static_cast<jsize>(cropped.total() * cropped.elemSize()),
                            reinterpret_cast<const jbyte *>(cropped.data));

    if (outInfo) {
        jint info[6] = {outX, outY, cropped.cols, cropped.rows, (jint) (angle * 1000), 1};
        env->SetIntArrayRegion(outInfo, 0, 6, info);
    }
    return outBytes;
}

// =============================================================================
// 6) 通用医学预处理
// =============================================================================
static jbyteArray
native_processMedicalCT(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h, jint depth,
                        jboolean big, jboolean isU16,
                        jintArray ops, jdoubleArray params, jintArray info) {
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    jint *pOps = env->GetIntArrayElements(ops, nullptr);
    jdouble *pParams = env->GetDoubleArrayElements(params, nullptr);
    jsize opsCount = env->GetArrayLength(ops);

    cv::Mat mat = CTPreprocess::LoadRawPixelBuffer(pRaw, h, w, isU16, 0, big);
    int pIdx = 0;
    for (int i = 0; i < opsCount; ++i) {
        int op = pOps[i];
        if (op == 1)
            mat = CTPreprocess::DenoiseGaussian(mat, (int) pParams[pIdx++], pParams[pIdx++]);
        else if (op == 2) mat = CTPreprocess::DenoiseMedian(mat, (int) pParams[pIdx++]);
        else if (op == 3)
            mat = CTPreprocess::DenoiseBilateral(mat, (int) pParams[pIdx++], pParams[pIdx++],
                                                 pParams[pIdx++]);
        else if (op == 4) mat = CTPreprocess::DenoiseFrequencyFFT(mat, (float) pParams[pIdx++]);
        else if (op == 5) {
            int tw = (int) pParams[pIdx++];
            int th = (int) pParams[pIdx++];
            mat = CTPreprocess::ResampleImage(mat, tw, th, pParams[pIdx++] > 0.5);
        } else if (op == 6)
            mat = CTPreprocess::ResampleByScale(mat, (float) pParams[pIdx++],
                                                (float) pParams[pIdx++]);
        else if (op == 7) {
            if (mat.depth() != CV_8U) {
                double mn, mx;
                cv::minMaxLoc(mat, &mn, &mx);
                mat.convertTo(mat, CV_8U, 255.0 / (mx - mn + 1e-7),
                              -mn * 255.0 / (mx - mn + 1e-7));
            }
            mat = CTPreprocess::EnhanceGlobalEqualize(mat);
        } else if (op == 8) {
            double c = pParams[pIdx++];
            int tx = (int) pParams[pIdx++];
            int ty = (int) pParams[pIdx++];
            if (mat.depth() != CV_8U && mat.depth() != CV_16U) {
                double mn, mx;
                cv::minMaxLoc(mat, &mn, &mx);
                mat.convertTo(mat, CV_8U, 255.0 / (mx - mn + 1e-7),
                              -mn * 255.0 / (mx - mn + 1e-7));
            }
            mat = CTPreprocess::EnhanceCLAHE(mat, c, cv::Size(tx, ty));
        } else if (op == 9) mat = CTPreprocess::EnhanceContrastStretch(mat);
        else if (op == 10)
            mat = CTPreprocess::ConvertRawToHU(mat, (float) pParams[pIdx++],
                                               (float) pParams[pIdx++]);
        else if (op == 11) {
            int outX, outY;
            double angle;
            bool ok;
            int minArea = (int) pParams[pIdx++];
            bool sobel = pParams[pIdx++] > 0.5;
            int morph = (int) pParams[pIdx++];
            double otsu = pParams[pIdx++];
            cv::Mat cropped = XrayProcessor::tailor(mat, minArea,
                                                    sobel, morph,
                                                    otsu, outX, outY,
                                                    angle, ok);
            if (ok && !cropped.empty()) mat = cropped;
        }
    }

    cv::Mat out8u;
    if (mat.depth() != CV_8U) {
        double mn, mx;
        cv::minMaxLoc(mat, &mn, &mx);
        mat.convertTo(out8u, CV_8U, 255.0 / (mx - mn + 1e-7),
                      -mn * 255.0 / (mx - mn + 1e-7));
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
    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    env->ReleaseIntArrayElements(ops, pOps, JNI_ABORT);
    env->ReleaseDoubleArrayElements(params, pParams, JNI_ABORT);
    return res;
}

static jbyteArray
native_processCTFullPipeline(JNIEnv *env, jclass, jbyteArray rawBuf, jint w, jint h, jint tw,
                             jint th, jfloat slope, jfloat intercept, jintArray info) {
    jbyte *pRaw = env->GetByteArrayElements(rawBuf, nullptr);
    cv::Mat resMat = CTPreprocess::CTFullPipeline(pRaw, h, w, tw, th, slope, intercept);
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
    env->ReleaseByteArrayElements(rawBuf, pRaw, JNI_ABORT);
    return res;
}

// =============================================================================
// JNI 注册
// =============================================================================
static const char *const kClassName = "com/example/rawpixeldeal/jni/RawPixelDealJni";
static const JNINativeMethod kMethods[] = {
        {"stringFromJNI",         "()Ljava/lang/String;",
                (void *) native_stringFromJNI},
        {"getOpenCVVersion",      "()Ljava/lang/String;",
                (void *) native_getOpenCVVersion},
        {"processRawGrayPixels",  "(II[B)[I",
                (void *) native_processRawGrayPixels},
        {"processRawToRgba",      "(III[BIIIIIDI[I)[B",
                (void *) native_processRawToRgba},
        {"processCtSeries",       "([[BIIIIDDIFIIIIDDIIIDDFFIFFIDDIDII[[B[D[I[I[I)Z",
                (void *) native_processCtSeries},
        {"tailorImage",           "([BIIIIIZID[B[I)[B",
                (void *) native_tailorImage},
        {"processMedicalCT",      "([BIIIZZ[I[D[I)[B",
                (void *) native_processMedicalCT},
        {"processCTFullPipeline", "([BIIIIFF[I)[B",
                (void *) native_processCTFullPipeline},
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
