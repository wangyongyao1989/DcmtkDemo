#include "include/ImageProcessor.h"

namespace ImageProcessor {

    double getScaledValue(double value, double a, double b, double rawMin, double rawMax) {
        if (rawMax == rawMin) return a;
        double k = (b - a) / (rawMax - rawMin);
        return a + k * (value - rawMin);
    }

    cv::Mat applyBrightnessContrast(const cv::Mat& src, double contrast, double brightness, double min, double max) {
        if (contrast > 0 || brightness != 0.0) {
            double brightnessPar = getScaledValue(brightness, -100.0, 100.0, min, max);
            double contrastPar = getScaledValue(contrast, 1.0, 1.8, min, max);
            cv::Mat dst;
            src.convertTo(dst, src.type(), contrastPar, brightnessPar);
            return dst;
        }
        return src.clone();
    }

    cv::Mat applySharpen(const cv::Mat& src, double sharpen, double min, double max) {
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

    void applyInvertedColor(cv::Mat& mat, bool invert) {
        if (invert) {
            cv::bitwise_not(mat, mat);
        }
    }

    void applyFalseColor(cv::Mat& mat, bool falseColor) {
        if (falseColor) {
            if (mat.channels() == 1) {
                cv::applyColorMap(mat, mat, cv::COLORMAP_JET);
            }
        }
    }

    cv::Mat applyEmbossingEffect(const cv::Mat& src, bool embossed) {
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

    cv::Mat applyRotation(const cv::Mat& src, double angle) {
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

    cv::Mat process(cv::Mat& src, double contrast, double brightness, double sharpenDegree,
                    bool invert, bool falseColor, bool relief, double min, double max) {

        cv::Mat gray;
        if (src.channels() == 4) {
            cv::cvtColor(src, gray, cv::COLOR_RGBA2GRAY);
        } else if (src.channels() == 3) {
            cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
        } else if (src.channels() == 2) {
            // Probably RGB_565 (Android format)
            cv::cvtColor(src, gray, cv::COLOR_BGR5652GRAY);
        } else {
            gray = src.clone();
        }

        cv::Mat bcResult = applyBrightnessContrast(gray, contrast, brightness, min, max);
        cv::Mat sharpenResult = applySharpen(bcResult, sharpenDegree, min, max);

        cv::Mat current = sharpenResult;
        applyInvertedColor(current, invert);
        applyFalseColor(current, falseColor);

        cv::Mat finalResult = applyEmbossingEffect(current, relief);

        return finalResult;
    }
}
