#include "include/JniHelper.h"
#include <opencv2/imgproc.hpp>

namespace JniHelper {

    cv::Mat wrapRawMat(const uint8_t *data, int width, int height,
                       int bitsAllocated, int pixelSigned) {
        if (data == nullptr || width <= 0 || height <= 0) return cv::Mat();

        if (bitsAllocated == 16) {
            int type = (pixelSigned != 0) ? CV_16SC1 : CV_16UC1;
            return cv::Mat(height, width, type, const_cast<uint8_t *>(data));
        }
        if (bitsAllocated == 8) {
            return cv::Mat(height, width, CV_8UC1, const_cast<uint8_t *>(data));
        }
        return cv::Mat();
    }

    bool copyJByteArray(JNIEnv *env, jbyteArray src, std::vector<uint8_t> &dst) {
        if (src == nullptr) return false;
        jsize len = env->GetArrayLength(src);
        dst.resize(static_cast<size_t>(len));
        if (len > 0) {
            env->GetByteArrayRegion(src, 0, len, reinterpret_cast<jbyte *>(dst.data()));
        }
        return true;
    }

    bool gray8uToRgbaJBytes(JNIEnv *env, const cv::Mat &gray, jbyteArray &outRgba) {
        if (gray.empty() || gray.type() != CV_8UC1) return false;
        cv::Mat rgba;
        cv::cvtColor(gray, rgba, cv::COLOR_GRAY2RGBA);
        const size_t total = static_cast<size_t>(rgba.total()) * rgba.elemSize();
        outRgba = env->NewByteArray(static_cast<jsize>(total));
        if (outRgba == nullptr) return false;
        env->SetByteArrayRegion(outRgba, 0, static_cast<jsize>(total),
                                reinterpret_cast<const jbyte *>(rgba.data));
        return true;
    }

}
