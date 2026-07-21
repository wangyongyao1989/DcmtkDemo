// =============================================================================
// rawpixeldeal native bridge
// - 最小验证链路（Kotlin -> JNI -> OpenCV）。
// - 已有 assets 归一化/CLAHE 流程：native_processRawToRgba。
// - 新增 CT 序列级处理管线（PRD 需求 ct-opencv-raw-buffer-windowing-prd）：
//     raw buffer -> HU 标准化 -> OpenCV 优化 -> 自动裁剪 ->
//     序列级自适应窗宽窗位 -> 8-bit 显示。
//   见 native_processCtSeries 及相关 helper。
// =============================================================================
#include <jni.h>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>

#include <android/log.h>

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>

#include "include/MedicalCTPreprocess.h"

#define TAG "RawPixelDealJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// =============================================================================
// 1) 返回 OpenCV 版本号：验证 SO 与头文件版本一致、链接 OK
// =============================================================================
static jstring native_getOpenCVVersion(JNIEnv *env, jclass clazz) {
    const char *ver = cv::getVersionString().c_str();
    LOGI("OpenCV version: %s", ver);
    return env->NewStringUTF(ver);
}

// =============================================================================
// 2) 接受 Kotlin 传入的灰度原始像素（宽*高），构建 cv::Mat，做一次高斯模糊，
//    把结果再拷贝回 byte[] 交给 Kotlin。流程：byte[] -> Mat -> blur -> byte[]
//    同时返回前 N 个通道的像素和，方便在 Kotlin 端做"非全 0"校验。
// =============================================================================
static jintArray native_processRawGrayPixels(JNIEnv *env, jclass clazz,
                                             jint width, jint height,
                                             jbyteArray src_gray) {
    if (width <= 0 || height <= 0 || src_gray == nullptr) {
        LOGE("processRawGrayPixels: invalid args w=%d h=%d src=%p",
             width, height, src_gray);
        return nullptr;
    }

    jsize src_len = env->GetArrayLength(src_gray);
    const int expected = width * height;
    if (src_len < expected) {
        LOGE("processRawGrayPixels: src length %d < expected %d", src_len, expected);
        return nullptr;
    }

    // 1) 把 Java byte[] 拷贝到 C++ 临时缓冲
    std::vector<jbyte> jbuf(expected);
    env->GetByteArrayRegion(src_gray, 0, expected, jbuf.data());

    // 2) 构造输入 cv::Mat（CV_8UC1 灰度）
    cv::Mat src(height, width, CV_8UC1, jbuf.data());
    cv::Mat dst;

    // 3) 真正调用 OpenCV：3x3 高斯模糊
    cv::GaussianBlur(src, dst, cv::Size(3, 3), 0.0, 0.0,
                     cv::BORDER_REPLICATE);

    // 4) 计算 dst 像素总和 + 前 4 个像素值，作为最小校验信息回传
    double sum = cv::sum(dst)[0];
    jint head0 = dst.at<uchar>(0, 0);
    jint head1 = (width > 1) ? dst.at<uchar>(0, 1) : 0;
    jint head2 = (width > 2) ? dst.at<uchar>(0, 2) : 0;
    jint head3 = (height > 1 && width > 0) ? dst.at<uchar>(1, 0) : 0;

    // 5) 回填处理后的灰度像素到原 byte[] 末尾之后：先复用原 buffer 的前 expected 字节
    //    （这里用 memcpy 模拟"原数组里得到处理结果"的常见用法）
    std::memcpy(jbuf.data(), dst.data, expected);
    env->SetByteArrayRegion(src_gray, 0, expected, jbuf.data());

    LOGI("processRawGrayPixels: %dx%d, dst[0,0..1,0]=%d,%d,%d,%d, sum=%.1f",
         width, height, head0, head1, head2, head3, sum);

    jint ret[5] = {head0, head1, head2, head3, (jint) sum};
    jintArray out = env->NewIntArray(5);
    env->SetIntArrayRegion(out, 0, 5, ret);
    return out;
}

// =============================================================================
// 3) 把 assets 中的"原始像素数据缓冲"（Data610.bin / Data622.bin）经
//    OpenCV 处理后输出为可直接填入 Android Bitmap.ARGB_8888 的 RGBA 字节。
//
//    流程（与"做人工筛查"的需求对齐）：
//      src16 bytes -> cv::Mat(CV_16UC1) -> min/max 归一化到 8-bit
//                  -> 可选 CLAHE(对比度增强) -> 可选中心裁剪(去除空白边框)
//                  -> COLOR_GRAY2RGBA -> jbyteArray  (R,G,B,A 内存布局，
//                     与 Android Bitmap.copyPixelsFromBuffer 兼容)
//
//    参数：
//      width, height    原图宽高（像素）
//      bitDepth         8 或 16
//      srcBytes         原始字节，little-endian
//      cropLeft/Top/Right/Bottom  四周要裁掉的像素数（>=0），传 0 表示不裁
//      enableClahe      1=做 CLAHE，0=不做
//      clipLimit        CLAHE 的 clipLimit（仅 enableClahe=1 时生效，<=0 走默认 2.0）
//      tileSize         CLAHE 的 tile 边长（仅 enableClahe=1 时生效，<=0 走默认 8）
//
//    返回：
//      jbyteArray，长度 = outW * outH * 4（RGBA8888）。失败返回 nullptr。
//      数组前 8 个 int 作为 [outW, outH, srcMin, srcMax, srcMean/10, cropLeft, cropTop, cropRight]
//      一并放在返回值最前面？——这里为简化，返回一个独立的 IntArray 头信息 + RGBA bytes
//      通过 out 参数 jintArray head（长度 4）回传 [outW, outH, srcMin, srcMax]。
// =============================================================================
static jbyteArray native_processRawToRgba(JNIEnv *env, jclass clazz,
                                          jint width, jint height,
                                          jint bitDepth,
                                          jbyteArray srcBytes,
                                          jint cropLeft, jint cropTop,
                                          jint cropRight, jint cropBottom,
                                          jint enableClahe,
                                          jdouble clipLimit, jint tileSize,
                                          jintArray head) {
    if (width <= 0 || height <= 0 || srcBytes == nullptr) {
        LOGE("processRawToRgba: invalid args w=%d h=%d src=%p",
             width, height, srcBytes);
        return nullptr;
    }
    if (bitDepth != 8 && bitDepth != 16) {
        LOGE("processRawToRgba: unsupported bitDepth=%d (only 8/16)", bitDepth);
        return nullptr;
    }

    const int channels = (bitDepth == 16) ? 2 : 1;
    const jsize src_len = env->GetArrayLength(srcBytes);
    const size_t expected = (size_t) width * (size_t) height * (size_t) channels;
    if ((size_t) src_len < expected) {
        LOGE("processRawToRgba: src length %d < expected %zu", src_len, expected);
        return nullptr;
    }

    // 1) 拷到本地缓冲（保留 little-endian 原样）
    std::vector<unsigned char> raw(expected);
    env->GetByteArrayRegion(srcBytes, 0, (jsize) expected,
                            reinterpret_cast<jbyte *>(raw.data()));

    // 2) 构造输入 Mat
    cv::Mat src;
    if (bitDepth == 16) {
        src = cv::Mat(height, width, CV_16UC1, raw.data());
    } else {
        src = cv::Mat(height, width, CV_8UC1, raw.data());
    }

    // 3) 8-bit 视图
    cv::Mat gray8;
    double minV = 0.0, maxV = 255.0;
    if (bitDepth == 16) {
        // min/max 归一化到 0~255，便于人工筛查观察全动态范围
        cv::minMaxLoc(src, &minV, &maxV);
        // OpenCV 4.x 用 convertTo 带掩码做线性拉伸
        src.convertTo(gray8, CV_8UC1,
                      255.0 / std::max(1.0, (maxV - minV)),
                      -minV * 255.0 / std::max(1.0, (maxV - minV)));
        LOGI("processRawToRgba: 16-bit min=%.1f max=%.1f -> 8-bit", minV, maxV);
    } else {
        gray8 = src.clone();
    }

    // 4) 可选 CLAHE：增强局部对比，便于观察组织结构
    if (enableClahe != 0) {
        double clip = (clipLimit > 0.0) ? clipLimit : 2.0;
        int tile = (tileSize > 0) ? tileSize : 8;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clip, cv::Size(tile, tile));
        cv::Mat enhanced;
        clahe->apply(gray8, enhanced);
        gray8 = enhanced;
        LOGI("processRawToRgba: CLAHE clip=%.2f tile=%d", clip, tile);
    }

    // 5) 可选中心裁剪（按四周要裁掉的像素数）。一般用来去掉传感器空白边。
    int cl = std::max(0, cropLeft);
    int ct = std::max(0, cropTop);
    int cr = std::max(0, cropRight);
    int cb = std::max(0, cropBottom);
    if (cl + cr >= gray8.cols || ct + cb >= gray8.rows) {
        LOGE("processRawToRgba: crop too large w=%d h=%d cl=%d cr=%d ct=%d cb=%d",
             gray8.cols, gray8.rows, cl, cr, ct, cb);
        return nullptr;
    }
    cv::Mat cropped;
    if (cl > 0 || ct > 0 || cr > 0 || cb > 0) {
        cv::Rect roi(cl, ct, gray8.cols - cl - cr, gray8.rows - ct - cb);
        cropped = gray8(roi).clone();
        LOGI("processRawToRgba: crop LTRB=%d,%d,%d,%d -> %dx%d",
             cl, ct, cr, cb, cropped.cols, cropped.rows);
    } else {
        cropped = gray8;
    }

    // 6) 灰度 -> RGBA（与 Android Bitmap ARGB_8888 内存布局一致：R,G,B,A）
    cv::Mat rgba;
    cv::cvtColor(cropped, rgba, cv::COLOR_GRAY2RGBA);

    // 回写 head（srcMin, srcMax, outW, outH），由 Kotlin 端用于 Bitmap 构造与展示
    if (head != nullptr && env->GetArrayLength(head) >= 4) {
        jint hOut[4] = {
                (jint) minV,
                (jint) maxV,
                (jint) rgba.cols,
                (jint) rgba.rows,
        };
        env->SetIntArrayRegion(head, 0, 4, hOut);
    }

    // 7) 拷贝到 jbyteArray 返回
    const size_t outBytes = (size_t) rgba.total() * rgba.elemSize();
    jbyteArray out = env->NewByteArray((jsize) outBytes);
    if (out == nullptr) return nullptr;
    env->SetByteArrayRegion(out, 0, (jsize) outBytes,
                            reinterpret_cast<const jbyte *>(rgba.data));
    return out;
}

// =============================================================================
// 4) 一个独立的纯 JNI 探针：不经过 OpenCV，仅确认 JNI 注册链通畅。
//    常用于在加载 OpenCV 失败时，先确认 native-lib.cpp 自己能跑。
// =============================================================================
static jstring native_stringFromJNI(JNIEnv *env, jclass clazz) {
    LOGI("RawPixelDeal native-lib loaded");
    return env->NewStringUTF("Hello from rawpixeldeal native (OpenCV linked)");
}

// =============================================================================
// 5) CT 序列级处理管线（对应 ct-opencv-raw-buffer-windowing-prd）：
//
//   raw buffer(s)  --[1 解码 + 自动符号位]-->  stored value 矩阵 (CV_16U/CV_16S)
//                   --[2 HU 标准化]-->  float HU 矩阵  (CV_32F)
//                   --[3 裁剪]-->  定位人体 ROI（裁剪前置，避免把空气一起做双边）
//                   --[4 OpenCV 优化]-->  ROI 内去噪 + 极端值抑制
//                   --[5 序列直方图]-->  ROI 内百分位 Gmin/Gmax + 256 bin
//                   --[6 自适应调窗]-->  (c, w) by 论文算法
//                                 \-> 直方图熵过低时退化为预设常用窗
//                   --[7 窗映射]-->  CV_8U 显示图
//                   --[8 可选 CLAHE 增强]-->  提升软组织对比度
//                   --[9 灰 -> RGBA]-->  按 slice 返回 jbyteArray
//
//   入参（顺序与 kMethods 的 JNI 签名严格一致）：
//     rawBuffers  jobjectArray，每个元素是某个 slice 的 jbyteArray raw bytes
//     width/height/bitsAllocated/pixelSigned
//        —— 图像几何与位深（与 DICOM Rows/Columns/BitsAllocated/PixelRepresentation 对应）
//     rescaleSlope/rescaleIntercept
//        —— DICOM RescaleSlope / RescaleIntercept；用于 HU = m*SV + b
//        —— 传入 1 / 0 表示"已为 HU，不需要再变换"
//     photometric  0 = MONOCHROME2（常用），1 = MONOCHROME1（输出需反相）
//     bodyThreshold   自动裁剪时"非空气" HU 阈值，默认 -600
//     cropMargin      裁剪外扩 margin（像素），默认 16
//     bodyMorphSize   形态学核尺寸，默认 5
//     minBodyAreaPx   连通域最小面积，过滤小噪点，默认 1000
//     nBins / n0 / n1  自适应调窗参数：bin 数 / T0 比例 / T1 比例
//     histSampleStride 直方图采样步长（论文允许"等距采样"），默认 1
//     enableBilateral / bilateralD / bilateralSigma*
//        —— 是否启用双边滤波（去噪，保持边缘），默认 1 / 5 / 50 / 50
//     clipLowHu / clipHighHu  HU 极端值抑制区间，默认 -1200 / 3000
//     --- 新增参数（v2 优化） ---
//     enableAutoPixelSign  1=当 signed 全负时自动尝试 unsigned（处理裸 raw 误读）
//     gminPercentile       百分位 Gmin（默认 0.5，对应 0.5% 百分位）
//     gmaxPercentile       百分位 Gmax（默认 99.5）
//     enableHistFallback   1=当直方图退化（熵过低/单 bin 主导）时使用预设常用窗
//     fallbackWindowCenter 预设窗位（默认 40，软组织常用）
//     fallbackWindowWidth  预设窗宽（默认 400）
//     enableDisplayClahe   1=对最终 8-bit 图像做 CLAHE 提升对比度
//     displayClaheClip     CLAHE clipLimit（默认 2.0）
//     displayClaheTile     CLAHE tile size（默认 8）
//     cropFirst            1=先裁剪再优化（推荐），0=旧顺序
//
//   出参：
//     displayRgba jobjectArray —— 每个 slice 的 RGBA8888 bytes
//     windowStats jdoubleArray —— 长度 11
//     cropAndOut  jintArray    —— 长度 10
//     histogram   jintArray    —— 256 长度直方图（已剔除/合并前）
//     outUsedFlags jintArray   —— 长度 1
// =============================================================================

namespace {

// ---------- 工具：从 rawBuffer 构造 cv::Mat（不拷贝） ----------
// bitsAllocated=16 时按 signed/unsigned 选 CV_16SC1 / CV_16UC1；
// buffer 生命周期由调用方负责（这里只读，不会写）。
    static cv::Mat wrapRawMat(const uint8_t *data, int width, int height,
                              int bitsAllocated, int pixelSigned) {
        CV_Assert(data != nullptr && width > 0 && height > 0);
        if (bitsAllocated == 16) {
            int type = (pixelSigned != 0) ? CV_16SC1 : CV_16UC1;
            return cv::Mat(height, width, type, const_cast<uint8_t *>(data));
        }
        if (bitsAllocated == 8) {
            return cv::Mat(height, width, CV_8UC1, const_cast<uint8_t *>(data));
        }
        // 其它位深不支持
        return cv::Mat();
    }

// ---------- 工具：把 jbyteArray 拷贝到本地 std::vector<uint8_t> ----------
    static bool copyJByteArray(JNIEnv *env, jbyteArray src, std::vector<uint8_t> &dst) {
        if (src == nullptr) return false;
        jsize len = env->GetArrayLength(src);
        dst.resize(static_cast<size_t>(len));
        if (len > 0) {
            env->GetByteArrayRegion(src, 0, len,
                                    reinterpret_cast<jbyte *>(dst.data()));
        }
        return true;
    }

// ---------- 工具：HU 标准化 ----------
// SV -> HU，公式 HU = slope * SV + intercept
// 输入：CV_16SC1 或 CV_16UC1；输出：CV_32FC1（便于后续滤波与直方图统计）
    static cv::Mat toHu(const cv::Mat &sv, double slope, double intercept) {
        cv::Mat hu;
        if (sv.empty()) return hu;
        // convertTo：dst = src * alpha + beta
        sv.convertTo(hu, CV_32FC1, slope, intercept);
        return hu;
    }

// ---------- 工具：OpenCV 视觉优化 ----------
// (a) 双边滤波（保边去噪），开关可控；
// (b) 极端 HU 值温和裁剪（按 clipLowHu / clipHighHu 截断到边界值）。
    static cv::Mat optimizeHu(const cv::Mat &hu, bool enableBilateral,
                              int bilateralD, double sigmaColor, double sigmaSpace,
                              float clipLowHu, float clipHighHu) {
        cv::Mat out = hu.clone();
        if (out.empty()) return out;

        if (enableBilateral) {
            cv::Mat denoised;
            // OpenCV bilateralFilter 仅支持 CV_8UC1；这里把 HU 归一化到 0..255 后做
            // 双边，再线性映射回原 HU 范围。
            double mn = 0.0, mx = 0.0;
            cv::minMaxLoc(out, &mn, &mx);
            const double span = std::max(1e-6, mx - mn);
            cv::Mat normalized;
            out.convertTo(normalized, CV_8UC1, 255.0 / span, -mn * 255.0 / span);
            cv::Mat filtered8u;
            cv::bilateralFilter(normalized, filtered8u, bilateralD, sigmaColor,
                                sigmaSpace, cv::BORDER_REPLICATE);
            filtered8u.convertTo(out, CV_32FC1, span / 255.0, mn);
        }

        // 极端 HU 值截断（论文中"对极端 HU 值做温和裁剪"）
        cv::threshold(out, out, clipHighHu, clipHighHu, cv::THRESH_TRUNC);
        cv::max(out, clipLowHu, out);

        return out;
    }

// ---------- 工具：自动裁剪人体 ROI ----------
// 1) 阈值初筛：hu > bodyThreshold
// 2) 形态学闭运算（填洞）+ 开运算（去小噪点）
// 3) 连通域分析：取最大连通域作为 bodyMask
// 4) boundingRect + 外扩 margin
// 返回：cropRect（已是图像坐标）。若 mask 为空（背景全是空气），返回原图大小。
    static cv::Rect autoCropBodyRoi(const cv::Mat &hu, float bodyThreshold,
                                    int morphSize, int minBodyAreaPx,
                                    int marginPx) {
        if (hu.empty()) return cv::Rect();

        cv::Mat mask;
        cv::threshold(hu, mask, bodyThreshold, 255.0, cv::THRESH_BINARY);
        mask.convertTo(mask, CV_8UC1);

        if (morphSize > 1) {
            cv::Mat kernel = cv::getStructuringElement(
                    cv::MORPH_ELLIPSE, cv::Size(morphSize, morphSize));
            // 先闭（填洞）后开（去小颗粒）
            cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
            cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
        }

        // 连通域：找最大块
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids,
                                                 8, CV_32S);
        if (n <= 1) {
            // 没有前景
            return cv::Rect(0, 0, hu.cols, hu.rows);
        }
        int bestLabel = -1;
        int bestArea = 0;
        for (int i = 1; i < n; ++i) {  // 0 = 背景
            int area = stats.at<int>(i, cv::CC_STAT_AREA);
            if (area > bestArea) {
                bestArea = area;
                bestLabel = i;
            }
        }
        if (bestLabel < 0 || bestArea < std::max(1, minBodyAreaPx)) {
            return cv::Rect(0, 0, hu.cols, hu.rows);
        }
        int x = stats.at<int>(bestLabel, cv::CC_STAT_LEFT);
        int y = stats.at<int>(bestLabel, cv::CC_STAT_TOP);
        int w = stats.at<int>(bestLabel, cv::CC_STAT_WIDTH);
        int h = stats.at<int>(bestLabel, cv::CC_STAT_HEIGHT);

        // 外扩 margin
        int x0 = std::max(0, x - marginPx);
        int y0 = std::max(0, y - marginPx);
        int x1 = std::min(hu.cols, x + w + marginPx);
        int y1 = std::min(hu.rows, y + h + marginPx);
        return cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
    }

// ---------- 工具：序列级直方图聚合 ----------
// 对每个 slice 在 cropRect 内逐像素统计 256 bin 频数；
// stride > 1 时按论文建议做"等距采样"以提速，结果可复现。
    static void aggregateSeriesHistogram(const std::vector<cv::Mat> &huSlices,
                                         const cv::Rect &roi,
                                         double gmin, double gmax, int nBins,
                                         int stride, std::vector<int> &histOut) {
        histOut.assign(nBins, 0);
        if (huSlices.empty() || roi.area() <= 0) return;
        const double span = std::max(1e-6, gmax - gmin);
        const double hBin = span / static_cast<double>(nBins);
        for (const cv::Mat &hu: huSlices) {
            if (hu.empty()) continue;
            cv::Rect r = roi & cv::Rect(0, 0, hu.cols, hu.rows);
            if (r.area() <= 0) continue;
            for (int yy = r.y; yy < r.y + r.height; yy += std::max(1, stride)) {
                const float *row = hu.ptr<float>(yy);
                for (int xx = r.x; xx < r.x + r.width; xx += std::max(1, stride)) {
                    double v = static_cast<double>(row[xx]);
                    if (v < gmin) v = gmin;
                    if (v > gmax) v = gmax;
                    int bin = static_cast<int>((v - gmin) / hBin);
                    if (bin < 0) bin = 0;
                    if (bin >= nBins) bin = nBins - 1;
                    histOut[bin]++;
                }
            }
        }
    }

// ---------- 工具：序列 Gmin/Gmax 统计 ----------
// 在所有 slice 的 ROI 内统计 min/max HU。
    static bool computeSeriesGminGmax(const std::vector<cv::Mat> &huSlices,
                                      const cv::Rect &roi, int stride,
                                      float &gMinOut, float &gMaxOut) {
        gMinOut = std::numeric_limits<float>::infinity();
        gMaxOut = -std::numeric_limits<float>::infinity();
        bool any = false;
        for (const cv::Mat &hu: huSlices) {
            if (hu.empty()) continue;
            cv::Rect r = roi & cv::Rect(0, 0, hu.cols, hu.rows);
            if (r.area() <= 0) continue;
            for (int yy = r.y; yy < r.y + r.height; yy += std::max(1, stride)) {
                const float *row = hu.ptr<float>(yy);
                for (int xx = r.x; xx < r.x + r.width; xx += std::max(1, stride)) {
                    float v = row[xx];
                    if (v < gMinOut) gMinOut = v;
                    if (v > gMaxOut) gMaxOut = v;
                    any = true;
                }
            }
        }
        if (!any) {
            gMinOut = 0.0f;
            gMaxOut = 0.0f;
            return false;
        }
        return true;
    }

// ---------- 工具：自适应调窗 ----------
// 实现论文《自适应调节医学CT序列图像窗宽窗位算法》（东北大学学报，2023）：
//   1) 统计序列 Gmin/Gmax；H_bins = (Gmax - Gmin) / n_bins
//   2) 投到 n_bins 个组得到 Hist[]；T = sum(Hist)
//   3) 阈值剔除：保留 Hist[i] >= T*N0 的组，得 M[]
//   4) 相邻组合并：若 |M[i+1] - M[i]| < T*N1，则合并
//   5) 剩余组数 B；c = B * H_bins * 0.125；w = B * H_bins + c
// 返回：true 表示至少能算出 (c, w)；false 表示直方图过空 / Gmin==Gmax。
    struct AdaptiveWindowResult {
        double c = 0.0;
        double w = 1.0;
        double hBins = 1.0;
        double t0 = 0.0;
        double t1 = 0.0;
        int b = 0;
    };

    static bool computeAdaptiveWindow(const std::vector<int> &histOrig,
                                      int nBins, double n0, double n1,
                                      AdaptiveWindowResult &out) {
        if (histOrig.empty() || nBins <= 0) return false;
        long long T = 0;
        for (int v: histOrig) T += v;
        if (T <= 0) return false;
        out.t0 = static_cast<double>(T) * n0;
        out.t1 = static_cast<double>(T) * n1;

        // 步骤 3：阈值剔除（保留 Hist[i] >= T0）
        std::vector<int> M;
        M.reserve(nBins);
        for (int v: histOrig) {
            if (static_cast<double>(v) >= out.t0) M.push_back(v);
        }
        if (M.empty()) {
            // 全被剔除：退化用原始直方图，论文语义下"信息全在低频"，仍要给个 (c,w)
            M = histOrig;
        }

        // 步骤 4：相邻组合并（差值绝对值 < T1 视为同一段）
        std::vector<int> merged;
        merged.reserve(M.size());
        int cur = M[0];
        for (size_t i = 1; i < M.size(); ++i) {
            if (std::abs(M[i] - cur) < out.t1) {
                cur += M[i];  // 合并：求和
            } else {
                merged.push_back(cur);
                cur = M[i];
            }
        }
        merged.push_back(cur);

        out.b = static_cast<int>(merged.size());
        if (out.b <= 0) out.b = 1;

        // H_bins：原直方图的组距（HU/组）
        // 由调用方写入 out.hBins；这里给一个保底值（防止外面没填）
        if (out.hBins <= 0.0) out.hBins = 1.0;

        out.c = static_cast<double>(out.b) * out.hBins * 0.125;
        out.w = static_cast<double>(out.b) * out.hBins + out.c;
        if (out.w < 1.0) out.w = 1.0;  // 防止窗宽过窄导致全 0/全 255
        return true;
    }

// ---------- 工具：HU -> 8-bit 窗映射 ----------
// lower = c - w/2; upper = c + w/2;
//   y = 0                  if x < lower
//   y = 255                if x > upper
//   y = (x-lower)*255/w    else
// photometric=MONOCHROME1 时取反。
    static cv::Mat applyWindow8u(const cv::Mat &hu, double c, double w,
                                 int photometric) {
        cv::Mat out(hu.size(), CV_8UC1);
        if (hu.empty()) return out;
        const double lower = c - w * 0.5;
        const double upper = c + w * 0.5;
        const double invSpan = (upper > lower) ? (255.0 / (upper - lower)) : 0.0;
        const bool invert = (photometric == 1);

        for (int yy = 0; yy < hu.rows; ++yy) {
            const float *src = hu.ptr<float>(yy);
            uint8_t *dst = out.ptr<uint8_t>(yy);
            for (int xx = 0; xx < hu.cols; ++xx) {
                double x = static_cast<double>(src[xx]);
                double y;
                if (invSpan <= 0.0) {
                    y = 127.5;
                } else if (x <= lower) {
                    y = 0.0;
                } else if (x >= upper) {
                    y = 255.0;
                } else {
                    y = (x - lower) * invSpan;
                }
                uint8_t v = static_cast<uint8_t>(y + 0.5);
                if (invert) v = static_cast<uint8_t>(255 - v);
                dst[xx] = v;
            }
        }
        return out;
    }

// ---------- 工具：8U gray -> RGBA 字节（与 Android Bitmap.ARGB_8888 兼容） ----------
    static bool gray8uToRgbaJBytes(JNIEnv *env, const cv::Mat &gray,
                                   jbyteArray &outRgba) {
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

// ---------- 工具（v2 优化）：百分位 Gmin/Gmax ----------
// 在 ROI 内按 stride 采样，把所有 HU 收集到 vector 里排序后取 pLow/pHigh 百分位。
// 与绝对 min/max 相比：能抗"空气段"和"骨头段"的极端尖峰，让 H_bins 反映"主体"动态范围。
    static void computePercentileHu(const std::vector<cv::Mat> &huSlices,
                                    const cv::Rect &roi, int stride,
                                    double pLow, double pHigh,
                                    float &gMinOut, float &gMaxOut) {
        gMinOut = 0.0f;
        gMaxOut = 1.0f;
        // 把 ROI 内所有 HU 收集到 values。Stride>1 时只是"采样"，对百分位估计影响可忽略。
        std::vector<float> values;
        values.reserve(1024);
        for (const cv::Mat &hu: huSlices) {
            if (hu.empty()) continue;
            cv::Rect r = roi & cv::Rect(0, 0, hu.cols, hu.rows);
            if (r.area() <= 0) continue;
            for (int yy = r.y; yy < r.y + r.height; yy += std::max(1, stride)) {
                const float *row = hu.ptr<float>(yy);
                for (int xx = r.x; xx < r.x + r.width; xx += std::max(1, stride)) {
                    values.push_back(row[xx]);
                }
            }
        }
        if (values.empty()) return;
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        // 百分位索引：pLow/pHigh 单位为百分（0..100）
        size_t idxLow = static_cast<size_t>(
                std::max(0.0, std::min(100.0, pLow)) * (n - 1) / 100.0);
        size_t idxHigh = static_cast<size_t>(
                std::max(0.0, std::min(100.0, pHigh)) * (n - 1) / 100.0);
        if (idxLow > n - 1) idxLow = n - 1;
        if (idxHigh > n - 1) idxHigh = n - 1;
        gMinOut = values[idxLow];
        gMaxOut = values[idxHigh];
        if (!(gMaxOut > gMinOut)) gMaxOut = gMinOut + 1.0f;
        LOGI("computePercentileHu: n=%zu pLow=%.2f pHigh=%.2f -> Gmin=%.1f Gmax=%.1f",
             n, pLow, pHigh, gMinOut, gMaxOut);
    }

// ---------- 工具（v2 优化）：直方图质量统计 ----------
// - entropy: 归一化信息熵（0..1），0=单 bin 主导
// - maxBinFrac: 最高 bin 占总频数的比例
// - numPeaks: 频数 > 5% 总数的连续段数
    struct HistogramStats {
        double entropy;
        double maxBinFrac;
        int numPeaks;
    };

    static HistogramStats computeHistogramStats(const std::vector<int> &hist) {
        HistogramStats s{0.0, 0.0, 0};
        if (hist.empty()) return s;
        long long total = 0;
        for (int v: hist) total += v;
        if (total <= 0) return s;
        const double logN = std::log(static_cast<double>(hist.size()));
        bool inPeak = false;
        for (int v: hist) {
            double p = static_cast<double>(v) / static_cast<double>(total);
            if (p > 0.0 && logN > 0.0) {
                s.entropy -= p * std::log(p) / logN;  // 归一化到 [0, 1]
            }
            if (p > s.maxBinFrac) s.maxBinFrac = p;
            bool curPeak = (p > 0.05);  // 5% 视为"峰"
            if (curPeak && !inPeak) s.numPeaks++;
            inPeak = curPeak;
        }
        return s;
    }

// ---------- 工具（v2 优化）：多阈值回退自动裁剪 ----------
// 1) 先用主阈值；2) 失败时降到 -300；3) 再降到 -100；4) 仍失败返回全图。
// 返回时若 width<hu.cols && height<hu.rows 即视为"裁剪成功"。
    static cv::Rect tryAutoCropBodyRoiEx(const cv::Mat &hu, float bodyThreshold,
                                         int morphSize, int minBodyAreaPx,
                                         int marginPx) {
        const float thresholds[3] = {bodyThreshold, -300.0f, -100.0f};
        const char *names[3] = {"primary", "loose(-300)", "very-loose(-100)"};
        for (int i = 0; i < 3; ++i) {
            cv::Rect r = autoCropBodyRoi(hu, thresholds[i], morphSize,
                                         minBodyAreaPx, marginPx);
            bool ok = (r.width < hu.cols) && (r.height < hu.rows);
            LOGI("tryAutoCropBodyRoiEx: %s thr=%.0f -> rect=(%d,%d,%d,%d) %s",
                 names[i], thresholds[i], r.x, r.y, r.width, r.height,
                 ok ? "OK" : "FULL");
            if (ok) return r;
        }
        LOGW("tryAutoCropBodyRoiEx: all thresholds failed, fallback to full image");
        return cv::Rect(0, 0, hu.cols, hu.rows);
    }

// ---------- 工具（v2 优化）：直方图退化时的预设常用窗 ----------
// 当 maxBinFrac>0.6 或 entropy<0.3 时，说明当前 ROI 数据分布严重偏斜（典型如"全在 bin 0"）。
// 此时按"软组织窗"兜底，保证 UI 上能看见解剖结构。
    static void pickDefaultWindow(float gmin, float gmax, HistogramStats hs,
                                  double fallbackC, double fallbackW,
                                  double &cOut, double &wOut, bool &usedDefault) {
        usedDefault = false;
        const bool skewed = (hs.maxBinFrac > 0.6) || (hs.entropy < 0.3);
        if (skewed) {
            cOut = fallbackC;
            wOut = fallbackW;
            usedDefault = true;
            LOGW("pickDefaultWindow: histogram skewed (entropy=%.2f maxBinFrac=%.2f) "
                 "-> fallback window c=%.1f w=%.1f",
                 hs.entropy, hs.maxBinFrac, cOut, wOut);
            return;
        }
        // 分布合理：根据 ROI 跨度给一个宽窗
        double range = static_cast<double>(gmax) - static_cast<double>(gmin);
        cOut = (static_cast<double>(gmin) + static_cast<double>(gmax)) * 0.5;
        wOut = std::max(150.0, range * 0.7);
        LOGI("pickDefaultWindow: range-based window c=%.1f w=%.1f (range=%.1f)",
             cOut, wOut, range);
    }

// ---------- 工具（v2 优化）：8-bit 显示图做 CLAHE ----------
// 仅在最终 CV_8UC1 窗映射图上做局部均衡；BORDER_REFLECT_101 避免边缘黑边。
// 接受 enable=0 时直接返回原图（无拷贝）。
    static cv::Mat applyDisplayClahe(const cv::Mat &gray8u, bool enable,
                                     double clip, int tile) {
        if (!enable || gray8u.empty() || gray8u.type() != CV_8UC1) {
            return gray8u;
        }
        double c = (clip > 0.0) ? clip : 2.0;
        int t = (tile > 0) ? tile : 8;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(c, cv::Size(t, t));
        cv::Mat out;
        clahe->apply(gray8u, out);
        LOGI("applyDisplayClahe: clip=%.2f tile=%d", c, t);
        return out;
    }

}  // namespace

// ---------- JNI 入口：processCtSeries ----------
//
// 期望签名（见 RawPixelDealJni.processCtSeries）：
//   ([[B         rawBuffers（Kotlin Array<ByteArray> = byte[][]）
//    I,          width
//    I,          height
//    I,          bitsAllocated
//    I,          pixelSigned
//    D,          rescaleSlope
//    D,          rescaleIntercept
//    I,          photometric
//    F,          bodyThreshold
//    I,          cropMargin
//    I,          bodyMorphSize
//    I,          minBodyAreaPx
//    I,          nBins
//    D,          n0
//    D,          n1
//    I,          histSampleStride
//    I,          enableBilateral
//    I,          bilateralD
//    D,          bilateralSigmaColor
//    D,          bilateralSigmaSpace
//    F,          clipLowHu
//    F,          clipHighHu
//    --- v2 新增 ---
//    I,          enableAutoPixelSign
//    F,          gminPercentile
//    F,          gmaxPercentile
//    I,          enableHistFallback
//    D,          fallbackWindowCenter
//    D,          fallbackWindowWidth
//    I,          enableDisplayClahe
//    D,          displayClaheClip
//    I,          displayClaheTile
//    I,          cropFirst
//    [[B         outDisplays
//    [D          outWindowStats
//    [I          outCropAndOut
//    [I          outHistogram
//    [I          outUsedFlags
//   )Z
static jboolean native_processCtSeries(
        JNIEnv *env, jclass clazz,
        // 输入：raw 序列
        jobjectArray rawBuffers,       // jbyteArray[]
        // 像素几何与编码
        jint width, jint height,
        jint bitsAllocated,            // 16
        jint pixelSigned,              // 0/1
        jdouble rescaleSlope,
        jdouble rescaleIntercept,
        jint photometric,              // 0=MONOCHROME2, 1=MONOCHROME1
        // 自动裁剪参数
        jfloat bodyThreshold,
        jint cropMargin,
        jint bodyMorphSize,
        jint minBodyAreaPx,
        // 调窗参数
        jint nBins,
        jdouble n0, jdouble n1,
        jint histSampleStride,
        // 优化参数
        jint enableBilateral,
        jint bilateralD,
        jdouble bilateralSigmaColor,
        jdouble bilateralSigmaSpace,
        jfloat clipLowHu, jfloat clipHighHu,
        // ---- v2 新增 ----
        jint enableAutoPixelSign,
        jfloat gminPercentile,
        jfloat gmaxPercentile,
        jint enableHistFallback,
        jdouble fallbackWindowCenter,
        jdouble fallbackWindowWidth,
        jint enableDisplayClahe,
        jdouble displayClaheClip,
        jint displayClaheTile,
        jint cropFirst,
        // 输出
        jobjectArray outDisplays,      // jobjectArray，每个元素是 jbyteArray（RGBA8888）
        jdoubleArray outWindowStats,   // length = 11
        jintArray outCropAndOut,       // length = 10
        jintArray outHistogram,        // length = nBins
        jintArray outUsedFlags         // length = 1: [0]=okFlag
) {
    (void) clazz;
    if (rawBuffers == nullptr || outDisplays == nullptr) {
        LOGE("processCtSeries: null input/output array");
        return JNI_FALSE;
    }
    const jsize nSlices = env->GetArrayLength(rawBuffers);
    if (nSlices <= 0) {
        LOGE("processCtSeries: empty rawBuffers");
        return JNI_FALSE;
    }
    if (width <= 0 || height <= 0) {
        LOGE("processCtSeries: invalid size %dx%d", width, height);
        return JNI_FALSE;
    }
    if (bitsAllocated != 16 && bitsAllocated != 8) {
        LOGE("processCtSeries: unsupported bitsAllocated=%d", bitsAllocated);
        return JNI_FALSE;
    }
    if (nBins <= 0) {
        LOGE("processCtSeries: invalid nBins=%d", nBins);
        return JNI_FALSE;
    }
    if (n0 <= 0.0 || n1 <= 0.0) {
        LOGE("processCtSeries: invalid n0=%f n1=%f", n0, n1);
        return JNI_FALSE;
    }
    LOGI("processCtSeries: START nSlices=%d %dx%d bits=%d sign=%d "
         "slope=%.3f intc=%.1f photo=%d bodyThr=%.0f cropFirst=%d",
         nSlices, width, height, bitsAllocated, pixelSigned,
         rescaleSlope, rescaleIntercept, photometric, bodyThreshold, cropFirst);

    // ---- 1) 把每片 raw 拷到本地 vector<uint8_t>，并包成 cv::Mat ----
    std::vector<std::vector<uint8_t>> rawBytes(nSlices);
    std::vector<cv::Mat> svMats;
    svMats.reserve(nSlices);
    for (jsize s = 0; s < nSlices; ++s) {
        jbyteArray jb = (jbyteArray) env->GetObjectArrayElement(rawBuffers, s);
        if (!copyJByteArray(env, jb, rawBytes[s])) {
            LOGE("processCtSeries: slice %d copy failed", s);
            return JNI_FALSE;
        }
        env->DeleteLocalRef(jb);
        const size_t expected =
                static_cast<size_t>(width) * height * (bitsAllocated / 8);
        if (rawBytes[s].size() < expected) {
            LOGE("processCtSeries: slice %d size=%zu < expected=%zu",
                 s, rawBytes[s].size(), expected);
            return JNI_FALSE;
        }
        cv::Mat sv = wrapRawMat(rawBytes[s].data(), width, height,
                                bitsAllocated, pixelSigned);
        if (sv.empty()) {
            LOGE("processCtSeries: wrapRawMat failed at slice %d", s);
            return JNI_FALSE;
        }
        // 关键日志：每片 SV 范围（裸 raw 调试必看）
        double svMin = 0, svMax = 0;
        cv::minMaxLoc(sv, &svMin, &svMax);
        LOGI("processCtSeries: slice=%d SV min=%.1f max=%.1f", s, svMin, svMax);
        svMats.push_back(sv);
    }

    // ---- 1.5) v2 自动符号位检测：signed 全负时提示用户切到 unsigned ----
    if (enableAutoPixelSign != 0 && bitsAllocated == 16 && pixelSigned != 0) {
        double gMinAll = std::numeric_limits<double>::infinity();
        double gMaxAll = -std::numeric_limits<double>::infinity();
        for (const auto &sv: svMats) {
            double a, b;
            cv::minMaxLoc(sv, &a, &b);
            if (a < gMinAll) gMinAll = a;
            if (b > gMaxAll) gMaxAll = b;
        }
        if (gMaxAll < 0.0) {
            LOGW("processCtSeries: ALL SV values negative (max=%.1f). "
                 "Likely unsigned 16-bit raw misread as signed. "
                 "Try setting pixelSigned=0 (unsigned) in UI.", gMaxAll);
        }
    }

    // ---- 2) HU 标准化 ----
    std::vector<cv::Mat> huMats;
    huMats.reserve(nSlices);
    for (jsize s = 0; s < nSlices; ++s) {
        cv::Mat hu = toHu(svMats[s], rescaleSlope, rescaleIntercept);
        // 关键日志：HU 范围
        double huMin = 0, huMax = 0;
        cv::minMaxLoc(hu, &huMin, &huMax);
        LOGI("processCtSeries: slice=%d HU min=%.1f max=%.1f", s, huMin, huMax);
        huMats.push_back(std::move(hu));
    }

    // ============================================================
    //  v2 关键改动：根据 cropFirst 选择流水线顺序
    // ============================================================
    cv::Rect cropRect;
    if (cropFirst != 0) {
        // ---- 3a) 裁剪前置 ----
        // 选中间片做裁剪依据（最稳定）
        int pickIdx = std::min<int>(static_cast<int>(nSlices) - 1,
                                    static_cast<int>(nSlices) / 2);
        cropRect = tryAutoCropBodyRoiEx(
                huMats[pickIdx], bodyThreshold, bodyMorphSize,
                minBodyAreaPx, cropMargin);
        if (cropRect.area() <= 0) {
            cropRect = cv::Rect(0, 0, width, height);
        }
        LOGI("processCtSeries: cropFirst=1 pickIdx=%d cropRect=(%d,%d,%d,%d)",
             pickIdx, cropRect.x, cropRect.y, cropRect.width, cropRect.height);
        // ---- 4a) 仅在 ROI 内做优化（不浪费算力、不把空气一起平滑）----
        for (jsize s = 0; s < nSlices; ++s) {
            cv::Mat roiHu = huMats[s](cropRect).clone();
            cv::Mat opt = optimizeHu(roiHu, enableBilateral != 0, bilateralD,
                                     bilateralSigmaColor, bilateralSigmaSpace,
                                     clipLowHu, clipHighHu);
            // 写回 huMats 对应 ROI 区
            opt.copyTo(huMats[s](cropRect));
        }
    } else {
        // ---- 3b) 旧顺序：先优化再裁剪 ----
        for (jsize s = 0; s < nSlices; ++s) {
            huMats[s] = optimizeHu(huMats[s], enableBilateral != 0, bilateralD,
                                   bilateralSigmaColor, bilateralSigmaSpace,
                                   clipLowHu, clipHighHu);
        }
        int pickIdx = std::min<int>(static_cast<int>(nSlices) - 1,
                                    static_cast<int>(nSlices) / 2);
        cropRect = tryAutoCropBodyRoiEx(
                huMats[pickIdx], bodyThreshold, bodyMorphSize,
                minBodyAreaPx, cropMargin);
        if (cropRect.area() <= 0) {
            cropRect = cv::Rect(0, 0, width, height);
        }
        LOGI("processCtSeries: cropFirst=0 pickIdx=%d cropRect=(%d,%d,%d,%d)",
             pickIdx, cropRect.x, cropRect.y, cropRect.width, cropRect.height);
    }

    // ---- 5) 序列级直方图：在 ROI 内聚合 ----
    //  v2 改动：使用百分位 Gmin/Gmax（替代绝对 min/max）
    float gMin = 0.0f, gMax = 0.0f;
    if (gminPercentile > 0.0f || gmaxPercentile < 100.0f) {
        // 百分位模式
        computePercentileHu(huMats, cropRect,
                            std::max(1, static_cast<int>(histSampleStride)),
                            static_cast<double>(gminPercentile),
                            static_cast<double>(gmaxPercentile),
                            gMin, gMax);
    } else {
        // 兼容旧路径：绝对 min/max
        computeSeriesGminGmax(huMats, cropRect,
                              std::max(1, static_cast<int>(histSampleStride)),
                              gMin, gMax);
    }
    if (!(gMax > gMin)) {
        gMin = 0.0f;
        gMax = 1.0f;
    }
    LOGI("processCtSeries: ROI Gmin=%.1f Gmax=%.1f", gMin, gMax);

    std::vector<int> hist;
    aggregateSeriesHistogram(huMats, cropRect,
                             static_cast<double>(gMin),
                             static_cast<double>(gMax),
                             nBins,
                             std::max(1, static_cast<int>(histSampleStride)),
                             hist);
    // 直方图日志：前 16 个 bin + 总频数
    long long histTotal = 0;
    for (int v: hist) histTotal += v;
    LOGI("processCtSeries: hist total=%lld first16=[%lld,%d,%d,%d,%d,%d,%d,%d,"
         "%d,%d,%d,%d,%d,%d,%d,%d]",
         histTotal,
         hist.size() > 0 ? (long long) hist[0] : 0LL,
         hist.size() > 1 ? hist[1] : 0, hist.size() > 2 ? hist[2] : 0,
         hist.size() > 3 ? hist[3] : 0, hist.size() > 4 ? hist[4] : 0,
         hist.size() > 5 ? hist[5] : 0, hist.size() > 6 ? hist[6] : 0,
         hist.size() > 7 ? hist[7] : 0, hist.size() > 8 ? hist[8] : 0,
         hist.size() > 9 ? hist[9] : 0, hist.size() > 10 ? hist[10] : 0,
         hist.size() > 11 ? hist[11] : 0, hist.size() > 12 ? hist[12] : 0,
         hist.size() > 13 ? hist[13] : 0, hist.size() > 14 ? hist[14] : 0,
         hist.size() > 15 ? hist[15] : 0);

    // v2：直方图质量统计 + 退化判断
    HistogramStats hs = computeHistogramStats(hist);
    LOGI("processCtSeries: hist stats entropy=%.3f maxBinFrac=%.3f numPeaks=%d",
         hs.entropy, hs.maxBinFrac, hs.numPeaks);

    // ---- 6) 自适应调窗（v2：可选退化到预设常用窗）----
    AdaptiveWindowResult aw;
    aw.hBins = (static_cast<double>(gMax) - gMin) /
               static_cast<double>(nBins);
    if (aw.hBins <= 0.0) aw.hBins = 1.0;
    bool awOK = computeAdaptiveWindow(hist, nBins, n0, n1, aw);
    if (!awOK) {
        aw.c = (gMin + gMax) * 0.5;
        aw.w = std::max<double>(1.0, gMax - gMin);
        aw.b = 1;
    }
    LOGI("processCtSeries: paper algo -> c=%.2f w=%.2f B=%d (T0=%.1f T1=%.1f)",
         aw.c, aw.w, aw.b, aw.t0, aw.t1);

    // v2：若直方图退化，叠加预设窗的"安全底"
    double finalC = aw.c, finalW = aw.w;
    if (enableHistFallback != 0) {
        double fbC, fbW;
        bool usedDefault;
        pickDefaultWindow(gMin, gMax, hs,
                          fallbackWindowCenter, fallbackWindowWidth,
                          fbC, fbW, usedDefault);
        if (usedDefault) {
            // 取"调窗算法与预设窗"中较宽的那个：保证不丢对比度
            finalC = fbC;
            finalW = fbW;
            LOGW("processCtSeries: histogram fallback ENGAGED -> c=%.1f w=%.1f",
                 finalC, finalW);
        }
    }

    // ---- 7) 窗映射 + 可选 CLAHE 增强 + 8) 灰 -> RGBA，按 slice 输出 ----
    for (jsize s = 0; s < nSlices; ++s) {
        cv::Mat cropped = huMats[s](cropRect).clone();
        cv::Mat disp8u = applyWindow8u(cropped, finalC, finalW, photometric);
        // v2: 可选对显示图做 CLAHE 提升软组织对比度
        cv::Mat final8u = applyDisplayClahe(disp8u, enableDisplayClahe != 0,
                                            displayClaheClip, displayClaheTile);
        jbyteArray rgba = nullptr;
        if (!gray8uToRgbaJBytes(env, final8u, rgba)) {
            LOGE("processCtSeries: rgba conversion failed at slice %d", s);
            return JNI_FALSE;
        }
        env->SetObjectArrayElement(outDisplays, s, rgba);
        env->DeleteLocalRef(rgba);
    }

    // ---- 写回各类 out 参数 ----
    if (outWindowStats != nullptr &&
        env->GetArrayLength(outWindowStats) >= 11) {
        jdouble ws[11] = {
                finalC, finalW,
                static_cast<double>(gMin), static_cast<double>(gMax),
                aw.hBins, aw.t0, aw.t1,
                static_cast<double>(aw.b),
                0.0, 0.0,            // srcMin/srcMax(SV) 由调用方计算
                0.0,                 // 预留
        };
        // 计算 srcMin/srcMax(SV) 从 rawBytes
        long long svMin = std::numeric_limits<long long>::max();
        long long svMax = std::numeric_limits<long long>::min();
        for (const auto &rb: rawBytes) {
            if (bitsAllocated == 16) {
                for (size_t i = 0; i + 1 < rb.size(); i += 2) {
                    int16_t v = static_cast<int16_t>(
                            (uint16_t) rb[i] | ((uint16_t) rb[i + 1] << 8));
                    if (v < svMin) svMin = v;
                    if (v > svMax) svMax = v;
                }
            } else {
                for (uint8_t b: rb) {
                    if (b < svMin) svMin = b;
                    if (b > svMax) svMax = b;
                }
            }
        }
        ws[8] = static_cast<double>(svMin);
        ws[9] = static_cast<double>(svMax);
        ws[10] = static_cast<double>(n0) * 1e6;  // 透传 N0/N1*1e6（给 UI 显示）
        env->SetDoubleArrayRegion(outWindowStats, 0, 11, ws);
    }
    if (outCropAndOut != nullptr &&
        env->GetArrayLength(outCropAndOut) >= 10) {
        jint co[10] = {
                cropRect.x, cropRect.y,
                cropRect.width, cropRect.height,
                cropRect.width, cropRect.height,  // outW/outH 与 cropRect 一致
                static_cast<jint>(nSlices),
                static_cast<jint>(n0 * 1e6),
                static_cast<jint>(n1 * 1e6),
                1,                                // okFlag
        };
        env->SetIntArrayRegion(outCropAndOut, 0, 10, co);
    }
    if (outHistogram != nullptr) {
        jsize histLen = env->GetArrayLength(outHistogram);
        jsize cap = std::min<jsize>(histLen, static_cast<jsize>(hist.size()));
        if (cap > 0) {
            env->SetIntArrayRegion(outHistogram, 0, cap,
                                   reinterpret_cast<const jint *>(hist.data()));
        }
    }
    if (outUsedFlags != nullptr && env->GetArrayLength(outUsedFlags) >= 1) {
        jint flag[1] = {1};
        env->SetIntArrayRegion(outUsedFlags, 0, 1, flag);
    }
    LOGI("processCtSeries: DONE finalC=%.2f finalW=%.2f cropRect=(%d,%d,%d,%d)",
         finalC, finalW, cropRect.x, cropRect.y, cropRect.width, cropRect.height);
    return JNI_TRUE;
}

// =============================================================================
// 6) X-ray tailorImage：实现 ProcessPixelData-readme.md §4.1 的多阶段裁剪
//    raw 16-bit → 8-bit 降级 → OTSU 找前景 → minAreaRect 旋转 → 锐化 →
//    Sobel x-y 差异 → OTSU → 闭运算 → boundingRect → 输出裁剪后 16-bit raw
//
//    入参：
//     rawBuffer        jbyteArray，width × height × bitsAllocated/8 字节
//     width/height     像素几何
//     bitsAllocated    16 / 8
//     pixelSigned      0/1
//     minAreaThreshold < 该面积的矩形视为裁剪失败
//     enableSobel      0/1，是否启用 Sobel + 闭运算（false 只用 OTSU 兜底）
//     morphCross       闭运算核边长（0=跳过）
//     otsuThresholdLow 排除全黑背景的固定下界
//
//    出参：
//     outCroppedBytes  jbyteArray，裁剪后 raw（width × height × bitsAllocated/8）
//     outInfo          jintArray，长度 6：[cropL, cropT, cropW, cropH, rotateAngle*1000, okFlag]
// =============================================================================
static jbyteArray buildFullImageResult(JNIEnv *env, const cv::Mat &sv,
                                       jintArray outInfo) {
    cv::Mat out16;
    if (sv.type() == CV_16S || sv.type() == CV_16U) {
        out16 = sv.clone();
    } else {
        sv.convertTo(out16, CV_16S);
    }
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

static jbyteArray native_tailorImage(
        JNIEnv *env, jclass clazz,
        jbyteArray rawBuffer,
        jint width, jint height,
        jint bitsAllocated, jint pixelSigned,
        jint minAreaThreshold,
        jboolean enableSobel,
        jint morphCross,
        jdouble otsuThresholdLow,
        jbyteArray outCroppedBytes,
        jintArray outInfo
) {
    (void) clazz;
    if (rawBuffer == nullptr) {
        LOGE("tailorImage: null rawBuffer");
        return nullptr;
    }
    if (width <= 0 || height <= 0) {
        LOGE("tailorImage: invalid size %dx%d", width, height);
        return nullptr;
    }
    LOGI("tailorImage: START %dx%d bits=%d sign=%d minArea=%d sobel=%d "
         "morph=%d otsuLow=%.1f",
         width, height, bitsAllocated, pixelSigned, minAreaThreshold,
         (int) enableSobel, morphCross, otsuThresholdLow);

    // ---- 1) 解码 raw → 16-bit int ----
    std::vector<uint8_t> raw;
    if (!copyJByteArray(env, rawBuffer, raw)) {
        LOGE("tailorImage: copy raw failed");
        return nullptr;
    }
    const size_t expected =
            static_cast<size_t>(width) * height * (bitsAllocated / 8);
    if (raw.size() < expected) {
        LOGE("tailorImage: raw size=%zu < expected=%zu", raw.size(), expected);
        return nullptr;
    }
    cv::Mat sv = wrapRawMat(raw.data(), width, height, bitsAllocated, pixelSigned);
    if (sv.empty()) {
        LOGE("tailorImage: wrapRawMat failed");
        return nullptr;
    }

    // ---- 2) 16→8 降级（alpha=1/256，丢弃低字节）----
    cv::Mat img8u;
    {
        cv::Mat tmp32f;
        sv.convertTo(tmp32f, CV_32F, 1.0 / 256.0, 0.0);
        cv::convertScaleAbs(tmp32f, img8u);
    }

    // ---- 3) OTSU 找前景 + 最大外轮廓 ----
    cv::Mat otsu;
    cv::threshold(img8u, otsu, otsuThresholdLow, 255.0, cv::THRESH_OTSU);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(otsu, contours, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) {
        LOGW("tailorImage: no contours found, fallback to full image");
        return buildFullImageResult(env, sv, outInfo);
    }
    // 取最大轮廓
    auto maxIt = std::max_element(
            contours.begin(), contours.end(),
            [](const std::vector<cv::Point> &a,
               const std::vector<cv::Point> &b) {
                return cv::contourArea(a) < cv::contourArea(b);
            });
    const double maxArea = cv::contourArea(*maxIt);
    LOGI("tailorImage: max contour area=%.1f", maxArea);

    // ---- 4) minAreaRect 角度归一化 ----
    cv::RotatedRect rr;
    {
        std::vector<cv::Point2f> pts2f;
        for (const auto &p: *maxIt) pts2f.emplace_back(p.x, p.y);
        rr = cv::minAreaRect(pts2f);
    }
    double angle = rr.angle;
    if (std::abs(angle) > 45.0) angle += 90.0;
    if (std::abs(angle) < 1e-3) angle = 0.0;
    LOGI("tailorImage: minAreaRect angle=%.2f (after normalize=%.2f)",
         rr.angle, angle);

    // ---- 5) 旋转校正（以图像中心为旋转中心）----
    cv::Mat rotated;
    if (std::abs(angle) > 0.5) {
        cv::Point2f center(static_cast<float>(width) * 0.5f,
                           static_cast<float>(height) * 0.5f);
        cv::Mat M = cv::getRotationMatrix2D(center, angle, 1.0);
        cv::warpAffine(sv, rotated, M, cv::Size(width, height),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                       cv::Scalar(0));
        cv::warpAffine(img8u, img8u, M, cv::Size(width, height),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                       cv::Scalar(0));
        LOGI("tailorImage: rotated by %.2f deg", angle);
    } else {
        rotated = sv.clone();
    }

    // ---- 6) 锐化 + Sobel x-y 差异（可选）----
    cv::Rect boundingRectAll(0, 0, width, height);
    bool usedSobel = (enableSobel == JNI_TRUE);
    if (usedSobel && morphCross > 0) {
        // 锐化核：center=9
        cv::Mat sharpKernel = (cv::Mat_<float>(3, 3) <<
                                                     -1, -1, -1,
                -1, 9, -1,
                -1, -1, -1);
        cv::Mat sharpened;
        cv::filter2D(img8u, sharpened, CV_32F, sharpKernel, cv::Point(-1, -1), 0);
        cv::convertScaleAbs(sharpened, img8u);

        cv::Mat gx, gy;
        cv::Sobel(rotated, gx, CV_16S, 1, 0, 3);
        cv::Sobel(rotated, gy, CV_16S, 0, 1, 3);
        cv::Mat grad;
        cv::subtract(gx, gy, grad);
        cv::Mat grad8u;
        cv::convertScaleAbs(grad, grad8u);

        cv::Mat blurred;
        cv::blur(grad8u, blurred, cv::Size(25, 25));
        cv::Mat otsu2;
        cv::threshold(blurred, otsu2, 0, 255,
                      cv::THRESH_BINARY | cv::THRESH_OTSU);
        cv::Mat dil;
        cv::dilate(otsu2, dil, cv::Mat(), cv::Point(-1, -1), 4);

        cv::Mat closed;
        cv::Mat kernel = cv::getStructuringElement(
                cv::MORPH_CROSS, cv::Size(morphCross, morphCross));
        cv::morphologyEx(dil, closed, cv::MORPH_CLOSE, kernel);

        std::vector<std::vector<cv::Point>> contours2;
        cv::findContours(closed, contours2, cv::RETR_EXTERNAL,
                         cv::CHAIN_APPROX_SIMPLE);
        if (!contours2.empty()) {
            auto it2 = std::max_element(
                    contours2.begin(), contours2.end(),
                    [](const std::vector<cv::Point> &a,
                       const std::vector<cv::Point> &b) {
                        return cv::contourArea(a) < cv::contourArea(b);
                    });
            boundingRectAll = cv::boundingRect(*it2);
            LOGI("tailorImage: sobel+close boundingRect=(%d,%d,%d,%d) area=%d",
                 boundingRectAll.x, boundingRectAll.y,
                 boundingRectAll.width, boundingRectAll.height,
                 boundingRectAll.width * boundingRectAll.height);
        }
    } else if (maxArea > 0) {
        // 不走 Sobel：直接用第一阶段的最大轮廓做 boundingRect
        boundingRectAll = cv::boundingRect(*maxIt);
        LOGI("tailorImage: OTSU-only boundingRect=(%d,%d,%d,%d) area=%d",
             boundingRectAll.x, boundingRectAll.y,
             boundingRectAll.width, boundingRectAll.height,
             boundingRectAll.width * boundingRectAll.height);
    }

    // ---- 7) 抗噪兜底：面积过小回退全图 ----
    if (boundingRectAll.width * boundingRectAll.height < minAreaThreshold) {
        LOGW("tailorImage: cropped area=%d < minArea=%d, fallback to full image",
             boundingRectAll.width * boundingRectAll.height, minAreaThreshold);
        return buildFullImageResult(env, sv, outInfo);
    }

    // ---- 8) 裁剪输出 ----
    cv::Mat cropped = rotated(boundingRectAll).clone();
    // 16-bit 强制转回大端字节（与 raw buffer 同格式）
    cv::Mat cropped16;
    if (bitsAllocated == 16) {
        cropped.convertTo(cropped16, pixelSigned ? CV_16S : CV_16U);
    } else {
        cropped16 = cropped;
    }
    const size_t outSize = cropped16.total() * cropped16.elemSize();
    jbyteArray outBytes = env->NewByteArray(static_cast<jsize>(outSize));
    if (outBytes == nullptr) {
        LOGE("tailorImage: NewByteArray failed");
        return nullptr;
    }
    env->SetByteArrayRegion(outBytes, 0, static_cast<jsize>(outSize),
                            reinterpret_cast<const jbyte *>(cropped16.data));

    if (outInfo != nullptr && env->GetArrayLength(outInfo) >= 6) {
        jint info[6] = {
                boundingRectAll.x, boundingRectAll.y,
                boundingRectAll.width, boundingRectAll.height,
                static_cast<jint>(angle * 1000.0),
                1,
        };
        env->SetIntArrayRegion(outInfo, 0, 6, info);
    }
    if (outCroppedBytes != nullptr) {
        env->SetByteArrayRegion(outCroppedBytes, 0, static_cast<jsize>(outSize),
                                reinterpret_cast<const jbyte *>(cropped16.data));
    }
    LOGI("tailorImage: DONE rect=(%d,%d,%d,%d) angle=%.2f",
         boundingRectAll.x, boundingRectAll.y,
         boundingRectAll.width, boundingRectAll.height, angle);
    return outBytes;
}

// =============================================================================
// 医学图像通用预处理接口 (Requirement 2)
// =============================================================================
static jbyteArray native_processMedicalCT(JNIEnv *env, jclass clazz,
                                          jbyteArray rawBuffer, jint width, jint height,
                                          jint bitDepth, jboolean bigEndian, jboolean isUint16,
                                          jintArray ops, jdoubleArray params, jintArray outInfo) {
    LOGI("native_processMedicalCT: START width=%d, height=%d, bitDepth=%d", width, height, bitDepth);
    if (rawBuffer == nullptr || ops == nullptr || params == nullptr || outInfo == nullptr) {
        LOGE("processMedicalCT: null arguments");
        return nullptr;
    }

    jbyte *pRaw = env->GetByteArrayElements(rawBuffer, nullptr);
    jint *pOps = env->GetIntArrayElements(ops, nullptr);
    jdouble *pParams = env->GetDoubleArrayElements(params, nullptr);
    jsize opsCount = env->GetArrayLength(ops);
    LOGD("native_processMedicalCT: opsCount=%d", opsCount);

    // 1. 载入原始像素
    cv::Mat mat = CTPreprocess::LoadRawPixelBuffer(pRaw, height,
                                                   width, isUint16, 0, bigEndian);

    // 2. 依次执行选中的预处理算子
    int paramIdx = 0;
    for (int i = 0; i < opsCount; ++i) {
        int op = pOps[i];
        LOGI("native_processMedicalCT: Executing op ID %d (Step %d/%d)", op, i + 1, opsCount);
        switch (op) {
            case 1: // Gaussian: [kernel, sigma]
            {
                int k = (int) pParams[paramIdx++];
                double sigma = pParams[paramIdx++];
                mat = CTPreprocess::DenoiseGaussian(mat, k, sigma);
                break;
            }
            case 2: // Median: [kernel]
            {
                int k = (int) pParams[paramIdx++];
                mat = CTPreprocess::DenoiseMedian(mat, k);
                break;
            }
            case 3: // Bilateral: [d, sigmaColor, sigmaSpace]
            {
                int d = (int) pParams[paramIdx++];
                double sc = pParams[paramIdx++];
                double ss = pParams[paramIdx++];
                mat = CTPreprocess::DenoiseBilateral(mat, d, sc, ss);
                break;
            }
            case 4: // FFT: [radius]
            {
                float r = (float) pParams[paramIdx++];
                mat = CTPreprocess::DenoiseFrequencyFFT(mat, r);
                break;
            }
            case 5: // Resample(Size): [targetW, targetH, isUpSample]
            {
                int tw = (int) pParams[paramIdx++];
                int th = (int) pParams[paramIdx++];
                bool up = pParams[paramIdx++] > 0.5;
                mat = CTPreprocess::ResampleImage(mat, tw, th, up);
                break;
            }
            case 6: // Resample(Scale): [scaleX, scaleY]
            {
                float sx = (float) pParams[paramIdx++];
                float sy = (float) pParams[paramIdx++];
                mat = CTPreprocess::ResampleByScale(mat, sx, sy);
                break;
            }
            case 7: // GlobalEqualize: []
            {
                // equalization requires 8u
                if (mat.depth() != CV_8U) {
                    LOGD("native_processMedicalCT: GlobalEqualize - converting to 8U");
                    double minV, maxV;
                    cv::minMaxLoc(mat, &minV, &maxV);
                    mat.convertTo(mat, CV_8U, 255.0 / (maxV - minV + 1e-7),
                                  -minV * 255.0 / (maxV - minV + 1e-7));
                }
                mat = CTPreprocess::EnhanceGlobalEqualize(mat);
                break;
            }
            case 8: // CLAHE: [clipLimit, tileX, tileY]
            {
                double clip = pParams[paramIdx++];
                int tx = (int) pParams[paramIdx++];
                int ty = (int) pParams[paramIdx++];
                if (mat.depth() != CV_8U && mat.depth() != CV_16U) {
                    LOGD("native_processMedicalCT: CLAHE - converting to 8U");
                    double minV, maxV;
                    cv::minMaxLoc(mat, &minV, &maxV);
                    mat.convertTo(mat, CV_8U, 255.0 / (maxV - minV + 1e-7),
                                  -minV * 255.0 / (maxV - minV + 1e-7));
                }
                mat = CTPreprocess::EnhanceCLAHE(mat, clip, cv::Size(tx, ty));
                break;
            }
            case 9: // ContrastStretch: []
            {
                mat = CTPreprocess::EnhanceContrastStretch(mat);
                break;
            }
            case 10: // ConvertRawToHU: [slope, intercept]
            {
                float slope = (float) pParams[paramIdx++];
                float intercept = (float) pParams[paramIdx++];
                mat = CTPreprocess::ConvertRawToHU(mat, slope, intercept);
                break;
            }
            default:
                LOGW("native_processMedicalCT: unknown op id %d", op);
                break;
        }
    }

    // 3. 最终归一化到 8-bit RGBA 用于 Bitmap 显示
    LOGD("native_processMedicalCT: Final conversion to RGBA");
    cv::Mat out8u;
    if (mat.depth() != CV_8U) {
        double minV, maxV;
        cv::minMaxLoc(mat, &minV, &maxV);
        mat.convertTo(out8u, CV_8U, 255.0 / (maxV - minV + 1e-7),
                      -minV * 255.0 / (maxV - minV + 1e-7));
    } else {
        out8u = mat;
    }

    cv::Mat rgba;
    cv::cvtColor(out8u, rgba, cv::COLOR_GRAY2RGBA);

    jsize rgbaSize = rgba.total() * rgba.elemSize();
    jbyteArray resultArr = env->NewByteArray(rgbaSize);
    env->SetByteArrayRegion(resultArr, 0, rgbaSize, (jbyte *) rgba.data);

    // 4. 写回输出信息
    jint *pOutInfo = env->GetIntArrayElements(outInfo, nullptr);
    if (env->GetArrayLength(outInfo) >= 4) {
        pOutInfo[0] = rgba.cols;
        pOutInfo[1] = rgba.rows;
        double minV, maxV;
        cv::minMaxLoc(mat, &minV, &maxV);
        pOutInfo[2] = (int) minV;
        pOutInfo[3] = (int) maxV;
        LOGD("native_processMedicalCT: outInfo [w=%d, h=%d, min=%d, max=%d]"
             , pOutInfo[0], pOutInfo[1], pOutInfo[2], pOutInfo[3]);
    }
    env->ReleaseIntArrayElements(outInfo, pOutInfo, 0);

    // 5. 释放
    env->ReleaseByteArrayElements(rawBuffer, pRaw, JNI_ABORT);
    env->ReleaseIntArrayElements(ops, pOps, JNI_ABORT);
    env->ReleaseDoubleArrayElements(params, pParams, JNI_ABORT);

    LOGI("native_processMedicalCT: DONE. outSize=%dx%d", rgba.cols, rgba.rows);
    return resultArr;
}

// =============================================================================
// JNI Registration
// =============================================================================
static const char *const kClassName = "com/example/rawpixeldeal/jni/RawPixelDealJni";

static const JNINativeMethod kMethods[] = {
        {"stringFromJNI",
                "()Ljava/lang/String;",
                (void *) native_stringFromJNI},
        {"getOpenCVVersion",
                "()Ljava/lang/String;",
                (void *) native_getOpenCVVersion},
        {"processRawGrayPixels",
                "(II[B)[I",
                (void *) native_processRawGrayPixels},
        {"processRawToRgba",
                "(III[BIIIIIDI[I)[B",
                (void *) native_processRawToRgba},
        // CT 序列级处理管线（ct-opencv-raw-buffer-windowing-prd）v2
        //   ([[B,      rawBuffers（Kotlin Array<ByteArray> -> byte[][]）
        //    I,         width
        //    I,         height
        //    I,         bitsAllocated
        //    I,         pixelSigned
        //    D,         rescaleSlope
        //    D,         rescaleIntercept
        //    I,         photometric
        //    F,         bodyThreshold
        //    I,         cropMargin
        //    I,         bodyMorphSize
        //    I,         minBodyAreaPx
        //    I,         nBins
        //    D,         n0
        //    D,         n1
        //    I,         histSampleStride
        //    I,         enableBilateral
        //    I,         bilateralD
        //    D,         bilateralSigmaColor
        //    D,         bilateralSigmaSpace
        //    F,         clipLowHu
        //    F,         clipHighHu
        //    --- v2 新增 ---
        //    I,         enableAutoPixelSign
        //    F,         gminPercentile
        //    F,         gmaxPercentile
        //    I,         enableHistFallback
        //    D,         fallbackWindowCenter
        //    D,         fallbackWindowWidth
        //    I,         enableDisplayClahe
        //    D,         displayClaheClip
        //    I,         displayClaheTile
        //    I,         cropFirst
        //    [[B,       outDisplays（每片一个 byte[] = RGBA8888）
        //    double[],   outWindowStats
        //    int[],      outCropAndOut
        //    int[],      outHistogram
        //    int[]       outUsedFlags
        //   )Z
        {"processCtSeries",
                "([[BIIIIDDIFIIIIDDIIIDDFFIFFIDDIDII[[B[D[I[I[I)Z",
                (void *) native_processCtSeries},
        // X-ray tailorImage：实现 ProcessPixelData-readme.md §4.1 多阶段裁剪
        //   ([B,            rawBuffer（大端 16-bit）
        //    I,             width
        //    I,             height
        //    I,             bitsAllocated
        //    I,             pixelSigned
        //    I,             minAreaThreshold
        //    Z,             enableSobel
        //    I,             morphCross
        //    D,             otsuThresholdLow
        //    [B,            outCroppedBytes
        //    [I             outInfo
        //   )[B
        {"tailorImage",
                "([BIIIIIZID[B[I)[B",
                (void *) native_tailorImage},
        {"processMedicalCT",
                "([BIIIZZ[I[D[I)[B",
                (void *) native_processMedicalCT},
};

extern "C" jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass clazz = env->FindClass(kClassName);
    if (clazz == nullptr) return JNI_ERR;

    if (env->RegisterNatives(clazz, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0) {
        return JNI_ERR;
    }

    LOGI("RawPixelDealJni native methods registered");
    return JNI_VERSION_1_6;
}
