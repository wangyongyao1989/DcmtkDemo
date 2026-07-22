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
        jbyteArray outBytes = env->NewByteArray(
                static_cast<jsize>(out16.total() * out16.elemSize()));
        if (outBytes != nullptr) {
            env->SetByteArrayRegion(outBytes, 0,
                                    static_cast<jsize>(out16.total() * out16.elemSize()),
                                    reinterpret_cast<const jbyte *>(out16.data));
        }
        if (outInfo != nullptr && env->GetArrayLength(outInfo) >= 6) {
            jint info[6] = {0, 0, sv.cols, sv.rows, 0, 0};
            env->SetIntArrayRegion(outInfo, 0, 6, info);
        }
        return outBytes;
    }

    /**
     * 对输入的放射图像进行裁剪和校正处理
     * @param sv 输入的原始图像 (cv::Mat)
     * @param minAreaThreshold 最小面积阈值，过滤过小的区域
     * @param enableSobel 是否启用 Sobel 边缘检测和形态学优化逻辑
     * @param morphCross 形态学操作中闭运算核的大小
     * @param otsuThresholdLow Otsu 阈值的下限（当前实现暂未直接使用该数值，采用自动阈值）
     * @param outX 输出参数：最终裁剪区域在原图（旋转后）的 X 坐标
     * @param outY 输出参数：最终裁剪区域在原图（旋转后）的 Y 坐标
     * @param outAngle 输出参数：图像旋转的角度
     * @param okFlag 输出参数：处理是否成功的标志
     * @return 裁剪校正后的图像
    */
    cv::Mat tailor(const cv::Mat &sv, int minAreaThreshold, bool enableSobel,
                   int morphCross, double otsuThresholdLow,
                   int &outX, int &outY, double &outAngle, bool &okFlag) {
        // 1. 初始化输出参数，确保在任何返回路径下都有确定的初值
        okFlag = false;          // 处理成功标志
        outX = 0;                // 裁剪区域原点 X
        outY = 0;                // 裁剪区域原点 Y
        outAngle = 0.0;          // 旋转角度
        int width = sv.cols;     // 输入图像宽度
        int height = sv.rows;    // 输入图像高度

        // 2. 图像深度转换（16位转8位）：放射图像通常为 16 位，需映射到 0-255 以便后续处理
        cv::Mat img8u;
        {
            cv::Mat tmp32f;
            // 缩放比例 1.0/256.0 将 16 位深度转为浮点 0-255，再转为 8 位字节类型
            sv.convertTo(tmp32f, CV_32F, 1.0 / 256.0, 0.0);
            cv::convertScaleAbs(tmp32f, img8u);
        }

        // 3. 初步二值化与轮廓分析，用于粗略定位和确定偏转角
        cv::Mat otsu;
        // 使用 Otsu 自适应阈值法进行二值化处理
        cv::threshold(img8u, otsu, 10, 255.0, cv::THRESH_OTSU);
        std::vector<std::vector<cv::Point>> contours;
        // 提取外部轮廓
        cv::findContours(otsu, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        // 如果未找到轮廓，则认为处理失败，返回空矩阵
        if (contours.empty()) return cv::Mat();

        // 寻找面积最大的轮廓，作为主照射野区域
        auto maxIt = std::max_element(contours.begin(), contours.end(),
                                      [](const std::vector<cv::Point> &a,
                                         const std::vector<cv::Point> &b) {
                                          return cv::contourArea(a) < cv::contourArea(b);
                                      });
        const double maxArea = cv::contourArea(*maxIt);

        // 4. 计算并校正旋转角度
        cv::RotatedRect rr;
        {
            std::vector<cv::Point2f> pts2f;
            // 将最大轮廓的点转为浮点坐标
            for (const auto &p: *maxIt) pts2f.emplace_back(p.x, p.y);
            // 计算最小面积外接矩形，从中获取倾斜角
            rr = cv::minAreaRect(pts2f);
        }
        double angle = rr.angle;
        // OpenCV 旋转角度逻辑修正：如果偏角很大，转换坐标轴方向
        if (std::abs(angle) > 45.0) angle += 90.0;
        // 忽略极小偏转
        if (std::abs(angle) < 1e-3) angle = 0.0;

        // 5. 应用仿射变换旋转图像
        cv::Mat rotated, img8uRot;
        if (std::abs(angle) > 0.5) {
            // 计算图像中心
            cv::Point2f center(static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f);
            // 生成旋转矩阵
            cv::Mat M = cv::getRotationMatrix2D(center, angle, 1.0);
            // 对原始 16 位图和处理用的 8 位图分别进行旋转变换
            cv::warpAffine(sv, rotated, M, cv::Size(width, height), cv::INTER_LINEAR,
                           cv::BORDER_CONSTANT, cv::Scalar(0));
            cv::warpAffine(img8u, img8uRot, M, cv::Size(width, height), cv::INTER_LINEAR,
                           cv::BORDER_CONSTANT, cv::Scalar(0));
            outAngle = angle;
        } else {
            // 角度较小时无需旋转，直接克隆数据
            rotated = sv.clone();
            img8uRot = img8u.clone();
        }

        // 6. 确定最终的裁剪矩形框
        cv::Rect boundingRectAll(0, 0, width, height);
        // 如果启用 Sobel 增强和形态学逻辑，进行更精细的边缘提取
        if (enableSobel && morphCross > 0) {
            // A. 通过锐化核增强图像边缘对比度
            cv::Mat sharpKernel = (cv::Mat_<float>(3, 3) << -1, -1, -1, -1, 9, -1, -1, -1, -1);
            cv::Mat sharpened;
            cv::filter2D(img8uRot, sharpened, CV_32F, sharpKernel, cv::Point(-1, -1), 0);
            cv::convertScaleAbs(sharpened, img8uRot);

            // B. 使用 Sobel 算子计算图像梯度梯度差异
            cv::Mat gx, gy;
            cv::Sobel(rotated, gx, CV_16S, 1, 0, 3);
            cv::Sobel(rotated, gy, CV_16S, 0, 1, 3);
            cv::Mat grad;
            cv::subtract(gx, gy, grad);

            // C. 梯度图后处理：转换格式、模糊降噪并二值化
            cv::Mat grad8u;
            cv::convertScaleAbs(grad, grad8u);
            cv::Mat blurred;
            cv::blur(grad8u, blurred, cv::Size(25, 25));
            cv::Mat otsu2;
            cv::threshold(blurred, otsu2, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

            // D. 形态学膨胀与闭运算：连接断开的边缘并填补空洞
            cv::Mat dil;
            cv::dilate(otsu2, dil, cv::Mat(), cv::Point(-1, -1), 4);
            cv::Mat closed;
            cv::Mat kernel = cv::getStructuringElement(cv::MORPH_CROSS,
                                                       cv::Size(morphCross, morphCross));
            cv::morphologyEx(dil, closed, cv::MORPH_CLOSE, kernel);

            // E. 在形态学处理后的结果中查找最终的最大裁剪区域
            std::vector<std::vector<cv::Point>> contours2;
            cv::findContours(closed, contours2, cv::RETR_EXTERNAL,
                             cv::CHAIN_APPROX_SIMPLE);
            if (!contours2.empty()) {
                auto it2 = std::max_element(contours2.begin(), contours2.end(),
                                            [](const std::vector<cv::Point> &a,
                                               const std::vector<cv::Point> &b) {
                                                return cv::contourArea(a) < cv::contourArea(b);
                                            });
                boundingRectAll = cv::boundingRect(*it2);
            }
        } else if (maxArea > 0) {
            // 如果未启用 Sobel，直接使用第一步计算出的最大轮廓外接矩形
            boundingRectAll = cv::boundingRect(*maxIt);
        }

        // 7. 过滤小面积区域并执行裁剪
        // 检查最终得到的裁剪区域是否超过最小面积阈值
        if (boundingRectAll.width * boundingRectAll.height < minAreaThreshold) return cv::Mat();

        outX = boundingRectAll.x; // 记录裁剪左上角 X
        outY = boundingRectAll.y; // 记录裁剪左上角 Y
        okFlag = true;            // 标记处理成功

        // 从旋转后的图中截取矩形区域并返回副本
        return rotated(boundingRectAll).clone();
    }
}
