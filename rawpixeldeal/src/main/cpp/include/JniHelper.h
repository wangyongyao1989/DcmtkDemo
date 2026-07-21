#ifndef RAWPIXELDEAL_JNIHELPER_H
#define RAWPIXELDEAL_JNIHELPER_H

#include <jni.h>
#include <vector>
#include <opencv2/core.hpp>

namespace JniHelper {

    /**
     * @brief 从 rawBuffer 构造 cv::Mat（不拷贝）。
     */
    cv::Mat wrapRawMat(const uint8_t *data, int width, int height,
                       int bitsAllocated, int pixelSigned);

    /**
     * @brief 把 jbyteArray 拷贝到本地 std::vector<uint8_t>。
     */
    bool copyJByteArray(JNIEnv *env, jbyteArray src, std::vector<uint8_t> &dst);

    /**
     * @brief 把 CV_8UC1 灰度图转为 RGBA 字节数组并返回 jbyteArray。
     */
    bool gray8uToRgbaJBytes(JNIEnv *env, const cv::Mat &gray, jbyteArray &outRgba);

}

#endif //RAWPIXELDEAL_JNIHELPER_H
