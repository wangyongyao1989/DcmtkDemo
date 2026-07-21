//
// Created by admin on 2026/7/21.
//

#include "include/MedicalCTPreprocess.h"

namespace CTPreprocess {
    cv::Mat LoadRawPixelBuffer(void *rawBuf, int rows, int cols, bool isUint16, size_t step,
                               bool bigEndian) {
        int type = isUint16 ? CV_16UC1 : CV_16SC1;
        cv::Mat mat(rows, cols, type, rawBuf, step);

        // 大端字节序转换
        if (bigEndian) {
            cv::Mat temp;
            mat.copyTo(temp);
            ushort *p = temp.ptr<ushort>();
            int total = rows * cols;
            for (int i = 0; i < total; i++) {
                ushort val = p[i];
                p[i] = (val >> 8) | (val << 8); // 高低字节交换
            }
            return temp;
        }
        return mat;
    }

    cv::Mat ConvertRawToHU(const cv::Mat &src16, float slope, float intercept) {
        cv::Mat f32Mat;
        src16.convertTo(f32Mat, CV_32FC1);
        f32Mat = f32Mat * slope + intercept;
        // 截断CT有效HU范围 [-1024, 3071]
        cv::threshold(f32Mat, f32Mat, -1024, -1024, cv::THRESH_TOZERO);
        cv::threshold(f32Mat, f32Mat, 3071, 3071, cv::THRESH_TRUNC);
        return f32Mat;
    }

    // 去噪实现
    cv::Mat DenoiseGaussian(const cv::Mat &src, int kernel, double sigma) {
        cv::Mat dst;
        cv::GaussianBlur(src, dst, cv::Size(kernel, kernel), sigma, sigma);
        return dst;
    }

    cv::Mat DenoiseMedian(const cv::Mat &src, int kernel) {
        cv::Mat dst;
        cv::medianBlur(src, dst, kernel);
        return dst;
    }

    cv::Mat DenoiseBilateral(const cv::Mat &src, int d, double sigmaColor, double sigmaSpace) {
        cv::Mat dst;
        cv::bilateralFilter(src, dst, d, sigmaColor, sigmaSpace);
        return dst;
    }

    cv::Mat DenoiseFrequencyFFT(const cv::Mat &src, float radius) {
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
        shiftMat = shiftMat.mul(mask2c / 255.0);

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
        cv::Mat dst;
        cv::InterpolationFlags inter = isUpSample ? cv::INTER_CUBIC : cv::INTER_AREA;
        cv::resize(src, dst, cv::Size(targetW, targetH), 0, 0, inter);
        return dst;
    }

    cv::Mat ResampleByScale(const cv::Mat &src, float scaleX, float scaleY) {
        cv::Mat dst;
        cv::resize(src, dst, cv::Size(), scaleX, scaleY);
        return dst;
    }

    // 图像增强
    cv::Mat EnhanceGlobalEqualize(const cv::Mat &src8u) {
        cv::Mat dst;
        cv::equalizeHist(src8u, dst);
        return dst;
    }

    cv::Mat EnhanceCLAHE(const cv::Mat &src8u, double clipLimit, cv::Size tileSize) {
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clipLimit, tileSize);
        cv::Mat dst;
        clahe->apply(src8u, dst);
        return dst;
    }

    cv::Mat EnhanceContrastStretch(const cv::Mat &src16) {
        cv::Mat dst8u;
        cv::normalize(src16, dst8u, 0, 255, cv::NORM_MINMAX, CV_8UC1);
        return dst8u;
    }

    // 完整流水线：原文标准流程
    cv::Mat CTFullPipeline(void *rawBuf, int rows, int cols, int tarW, int tarH, float slope,
                           float intercept) {
        // 1. 载入Raw像素缓冲区
        cv::Mat raw16 = LoadRawPixelBuffer(rawBuf, rows, cols, false, 0, true);
        // 2. HU物理值校正
        cv::Mat huMat = ConvertRawToHU(raw16, slope, intercept);
        // 3. 空间域双边去噪（医学图像首选）
        cv::Mat denoiseMat = DenoiseBilateral(huMat);
        // 4. 重采样统一分辨率
        cv::Mat resizedMat = ResampleImage(denoiseMat, tarW, tarH, false);
        // 5. 对比度拉伸 + CLAHE增强
        cv::Mat stretch8u = EnhanceContrastStretch(resizedMat);
        cv::Mat result = EnhanceCLAHE(stretch8u);
        return result;
    }
}