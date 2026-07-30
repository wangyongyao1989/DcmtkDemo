#include "include/MedicalCTPreprocess.h"
#include <android/log.h>
#include "include/CtSeriesProcessor.h"

#define TAG "MedicalCTPreprocess"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace CTPreprocess {

    /**
     * 加载原始像素缓冲区到 OpenCV Mat。
     * 支持 16-bit 有符号/无符号数据，并处理大端序(Big-Endian)转换。
     */
    cv::Mat LoadRawPixelBuffer(void *rawBuf, int rows, int cols, bool isUint16, size_t step, bool bigEndian) {
        if (rawBuf == nullptr || rows <= 0 || cols <= 0) return cv::Mat();
        int type = isUint16 ? CV_16UC1 : CV_16SC1;
        cv::Mat mat(rows, cols, type, rawBuf, step);
        if (bigEndian) {
            // 大端序处理：高低字节交换
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

    /**
     * 将原始 16-bit 像素转换为 HU (Hounsfield Unit) 值。
     * 公式: HU = raw * slope + intercept
     */
    cv::Mat ConvertRawToHU(const cv::Mat &src16, float slope, float intercept) {
        cv::Mat hu;
        src16.convertTo(hu, CV_32F, static_cast<double>(slope), static_cast<double>(intercept));
        return hu;
    }

    /**
     * 双边滤波去噪。
     * OpenCV 的 bilateralFilter 支持 8-bit 或 32-bit 浮点。
     */
    cv::Mat DenoiseBilateral(const cv::Mat &src, int d, double sigmaColor, double sigmaSpace) {
        if (src.empty()) return src;
        cv::Mat dst;
        // 如果是 16 位，双边滤波不支持，需要转为 32F
        if (src.depth() == CV_16U || src.depth() == CV_16S) {
            cv::Mat tmp;
            src.convertTo(tmp, CV_32F);
            cv::bilateralFilter(tmp, dst, d, sigmaColor, sigmaSpace);
        } else {
            cv::bilateralFilter(src, dst, d, sigmaColor, sigmaSpace);
        }
        return dst;
    }

    /**
     * 对 8-bit 图像应用限制对比度自适应直方图均衡化 (CLAHE)。
     */
    cv::Mat EnhanceCLAHE(const cv::Mat &src8u, double clipLimit, cv::Size tileSize) {
        if (src8u.empty() || src8u.depth() != CV_8U) return src8u;
        cv::Mat dst;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clipLimit, tileSize);
        clahe->apply(src8u, dst);
        return dst;
    }

    /**
     * 反相 (Negative)。
     */
    cv::Mat EnhanceInvertLut(const cv::Mat &src) {
        if (src.empty()) return src;
        cv::Mat dst;
        if (src.depth() == CV_8U) {
            cv::subtract(cv::Scalar::all(255), src, dst);
        } else if (src.depth() == CV_16U) {
            cv::subtract(cv::Scalar::all(65535), src, dst);
        } else {
            // 对于 16S, 32F 等，根据当前范围动态计算反相
            double mn, mx;
            cv::minMaxLoc(src, &mn, &mx);
            cv::subtract(cv::Scalar::all(mx + mn), src, dst);
        }
        return dst;
    }

    /**
     * 简单的锐化算子：USM (Unsharp Mask) 增强。
     * 公式: dst = src * 1.0 + (src - blurred) * strength
     */
    cv::Mat SharpenUSM(const cv::Mat &src, double sigma, double strength) {
        cv::Mat blurred, sharp, dst;
        cv::GaussianBlur(src, blurred, cv::Size(0, 0), sigma, sigma);
        cv::addWeighted(src, 1.0, blurred, -1.0, 0, sharp);
        cv::addWeighted(src, 1.0, sharp, strength, 0, dst);
        return dst;
    }

    // =========================================================================
    // ImageProcessor implementation
    // =========================================================================

    double ImageProcessor::getScaledValue(double value, double a, double b, double rawMin, double rawMax) {
        if (rawMax == rawMin) return a;
        double k = (b - a) / (rawMax - rawMin);
        return a + k * (value - rawMin);
    }

    cv::Mat ImageProcessor::convertToGrayScale(const cv::Mat& src) {
        cv::Mat gray;
        if (src.channels() == 4) {
            cv::cvtColor(src, gray, cv::COLOR_RGBA2GRAY);
        } else if (src.channels() == 3) {
            cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
        } else if (src.channels() == 2) {
            cv::cvtColor(src, gray, cv::COLOR_BGR5652GRAY);
        } else {
            gray = src.clone();
        }
        return gray;
    }

    cv::Mat ImageProcessor::appBrightnessContrast(const cv::Mat& src, double contrast, double brightness, double min, double max) {
        if (contrast > 0 || brightness != 0.0) {
            double brightnessPar = getScaledValue(brightness, -100.0, 100.0, min, max);
            double contrastPar = getScaledValue(contrast, 1.0, 1.8, min, max);
            cv::Mat dst;
            src.convertTo(dst, src.type(), contrastPar, brightnessPar);
            return dst;
        }
        return src.clone();
    }

    cv::Mat ImageProcessor::applySharpen(const cv::Mat& src, double sharpen, double min, double max) {
        if (sharpen > 0) {
            double sharpenDegreePar = getScaledValue(sharpen, 0.0, 1.5, min, max);
            cv::Mat blurred;
            cv::GaussianBlur(src, blurred, cv::Size(0, 0), 20.0);
            cv::Mat sharpened;
            double alpha = 1.0 + sharpenDegreePar * 2.0;
            double beta = -sharpenDegreePar * 1.5;
            cv::addWeighted(src, alpha, blurred, beta, 0.0, sharpened);
            return sharpened;
        }
        return src.clone();
    }

    cv::Mat ImageProcessor::applyInvertedColor(const cv::Mat& mat, bool invert) {
        cv::Mat dst = mat.clone();
        if (invert) {
            cv::bitwise_not(dst, dst);
        }
        return dst;
    }

    cv::Mat ImageProcessor::applyFalseColor(const cv::Mat& mat, bool falseColor) {
        cv::Mat dst = mat.clone();
        if (falseColor) {
            if (dst.channels() == 1) {
                cv::applyColorMap(dst, dst, cv::COLORMAP_JET);
            }
        }
        return dst;
    }

    cv::Mat ImageProcessor::applyEmbossingEffect(const cv::Mat& src, bool embossed) {
        if (embossed) {
            cv::Mat blurred;
            cv::GaussianBlur(src, blurred, cv::Size(3, 3), 0.0);

            cv::Mat xGrad, yGrad;
            cv::Sobel(blurred, xGrad, CV_32F, 1, 0, 3);
            cv::Sobel(blurred, yGrad, CV_32F, 0, 1, 3);

            cv::Mat dst;
            cv::subtract(xGrad, yGrad, dst);
            dst.convertTo(dst, CV_32F, 6.0, 110.0);
            dst.convertTo(dst, CV_8UC1);
            return dst;
        }
        return src.clone();
    }

    cv::Mat ImageProcessor::applyRotation(const cv::Mat& src, double angle) {
        if (src.empty() || angle == 0.0) {
            return src.clone();
        }

        cv::Mat rotatedImage;
        if (angle == 90.0 || angle == -270.0) {
            cv::transpose(src, rotatedImage);
            cv::flip(rotatedImage, rotatedImage, 1);
        } else if (angle == -90.0 || angle == 270.0) {
            cv::transpose(src, rotatedImage);
            cv::flip(rotatedImage, rotatedImage, 0);
        } else if (angle == 180.0 || angle == -180.0) {
            cv::flip(src, rotatedImage, -1);
        } else {
            return src.clone();
        }
        return rotatedImage;
    }

    cv::Mat ImageProcessor::process(cv::Mat& src, double contrast, double brightness, double sharpenDegree,
                                    bool invert, bool falseColor, bool relief, double min, double max) {
        cv::Mat gray = convertToGrayScale(src);

        cv::Mat bcResult = appBrightnessContrast(gray, contrast, brightness, min, max);
        cv::Mat sharpenResult = applySharpen(bcResult, sharpenDegree, min, max);

        cv::Mat invertResult = applyInvertedColor(sharpenResult, invert);
        cv::Mat falseColorResult = applyFalseColor(invertResult, falseColor);

        cv::Mat finalResult = applyEmbossingEffect(falseColorResult, relief);

        return finalResult;
    }
}
