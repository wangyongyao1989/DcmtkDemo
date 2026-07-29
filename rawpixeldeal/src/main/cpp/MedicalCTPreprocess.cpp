#include "include/MedicalCTPreprocess.h"
#include <android/log.h>
#include "include/CtSeriesProcessor.h"

#define TAG "MedicalCTPreprocess"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace CTPreprocess {

    cv::Mat LoadRawPixelBuffer(void *rawBuf, int rows, int cols, bool isUint16, size_t step, bool bigEndian) {
        if (rawBuf == nullptr || rows <= 0 || cols <= 0) return cv::Mat();
        int type = isUint16 ? CV_16UC1 : CV_16SC1;
        cv::Mat mat(rows, cols, type, rawBuf, step);
        if (bigEndian) {
            cv::Mat swapped;
            mat.copyTo(swapped);
            ushort *p = swapped.ptr<ushort>();
            const int total = swapped.rows * swapped.cols;
            for (int i = 0; i < total; i++) {
                const ushort val = p[i];
                p[i] = static_cast<ushort>((val >> 8) | (val << 8));
            }
            return swapped;
        }
        return mat;
    }

    cv::Mat ConvertRawToHU(const cv::Mat &src16, float slope, float intercept) {
        cv::Mat f32Mat;
        src16.convertTo(f32Mat, CV_32FC1);
        f32Mat = f32Mat * slope + intercept;
        cv::max(f32Mat, -1024.0f, f32Mat);
        cv::min(f32Mat, 3071.0f, f32Mat);
        return f32Mat;
    }

    cv::Mat DenoiseBilateral(const cv::Mat &src, int d, double sigmaColor, double sigmaSpace) {
        cv::Mat dst;
        cv::bilateralFilter(src, dst, d, sigmaColor, sigmaSpace);
        return dst;
    }

    cv::Mat EnhanceCLAHE(const cv::Mat &src8u, double clipLimit, cv::Size tileSize) {
        if (clipLimit <= 0.0) clipLimit = 3.0;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clipLimit, tileSize);
        cv::Mat dst;
        clahe->apply(src8u, dst);
        return dst;
    }

    cv::Mat EnhanceInvertLut(const cv::Mat &src) {
        cv::Mat dst;
        if (src.depth() == CV_16U || src.depth() == CV_16S || src.depth() == CV_32F) {
            double mn, mx;
            cv::minMaxLoc(src, &mn, &mx);
            dst = cv::Scalar::all(mn + mx) - src;
        } else if (src.depth() == CV_8U) {
            dst = cv::Scalar::all(255) - src;
        } else {
            dst = src.clone();
        }
        return dst;
    }

    cv::Mat SharpenUSM(const cv::Mat &src, double sigma, double strength) {
        cv::Mat blurred, sharp, dst;
        cv::GaussianBlur(src, blurred, cv::Size(0, 0), sigma, sigma);
        cv::addWeighted(src, 1.0, blurred, -1.0, 0, sharp);
        cv::addWeighted(src, 1.0, sharp, strength, 0, dst);
        return dst;
    }
}
