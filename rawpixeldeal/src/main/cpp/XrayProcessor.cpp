#include "include/XrayProcessor.h"
#include <opencv2/imgproc.hpp>
#include <android/log.h>
#include <vector>
#include <algorithm>

#define TAG "XrayProcessor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

namespace XrayProcessor {

    jbyteArray buildFullImageResult(JNIEnv *env, const cv::Mat &sv, jintArray outInfo) {
        cv::Mat out16;
        if (sv.type() == CV_16S || sv.type() == CV_16U) out16 = sv.clone();
        else sv.convertTo(out16, CV_16S);
        jbyteArray outBytes = env->NewByteArray(static_cast<jsize>(out16.total() * out16.elemSize()));
        if (outBytes != nullptr) {
            env->SetByteArrayRegion(outBytes, 0, static_cast<jsize>(out16.total() * out16.elemSize()),
                                    reinterpret_cast<const jbyte *>(out16.data));
        }
        if (outInfo != nullptr && env->GetArrayLength(outInfo) >= 6) {
            jint info[6] = {0, 0, sv.cols, sv.rows, 0, 0};
            env->SetIntArrayRegion(outInfo, 0, 6, info);
        }
        return outBytes;
    }

    cv::Mat tailor(const cv::Mat &sv, int minAreaThreshold, bool enableSobel,
                  int morphCross, double otsuThresholdLow,
                  int &outX, int &outY, double &outAngle, bool &okFlag) {
        okFlag = false; outX = 0; outY = 0; outAngle = 0.0;
        int width = sv.cols; int height = sv.rows;

        cv::Mat img8u;
        { cv::Mat tmp32f; sv.convertTo(tmp32f, CV_32F, 1.0 / 256.0, 0.0); cv::convertScaleAbs(tmp32f, img8u); }

        cv::Mat otsu;
        cv::threshold(img8u, otsu, otsuThresholdLow, 255.0, cv::THRESH_OTSU);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(otsu, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        if (contours.empty()) return cv::Mat();

        auto maxIt = std::max_element(contours.begin(), contours.end(),
            [](const std::vector<cv::Point> &a, const std::vector<cv::Point> &b) { return cv::contourArea(a) < cv::contourArea(b); });
        const double maxArea = cv::contourArea(*maxIt);

        cv::RotatedRect rr;
        { std::vector<cv::Point2f> pts2f; for (const auto &p: *maxIt) pts2f.emplace_back(p.x, p.y); rr = cv::minAreaRect(pts2f); }
        double angle = rr.angle;
        if (std::abs(angle) > 45.0) angle += 90.0;
        if (std::abs(angle) < 1e-3) angle = 0.0;

        cv::Mat rotated, img8uRot;
        if (std::abs(angle) > 0.5) {
            cv::Point2f center(static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f);
            cv::Mat M = cv::getRotationMatrix2D(center, angle, 1.0);
            cv::warpAffine(sv, rotated, M, cv::Size(width, height), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));
            cv::warpAffine(img8u, img8uRot, M, cv::Size(width, height), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));
            outAngle = angle;
        } else { rotated = sv.clone(); img8uRot = img8u.clone(); }

        cv::Rect boundingRectAll(0, 0, width, height);
        if (enableSobel && morphCross > 0) {
            cv::Mat sharpKernel = (cv::Mat_<float>(3, 3) << -1, -1, -1, -1, 9, -1, -1, -1, -1);
            cv::Mat sharpened; cv::filter2D(img8uRot, sharpened, CV_32F, sharpKernel, cv::Point(-1, -1), 0);
            cv::convertScaleAbs(sharpened, img8uRot);
            cv::Mat gx, gy; cv::Sobel(rotated, gx, CV_16S, 1, 0, 3); cv::Sobel(rotated, gy, CV_16S, 0, 1, 3);
            cv::Mat grad; cv::subtract(gx, gy, grad); cv::Mat grad8u; cv::convertScaleAbs(grad, grad8u);
            cv::Mat blurred; cv::blur(grad8u, blurred, cv::Size(25, 25));
            cv::Mat otsu2; cv::threshold(blurred, otsu2, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
            cv::Mat dil; cv::dilate(otsu2, dil, cv::Mat(), cv::Point(-1, -1), 4);
            cv::Mat closed; cv::Mat kernel = cv::getStructuringElement(cv::MORPH_CROSS, cv::Size(morphCross, morphCross));
            cv::morphologyEx(dil, closed, cv::MORPH_CLOSE, kernel);
            std::vector<std::vector<cv::Point>> contours2; cv::findContours(closed, contours2, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            if (!contours2.empty()) {
                auto it2 = std::max_element(contours2.begin(), contours2.end(),
                    [](const std::vector<cv::Point> &a, const std::vector<cv::Point> &b) { return cv::contourArea(a) < cv::contourArea(b); });
                boundingRectAll = cv::boundingRect(*it2);
            }
        } else if (maxArea > 0) boundingRectAll = cv::boundingRect(*maxIt);

        if (boundingRectAll.width * boundingRectAll.height < minAreaThreshold) return cv::Mat();

        outX = boundingRectAll.x; outY = boundingRectAll.y; okFlag = true;
        return rotated(boundingRectAll).clone();
    }

}
