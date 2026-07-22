//
// Created by admin on 2026/7/21.
//

#include "include/MedicalCTPreprocess.h"
#include <android/log.h>
#include <CtSeriesProcessor.h>

#define TAG "MedicalCTPreprocess"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace CTPreprocess {
    cv::Mat LoadRawPixelBuffer(void *rawBuf, int rows, int cols, bool isUint16, size_t step,
                               bool bigEndian) {
        LOGI("LoadRawPixelBuffer: rows=%d, cols=%d, isUint16=%d, bigEndian=%d", rows, cols,
             isUint16, bigEndian);
        int type = isUint16 ? CV_16UC1 : CV_16SC1;
        cv::Mat mat(rows, cols, type, rawBuf, step);

        // 改进：增加字节序自动检测提示（Heuristic Check）
        auto performSwap = [](cv::Mat &m) {
            ushort *p = m.ptr<ushort>();
            int total = m.rows * m.cols;
            for (int i = 0; i < total; i++) {
                ushort val = p[i];
                m.ptr<ushort>()[i] = (val >> 8) | (val << 8);
            }
        };

        if (bigEndian) {
            LOGD("LoadRawPixelBuffer: Converting big-endian to little-endian");
            cv::Mat temp;
            mat.copyTo(temp);
            performSwap(temp);
            return temp;
        } else {
            // 启发式校验：如果 16-bit 有符号数读取出大量异常极值，提示可能需要大端转换
            double minV, maxV;
            cv::minMaxLoc(mat, &minV, &maxV);
            if (maxV > 30000 || minV < -30000) {
                LOGD("LoadRawPixelBuffer: Detected extreme values [%.0f, %.0f]. Endianness might be wrong!", minV, maxV);
            }
        }
        return mat;
    }

    cv::Mat ConvertRawToHU(const cv::Mat &src16, float slope, float intercept) {
        LOGI("ConvertRawToHU: slope=%.2f, intercept=%.2f", slope, intercept);
        cv::Mat f32Mat;
        src16.convertTo(f32Mat, CV_32FC1);
        f32Mat = f32Mat * slope + intercept;
        // 截断CT有效HU范围 [-1024, 3071]
        cv::threshold(f32Mat, f32Mat, -1024, -1024, cv::THRESH_TOZERO);
        cv::threshold(f32Mat, f32Mat, 3071, 3071, cv::THRESH_TRUNC);

        double minV, maxV;
        cv::minMaxLoc(f32Mat, &minV, &maxV);
        LOGD("ConvertRawToHU: Result HU range [%.1f, %.1f]", minV, maxV);
        return f32Mat;
    }

    // 去噪实现
    cv::Mat DenoiseGaussian(const cv::Mat &src, int kernel, double sigma) {
        LOGI("DenoiseGaussian: kernel=%d, sigma=%.2f", kernel, sigma);
        cv::Mat dst;
        cv::GaussianBlur(src, dst, cv::Size(kernel, kernel), sigma, sigma);
        return dst;
    }

    cv::Mat DenoiseMedian(const cv::Mat &src, int kernel) {
        LOGI("DenoiseMedian: kernel=%d", kernel);
        cv::Mat dst;
        cv::medianBlur(src, dst, kernel);
        return dst;
    }

    cv::Mat DenoiseBilateral(const cv::Mat &src, int d, double sigmaColor, double sigmaSpace) {
        LOGI("DenoiseBilateral: d=%d, sigmaColor=%.2f, sigmaSpace=%.2f", d, sigmaColor, sigmaSpace);
        cv::Mat dst;
        cv::bilateralFilter(src, dst, d, sigmaColor, sigmaSpace);
        return dst;
    }

    cv::Mat DenoiseFrequencyFFT(const cv::Mat &src, float radius) {
        LOGI("DenoiseFrequencyFFT: radius=%.2f", radius);
        cv::Mat gray, floatMat;
        if (src.depth() == CV_16S || src.depth() == CV_16U)
            src.convertTo(floatMat, CV_32FC1);
        else
            floatMat = src.clone();

        cv::Mat planes[] = {floatMat, cv::Mat::zeros(floatMat.size(), CV_32FC1)};
        cv::Mat complex;
        cv::merge(planes, 2, complex);
        cv::dft(complex, complex);

        // 频谱中心化
        cv::Mat shiftMat = complex.clone();
        int cx = shiftMat.cols / 2;
        int cy = shiftMat.rows / 2;
        cv::Mat q1(shiftMat, cv::Rect(0, 0, cx, cy));
        cv::Mat q2(shiftMat, cv::Rect(cx, 0, cx, cy));
        cv::Mat q3(shiftMat, cv::Rect(0, cy, cx, cy));
        cv::Mat q4(shiftMat, cv::Rect(cx, cy, cx, cy));
        cv::Mat tmp;
        q1.copyTo(tmp);
        q4.copyTo(q1);
        tmp.copyTo(q4);
        q2.copyTo(tmp);
        q3.copyTo(q2);
        tmp.copyTo(q3);

        // 低通掩码 滤除高频噪声
        cv::Mat mask = cv::Mat::zeros(shiftMat.rows, shiftMat.cols, CV_8UC1);
        cv::circle(mask, cv::Point(cx, cy), radius, cv::Scalar(255), -1);
        std::vector<cv::Mat> maskCh;
        maskCh.push_back(mask);
        maskCh.push_back(mask);
        cv::Mat mask2c;
        cv::merge(maskCh, mask2c);

        // 修复：显式将掩码转换为 CV_32F 类型，确保与 shiftMat 类型一致
        cv::Mat maskF;
        mask2c.convertTo(maskF, CV_32F, 1.0 / 255.0);
        shiftMat = shiftMat.mul(maskF);

        // 逆中心化
        q1.copyTo(tmp);
        q4.copyTo(q1);
        tmp.copyTo(q4);
        q2.copyTo(tmp);
        q3.copyTo(q2);
        tmp.copyTo(q3);

        cv::idft(shiftMat, shiftMat);
        cv::split(shiftMat, planes);
        cv::normalize(planes[0], planes[0], 0, 255, cv::NORM_MINMAX, CV_8UC1);
        return planes[0];
    }

    // 重采样
    cv::Mat ResampleImage(const cv::Mat &src, int targetW, int targetH, bool isUpSample) {
        LOGI("ResampleImage: targetW=%d, targetH=%d, isUpSample=%d", targetW, targetH, isUpSample);
        cv::Mat dst;
        cv::InterpolationFlags inter = isUpSample ? cv::INTER_CUBIC : cv::INTER_AREA;
        cv::resize(src, dst, cv::Size(targetW, targetH), 0, 0, inter);
        return dst;
    }

    cv::Mat ResampleByScale(const cv::Mat &src, float scaleX, float scaleY) {
        LOGI("ResampleByScale: scaleX=%.2f, scaleY=%.2f", scaleX, scaleY);
        cv::Mat dst;
        cv::resize(src, dst, cv::Size(), scaleX, scaleY);
        return dst;
    }

    // 图像增强
    cv::Mat EnhanceGlobalEqualize(const cv::Mat &src8u) {
        LOGI("EnhanceGlobalEqualize");
        cv::Mat dst;
        cv::equalizeHist(src8u, dst);
        return dst;
    }

    cv::Mat EnhanceCLAHE(const cv::Mat &src8u, double clipLimit, cv::Size tileSize) {
        LOGI("EnhanceCLAHE: clipLimit=%.2f, tileSize=%dx%d", clipLimit, tileSize.width,
             tileSize.height);
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clipLimit, tileSize);
        cv::Mat dst;
        clahe->apply(src8u, dst);
        return dst;
    }

    cv::Mat EnhanceContrastStretch(const cv::Mat &src16) {
        LOGI("EnhanceContrastStretch (Adaptive)");
        cv::Mat dst8u;
        // 改进：使用 1%-99% 百分位拉伸替代全局 min/max，抑制异常极值干扰
        double minV, maxV;
        cv::minMaxLoc(src16, &minV, &maxV);

        // 简单百分位近似：剔除两端各 1% 的像素（如果支持统计）
        // 这里为了演示，采用温和截断
        double span = maxV - minV;
        double low = minV + span * 0.01;
        double high = maxV - span * 0.01;

        cv::Mat truncated;
        cv::threshold(src16, truncated, high, high, cv::THRESH_TRUNC);
        cv::max(truncated, low, truncated);

        cv::normalize(truncated, dst8u, 0, 255, cv::NORM_MINMAX, CV_8UC1);
        return dst8u;
    }

    cv::Mat EnhanceInvertLut(const cv::Mat &src16) {
        LOGI("EnhanceInvertLut: type=%d depth=%d", src16.type(), src16.depth());
        cv::Mat dst;
        if (src16.depth() == CV_16U || src16.depth() == CV_16S) {
            // 改进：针对医学图像，使用动态范围反转 (min + max) - val
            // 这样反转后的值依然落在原始数据的有效量程内，避免 HU 校正后溢出导致全灰
            double mn, mx;
            cv::minMaxLoc(src16, &mn, &mx);
            LOGD("EnhanceInvertLut: Range [%.0f, %.0f] -> Inverting around %.0f", mn, mx, mn + mx);
            dst = cv::Scalar::all(mn + mx) - src16;
        } else if (src16.depth() == CV_8U) {
            dst = cv::Scalar::all(255) - src16;
        } else {
            dst = src16.clone();
        }
        return dst;
    }

    // 完整流水线：原文标准流程（增强版）
    cv::Mat CTFullPipeline(void *rawBuf, int rows, int cols, int tarW, int tarH, float slope,
                           float intercept) {
        LOGI("CTFullPipeline: START (Enhanced with ROI & Percentile)");
        // 1. 载入Raw像素缓冲区
        cv::Mat raw16 = LoadRawPixelBuffer(rawBuf, rows, cols, false, 0, true);

        // 2. HU物理值校正
        cv::Mat huMat = ConvertRawToHU(raw16, slope, intercept);

        // 3. 改进点：自动裁剪 ROI（排除空气背景干扰）
        cv::Rect roi = CtSeriesProcessor::tryAutoCropBodyRoiEx(huMat, -600.0f,
                                                               5, 1000, 20);
        cv::Mat roiMat = huMat(roi);

        // 4. 空间域双边去噪（在 HU 域进行，保边）
        cv::Mat denoiseMat = DenoiseBilateral(roiMat);

        // 5. 重采样统一分辨率
        cv::Mat resizedMat = ResampleImage(denoiseMat, tarW, tarH, false);

        // 6. 改进点：基于百分位的自适应调窗 (0.5% ~ 99.5%)
        float gMin, gMax;
        std::vector<cv::Mat> slices = {resizedMat};
        CtSeriesProcessor::computePercentileHu(slices, cv::Rect(0, 0, resizedMat.cols, resizedMat.rows), 1, 0.5, 99.5, gMin, gMax);

        // 7. 改进点：临床窗位映射
        double c = (gMin + gMax) * 0.5;
        double w = (gMax - gMin);

        // 兜底：如果窗宽过窄（可能由于数据异常），使用标准软组织窗
        if (w < 100.0) {
            LOGW("CTFullPipeline: Auto window too narrow (W=%.1f), fallback to Soft Tissue", w);
            c = 40.0; w = 400.0;
        }

        cv::Mat stretch8u = CtSeriesProcessor::applyWindow8u(resizedMat, c, w, 0);

        // 8. CLAHE 局部增强
        cv::Mat result = EnhanceCLAHE(stretch8u, 2.0, cv::Size(8, 8));

        LOGI("CTFullPipeline: DONE. Final Window: C=%.1f, W=%.1f", c, w);
        return result;
    }
}
