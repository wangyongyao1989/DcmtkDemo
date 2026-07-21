#ifndef RAWPIXELDEAL_XRAYPROCESSOR_H
#define RAWPIXELDEAL_XRAYPROCESSOR_H

#include <jni.h>
#include <opencv2/core.hpp>

namespace XrayProcessor {

    /**
     * @brief 构造包含原始图像的 jbyteArray。
     */
    jbyteArray buildFullImageResult(JNIEnv *env, const cv::Mat &sv, jintArray outInfo);

    /**
     * @brief 执行 X-ray 图像裁剪核心逻辑。
     */
    cv::Mat tailor(const cv::Mat &sv, int minAreaThreshold, bool enableSobel,
                   int morphCross, double otsuThresholdLow,
                   int &outX, int &outY, double &outAngle, bool &okFlag);

}

#endif //RAWPIXELDEAL_XRAYPROCESSOR_H
