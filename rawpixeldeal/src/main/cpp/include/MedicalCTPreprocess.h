#ifndef DCMTKDEMO_MEDICALCTPREPROCESS_H
#define DCMTKDEMO_MEDICALCTPREPROCESS_H

#include "opencv2/opencv.hpp"

namespace CTPreprocess {
    enum class Op : int {
        BILATERAL      = 3,
        CLAHE          = 8,
        HU_CONVERT     = 10,
        TAILOR         = 11,
        INVERT_LUT     = 12,
        FEATURE_SHARPEN= 13,
    };

    cv::Mat LoadRawPixelBuffer(void *rawBuf, int rows, int cols, bool isUint16 = true, size_t step = 0, bool bigEndian = true);

    cv::Mat ConvertRawToHU(const cv::Mat &src16, float slope = 1.0f, float intercept = -1024.0f);

    cv::Mat DenoiseBilateral(const cv::Mat &src, int d = 5, double sigmaColor = 50, double sigmaSpace = 50);

    cv::Mat EnhanceCLAHE(const cv::Mat &src8u, double clipLimit = 2.0, cv::Size tileSize = cv::Size(8, 8));

    cv::Mat EnhanceInvertLut(const cv::Mat &src);

    cv::Mat SharpenUSM(const cv::Mat &src, double sigma = 1.5, double strength = 0.6);
}

#endif //DCMTKDEMO_MEDICALCTPREPROCESS_H
