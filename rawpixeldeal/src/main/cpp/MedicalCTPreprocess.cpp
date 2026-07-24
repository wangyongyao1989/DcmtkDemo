//
// Created by admin on 2026/7/21.
//

#include "include/MedicalCTPreprocess.h"
#include <android/log.h>
#include <CtSeriesProcessor.h>
#include <XrayProcessor.h>

#define TAG "MedicalCTPreprocess"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace CTPreprocess {
    /**
     * 载入 16-bit raw 像素缓冲区。
     *
     * @param rawBuf    16-bit raw 字节（rows * cols * 2 字节）
     * @param rows      图像高
     * @param cols      图像宽
     * @param isUint16  true=无符号 (CV_16UC1)，false=有符号 (CV_16SC1)。
     *                  **CT/HU 场景下必须传 true**。
     * @param step      步长（字节），传 0 走默认 cols*elemSize。
     * @param bigEndian 是否大端字节序；为 true 时会原地 swap 为 little-endian。
     */
    cv::Mat LoadRawPixelBuffer(void *rawBuf, int rows, int cols, bool isUint16, size_t step,
                               bool bigEndian) {
        LOGI("LoadRawPixelBuffer: rows=%d, cols=%d, isUint16=%d, bigEndian=%d", rows, cols,
             isUint16, bigEndian);

        // 校验 buffer 指针与几何
        if (rawBuf == nullptr) {
            LOGE("LoadRawPixelBuffer: rawBuf is null");
            return cv::Mat();
        }
        if (rows <= 0 || cols <= 0) {
            LOGE("LoadRawPixelBuffer: invalid geometry rows=%d cols=%d", rows, cols);
            return cv::Mat();
        }

        int type = isUint16 ? CV_16UC1 : CV_16SC1;
        cv::Mat mat(rows, cols, type, rawBuf, step);

        // 字节序反转：bigEndian 时把每个 16-bit 单元按字节翻转
        // P2-fix: 改为 clone 后再 swap，避免原地修改 JNI 缓冲区（pRaw 可能被
        //         Kotlin 端引用，原地写会导致数据不一致）。额外的全图拷贝开销
        //         仅在 bigEndian=true 时产生，对 little-endian 路径零影响。
        if (bigEndian) {
            LOGD("LoadRawPixelBuffer: Swapping big-endian to little-endian (cloned)");
            cv::Mat swapped;
            mat.copyTo(swapped); // 独立副本，不修改原始 JNI 缓冲区
            ushort *p = swapped.ptr<ushort>();
            const int total = swapped.rows * swapped.cols;
            for (int i = 0; i < total; i++) {
                const ushort val = p[i];
                p[i] = static_cast<ushort>((val >> 8) | (val << 8));
            }
            // 启发式校验：如果 16-bit 数读取出大量异常极值，提示可能需要大端转换
            double minV, maxV;
            cv::minMaxLoc(swapped, &minV, &maxV);
            if (maxV > 30000 || minV < -30000) {
                LOGD("LoadRawPixelBuffer: Detected extreme values [%.0f, %.0f]. Endianness might be wrong!",
                     minV, maxV);
            }
            return swapped;
        } else {
            // 启发式校验：如果 16-bit 有符号数读取出大量异常极值，提示可能需要大端转换
            double minV, maxV;
            cv::minMaxLoc(mat, &minV, &maxV);
            if (maxV > 30000 || minV < -30000) {
                LOGD("LoadRawPixelBuffer: Detected extreme values [%.0f, %.0f]. Endianness might be wrong!",
                     minV, maxV);
            }
        }
        return mat;
    }

    cv::Mat ConvertRawToHU(const cv::Mat &src16, float slope, float intercept) {
        LOGI("ConvertRawToHU: slope=%.2f, intercept=%.2f", slope, intercept);
        cv::Mat f32Mat;
        src16.convertTo(f32Mat, CV_32FC1);
        f32Mat = f32Mat * slope + intercept;
        // 截断到 CT 有效 HU 范围 [-1024, 3071]
        // 修复：原代码用 cv::threshold(... THRESH_TOZERO) 把所有 <= -1024 的值塞成 0
        //      这会把空气背景强行拉到 0 HU，与软组织混在一起。
        //      下限应使用 cv::max（值 < -1024 时夹到 -1024），
        //      上限应使用 cv::min（值 > 3071 时夹到 3071）。
        cv::max(f32Mat, -1024.0f, f32Mat);
        cv::min(f32Mat, 3071.0f, f32Mat);

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
        cv::Mat floatMat;
        if (src.depth() == CV_16S || src.depth() == CV_16U)
            src.convertTo(floatMat, CV_32FC1);
        else if (src.depth() == CV_8U)
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
        // 确保 cx/cy 非零，避免 Rect 异常
        cx = std::max(1, cx);
        cy = std::max(1, cy);
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
        cv::circle(mask, cv::Point(cx, cy), static_cast<int>(radius), cv::Scalar(255), -1);
        std::vector<cv::Mat> maskCh;
        maskCh.push_back(mask);
        maskCh.push_back(mask);
        cv::Mat mask2c;
        cv::merge(maskCh, mask2c);

        // 显式将掩码转换为 CV_32F 类型，确保与 shiftMat 类型一致
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

        cv::idft(shiftMat, shiftMat, cv::DFT_SCALE | cv::DFT_REAL_OUTPUT);
        cv::split(shiftMat, planes);
        // P0-fix: 保留 CV_32FC1 精度，不再强制归一化到 8-bit。
        //   原实现 cv::normalize(..., CV_8UC1) 会永久截断浮点精度，
        //   导致后续 HU_CONVERT / CLAHE 等算子拿到的是 256 级灰度而非 HU 域数据。
        //   现在返回实部 CV_32FC1，由最终显示阶段统一做 8-bit 映射。
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
        LOGI("EnhanceContrastStretch (Percentile)");
        cv::Mat dst8u;

        // P2-fix: 使用真正的像素计数百分位 (1%-99%) 替代原来的值域截断近似。
        //   原实现 low = minV + span*0.01 只是值域两端截断 1%，
        //   对严重偏斜的 CT 直方图无效。现在按累积分布计算真实百分位。
        double minV, maxV;
        cv::minMaxLoc(src16, &minV, &maxV);
        if (maxV <= minV) {
            // 全图同一值，直接输出黑图
            dst8u = cv::Mat::zeros(src16.size(), CV_8UC1);
            return dst8u;
        }

        // 构建 256-bin 直方图（足够精度且性能好）
        int nBins = 256;
        int histSize[] = {nBins};
        const float rangeF[] = {(float) minV, (float) (maxV + 1e-6)};
        const float *ranges[] = {rangeF};
        cv::Mat hist;
        cv::calcHist(&src16, 1, 0, cv::Mat(), hist, 1, histSize, ranges);

        // 累积分布找 1% 和 99% 百分位值
        int totalPixels = src16.total();
        int cumSum = 0;
        double low = minV, high = maxV;
        for (int i = 0; i < nBins; i++) {
            cumSum += static_cast<int>(hist.at<float>(i));
            double binVal = minV + (maxV - minV) * (i + 0.5) / nBins;
            if (low == minV && cumSum >= static_cast<int>(totalPixels * 0.01)) {
                low = binVal;
            }
            if (cumSum >= static_cast<int>(totalPixels * 0.99)) {
                high = binVal;
                break;
            }
        }
        if (high <= low) { low = minV; high = maxV; }
        LOGD("EnhanceContrastStretch: Percentile [%.2f, %.2f] from range [%.2f, %.2f]", low, high, minV, maxV);

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

    cv::Mat SharpenUSM(const cv::Mat &src, double sigma, double strength) {
        LOGI("SharpenUSM: sigma=%.2f, strength=%.2f", sigma, strength);
        cv::Mat blurred, sharp, dst;
        cv::GaussianBlur(src, blurred, cv::Size(0, 0), sigma, sigma);
        // sharp = src - blurred
        cv::addWeighted(src, 1.0, blurred, -1.0, 0, sharp);
        // dst = src + strength * sharp
        cv::addWeighted(src, 1.0, sharp, strength, 0, dst);
        return dst;
    }

    // 完整流水线：原文标准流程（增强版）
    cv::Mat CTFullPipeline(void *rawBuf, int rows, int cols, int tarW, int tarH, float slope,
                           float intercept, bool bigEndian, bool isUint16) {
        LOGI("CTFullPipeline: START (Enhanced with ROI & Percentile) bigEndian=%d isUint16=%d",
             bigEndian, isUint16);
        // 1. 载入Raw像素缓冲区
        cv::Mat raw16 = LoadRawPixelBuffer(rawBuf, rows, cols, isUint16, 0, bigEndian);

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
        CtSeriesProcessor::computePercentileHu(slices,
                                               cv::Rect(0, 0, resizedMat.cols, resizedMat.rows), 1,
                                               0.5, 99.5, gMin, gMax);

        // 7. 改进点：临床窗位映射
        double c = (gMin + gMax) * 0.5;
        double w = (gMax - gMin);

        // 兜底：如果窗宽过窄（可能由于数据异常），使用标准软组织窗
        if (w < 100.0) {
            LOGW("CTFullPipeline: Auto window too narrow (W=%.1f), fallback to Soft Tissue", w);
            c = 40.0;
            w = 400.0;
        }

        cv::Mat stretch8u = CtSeriesProcessor::applyWindow8u(resizedMat, c, w, 0);

        // 8. CLAHE 局部增强
        cv::Mat result = EnhanceCLAHE(stretch8u, 2.0, cv::Size(8, 8));

        LOGI("CTFullPipeline: DONE. Final Window: C=%.1f, W=%.1f", c, w);
        return result;
    }

    cv::Mat CTTailorInvertWindowPipeline(void *rawBuf, int rows, int cols,
                                         float slope, float intercept,
                                         bool bigEndian, int windowMethod,
                                         int &outMin, int &outMax) {
        LOGI("CTTailorInvertWindowPipeline: START method=%d bigEndian=%d", windowMethod, bigEndian);

        // 1. 载入 Raw 像素（CT/HU 场景下使用无符号 16-bit）
        cv::Mat raw16 = LoadRawPixelBuffer(rawBuf, rows, cols, /*isUint16=*/true, 0, bigEndian);

        // 2. 自动裁剪 (tailorImage 逻辑)
        int tx, ty;
        double ta;
        bool tok;
        cv::Mat cropped = XrayProcessor::tailor(raw16, 40000, true,
                                                25, 10.0, tx, ty, ta, tok);
        if (!tok || cropped.empty()) {
            LOGW("CTTailorInvertWindowPipeline: Tailor failed, using full image.");
            cropped = raw16.clone();
        }

        // P0-fix: 重排 Invert 和 HU_CONVERT 的顺序。
        //   原逻辑: raw → crop → InvertLUT → HU_CONVERT → denoise
        //   问题: EnhanceInvertLut 做的是 (min+max)-val，反转的是探测器原始计数值，
        //         反转后再做 pixel*slope+intercept 得到的 HU 无物理意义。
        //   修正: 先做 HU 校正，再在 HU 域做反转（如果确实需要反转的话）。
        // 3. HU 校正（先转换为物理 HU 域）
        cv::Mat huMat = ConvertRawToHU(cropped, slope, intercept);

        // 4. 颜色反转 (在 HU 域做 Invert LUTs)
        cv::Mat inverted = EnhanceInvertLut(huMat);

        // 5. 标准流水线去噪：双边滤波
        cv::Mat denoiseMat = DenoiseBilateral(inverted);

        // 6. 调窗：method=-1 走"百分位自适应"，否则统一委托 pickWindowCenterWidth
        double minV = 0.0, maxV = 0.0;
        cv::minMaxLoc(denoiseMat, &minV, &maxV);
        outMin = (int) minV;
        outMax = (int) maxV;

        double c = 127.5, w = 255.0;
        if (windowMethod == -1) {
            // Requirement: 不选择调窗算法时，仅执行"裁剪+反转+标准流程"
            // 采用标准百分位自适应映射 (0.5% - 99.5%) 以保证图像可见
            float gMin, gMax;
            std::vector<cv::Mat> slices = {denoiseMat};
            CtSeriesProcessor::computePercentileHu(slices,
                                                   cv::Rect(0, 0, denoiseMat.cols, denoiseMat.rows),
                                                   1, 0.5, 99.5, gMin, gMax);
            c = (gMin + gMax) * 0.5;
            w = (gMax - gMin);
            if (w < 100.0) {
                c = 40.0;
                w = 400.0;
            }
        } else {
            // 1/2/3 需要直方图；0/4/5 不需要
            int nBins = 256;
            std::vector<int> hist;
            const std::vector<int> *histPtr = nullptr;
            if (windowMethod == 1 || windowMethod == 2 || windowMethod == 3) {
                std::vector<cv::Mat> slices = {denoiseMat};
                CtSeriesProcessor::aggregateSeriesHistogram(slices,
                                                            cv::Rect(0, 0, denoiseMat.cols,
                                                                     denoiseMat.rows),
                                                            minV, maxV, nBins, 1, hist);
                histPtr = &hist;
            }
            CtSeriesProcessor::pickWindowCenterWidth(windowMethod, minV, maxV, histPtr, nBins, c, w);
        }

        if (w < 1.0) w = 1.0;
        cv::Mat stretch8u = CtSeriesProcessor::applyWindow8u(denoiseMat, c, w, 0);

        // 7. CLAHE 局部增强
        cv::Mat result = EnhanceCLAHE(stretch8u, 2.0, cv::Size(8, 8));

        LOGI("CTTailorInvertWindowPipeline: DONE. Final Window: C=%.1f, W=%.1f", c, w);
        return result;
    }
}
