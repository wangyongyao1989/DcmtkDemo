#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <opencv2/opencv.hpp>

namespace ImageProcessor {
    cv::Mat process(cv::Mat& src, double contrast, double brightness, double sharpenDegree,
                    bool invert, bool falseColor, bool relief, double min, double max);

    cv::Mat applyBrightnessContrast(const cv::Mat& src, double contrast, double brightness, double min, double max);
    cv::Mat applySharpen(const cv::Mat& src, double sharpen, double min, double max);
    void applyInvertedColor(cv::Mat& mat, bool invert);
    void applyFalseColor(cv::Mat& mat, bool falseColor);
    cv::Mat applyEmbossingEffect(const cv::Mat& src, bool embossed);
    cv::Mat applyRotation(const cv::Mat& src, double angle);

    double getScaledValue(double value, double a, double b, double rawMin, double rawMax);
}

#endif // IMAGE_PROCESSOR_H
