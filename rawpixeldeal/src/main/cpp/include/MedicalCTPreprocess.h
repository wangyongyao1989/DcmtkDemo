//
// Created by admin on 2026/7/21.
//

#ifndef DCMTKDEMO_MEDICALCTPREPROCESS_H
#define DCMTKDEMO_MEDICALCTPREPROCESS_H

#include <vector>
#include <iostream>
#include "opencv2/opencv.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/core/mat.hpp"

namespace CTPreprocess {
    // ===================== 一、Raw裸像素缓冲区载入（硬件void*内存，无DICOM） =====================
    /**
     * @brief 采集卡/FPGA裸16bit像素内存直接映射cv::Mat（零拷贝）
     * @param rawBuf 硬件原始像素缓冲区指针 int16/uint16
     * @param rows 图像高
     * @param cols 图像宽
     * @param isUint16 true=CV_16UC1, false=CV_16SC1(CT标准HU存储)
     * @param step 硬件行对齐步长（采集卡存在填充时传入，0自动计算）
     * @param bigEndian 设备Raw大端字节序，true自动反转字节
     * @return 16bit单通道cv::Mat
     */
    cv::Mat
    LoadRawPixelBuffer(void *rawBuf, int rows, int cols, bool isUint16 = false, size_t step = 0,
                       bool bigEndian = true);

    // HU值校正：HU = pixel * slope + intercept
    cv::Mat ConvertRawToHU(const cv::Mat &src16, float slope = 1.0f, float intercept = -1024.0f);

    // ===================== 二、图像去噪（对应原文：空间域/频域去噪） =====================
    // 空间域1：高斯滤波（高斯噪声，Sinogram投影预处理）
    cv::Mat DenoiseGaussian(const cv::Mat &src, int kernel = 5, double sigma = 0);

    // 空间域2：中值滤波（椒盐脉冲噪声，采集卡跳变像素）
    cv::Mat DenoiseMedian(const cv::Mat &src, int kernel = 3);

    // 空间域3：双边滤波（保留器官边缘，CT切片降噪首选）
    cv::Mat
    DenoiseBilateral(const cv::Mat &src, int d = 5, double sigmaColor = 50, double sigmaSpace = 50);

    // 频域去噪：傅里叶变换滤除高频噪声（环形伪影、周期性条纹）
    cv::Mat DenoiseFrequencyFFT(const cv::Mat &src, float radius = 30.0f);

    // ===================== 三、图像重采样（统一分辨率，上下采样） =====================
    /**
     * @brief 图像重采样 统一空间分辨率
     * @param src 输入图像
     * @param targetW 目标宽度
     * @param targetH 目标高度
     * @param isUpSample true上采样(插值) false下采样
     * @return 重采样后Mat
     */
    cv::Mat ResampleImage(const cv::Mat &src, int targetW, int targetH, bool isUpSample = false);

    // 按缩放比例重采样
    cv::Mat ResampleByScale(const cv::Mat &src, float scaleX, float scaleY);

    // ===================== 四、图像增强（直方图均衡 + 对比度拉伸） =====================
    // 1. 全局直方图均衡化 equalizeHist（原文基础均衡）
    cv::Mat EnhanceGlobalEqualize(const cv::Mat &src8u);

    // 2. CLAHE自适应直方图均衡（CT工业标准，抑制噪声放大）
    cv::Mat
    EnhanceCLAHE(const cv::Mat &src8u, double clipLimit = 2.0, cv::Size tileSize = cv::Size(8, 8));

    // 3. 对比度线性拉伸（灰度区间压缩到0~255，原文对比度拉伸）
    cv::Mat EnhanceContrastStretch(const cv::Mat &src16);

    // ===================== 完整预处理流水线（对应原文标准流程） =====================
    /**
     * 流水线：Raw Buffer → HU校正 → 去噪 → 重采样 → 增强输出8bit可视化图
     */
    cv::Mat CTFullPipeline(void *rawBuf, int rows, int cols, int tarW, int tarH,
                           float slope = 1.0f, float intercept = -1024.0f);
}


#endif //DCMTKDEMO_MEDICALCTPREPROCESS_H
