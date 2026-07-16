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

#define TAG "RawPixelDealJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
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
    if (bitDepth == 16) {
        // min/max 归一化到 0~255，便于人工筛查观察全动态范围
        double minV = 0.0, maxV = 0.0;
        cv::minMaxLoc(src, &minV, &maxV);
        // OpenCV 4.x 用 convertTo 带掩码做线性拉伸
        src.convertTo(gray8, CV_8UC1,
                      255.0 / std::max(1.0, (maxV - minV)),
                      -minV * 255.0 / std::max(1.0, (maxV - minV)));
        LOGI("processRawToRgba: 16-bit min=%.1f max=%.1f -> 8-bit", minV, maxV);

        // 回写 head（srcMin, srcMax, etc.），由 Kotlin 端做文字展示
        if (head != nullptr && env->GetArrayLength(head) >= 4) {
            jint hOut[4] = {
                    (jint) minV,
                    (jint) maxV,
                    (jint) src.cols,
                    (jint) src.rows,
            };
            env->SetIntArrayRegion(head, 0, 4, hOut);
        }
    } else {
        gray8 = src.clone();
        if (head != nullptr && env->GetArrayLength(head) >= 4) {
            jint hOut[4] = {0, 255, width, height};
            env->SetIntArrayRegion(head, 0, 4, hOut);
        }
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
//   raw buffer(s)  --[1 解码]-->  stored value 矩阵 (CV_16U/CV_16S)
//                   --[2 HU 标准化]-->  float HU 矩阵  (CV_32F)
//                   --[3 OpenCV 优化]-->  去噪 + 极端值抑制
//                   --[4 自动裁剪]-->  定位人体 ROI (cv::Rect)
//                   --[5 序列直方图]-->  基于 ROI 聚合 256-bin Hist
//                   --[6 自适应调窗]-->  (c, w) by 论文算法
//                   --[7 窗映射]-->  CV_8U 显示图（按 cropRect 裁剪后）
//                   --[8 灰 -> RGBA]-->  按 slice 返回 jbyteArray
//
//   入参：
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
//
//   出参：
//     displayRgba jobjectArray —— 每个 slice 的 RGBA8888 bytes
//     windowStats jdoubleArray —— [c, w, Gmin, Gmax, H_bins, T0, T1, B,
//                                  srcMin(sv), srcMax(sv), huMin, huMax]
//     cropAndOut  jintArray    —— [cropL, cropT, cropR, cropB, outW, outH,
//                                  sliceCount, usedN0*1e6, usedN1*1e6,
//                                  debugFlag]
//     histogram   jintArray    —— 256 长度直方图（已剔除/合并前）
//     outPhotometric jintArray 长度 1 —— 实际使用的 photometric（输入透传，便于 UI 提示）
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
    for (const cv::Mat &hu : huSlices) {
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
    for (const cv::Mat &hu : huSlices) {
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
    for (int v : histOrig) T += v;
    if (T <= 0) return false;
    out.t0 = static_cast<double>(T) * n0;
    out.t1 = static_cast<double>(T) * n1;

    // 步骤 3：阈值剔除（保留 Hist[i] >= T0）
    std::vector<int> M;
    M.reserve(nBins);
    for (int v : histOrig) {
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
        svMats.push_back(sv);
    }

    // ---- 2) HU 标准化 + 3) OpenCV 优化 ----
    std::vector<cv::Mat> huMats;
    huMats.reserve(nSlices);
    for (jsize s = 0; s < nSlices; ++s) {
        cv::Mat hu = toHu(svMats[s], rescaleSlope, rescaleIntercept);
        cv::Mat opt = optimizeHu(hu, enableBilateral != 0, bilateralD,
                                 bilateralSigmaColor, bilateralSigmaSpace,
                                 clipLowHu, clipHighHu);
        huMats.push_back(std::move(opt));
    }

    // ---- 4) 自动裁剪：基于第一片（或中位片）的 HU 决定 cropRect ----
    // 论文要求"序列级一致性"，因此 cropRect 由首片决定即可（CT 序列通常同体型）。
    int pickIdx = std::min<int>(static_cast<int>(nSlices) - 1,
                                nSlices / 2);
    cv::Rect cropRect = autoCropBodyRoi(
            huMats[pickIdx], bodyThreshold, bodyMorphSize, minBodyAreaPx,
            cropMargin);
    if (cropRect.area() <= 0) {
        cropRect = cv::Rect(0, 0, width, height);
    }
    LOGI("processCtSeries: pickIdx=%d cropRect=(%d,%d,%d,%d)",
         pickIdx, cropRect.x, cropRect.y, cropRect.width, cropRect.height);

    // ---- 5) 序列级直方图：在 ROI 内聚合 ----
    float gMin = 0.0f, gMax = 0.0f;
    computeSeriesGminGmax(huMats, cropRect,
                          std::max(1, static_cast<int>(histSampleStride)),
                          gMin, gMax);
    if (!(gMax > gMin)) {
        // 直方图过空：退化为一个全 1 宽度 bin 防止 0 除
        gMin = 0.0f;
        gMax = 1.0f;
    }
    std::vector<int> hist;
    aggregateSeriesHistogram(huMats, cropRect,
                             static_cast<double>(gMin),
                             static_cast<double>(gMax),
                             nBins,
                             std::max(1, static_cast<int>(histSampleStride)),
                             hist);

    // ---- 6) 自适应调窗 ----
    AdaptiveWindowResult aw;
    aw.hBins = (static_cast<double>(gMax) - gMin) /
               static_cast<double>(nBins);
    if (aw.hBins <= 0.0) aw.hBins = 1.0;
    if (!computeAdaptiveWindow(hist, nBins, n0, n1, aw)) {
        // 极端退化：直接给一个保守窗
        aw.c = (gMin + gMax) * 0.5;
        aw.w = std::max<double>(1.0, gMax - gMin);
        aw.b = 1;
    }
    LOGI("processCtSeries: Gmin=%.1f Gmax=%.1f H_bins=%.3f T0=%.1f T1=%.1f "
         "B=%d -> c=%.2f w=%.2f",
         static_cast<double>(gMin), static_cast<double>(gMax), aw.hBins,
         aw.t0, aw.t1, aw.b, aw.c, aw.w);

    // ---- 7) 窗映射 + 8) 灰 -> RGBA，按 slice 输出 ----
    for (jsize s = 0; s < nSlices; ++s) {
        cv::Mat cropped = huMats[s](cropRect).clone();
        cv::Mat disp8u = applyWindow8u(cropped, aw.c, aw.w, photometric);
        jbyteArray rgba = nullptr;
        if (!gray8uToRgbaJBytes(env, disp8u, rgba)) {
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
                aw.c, aw.w,
                static_cast<double>(gMin), static_cast<double>(gMax),
                aw.hBins, aw.t0, aw.t1,
                static_cast<double>(aw.b),
                0.0, 0.0,            // srcMin/srcMax(SV) 由调用方计算
                0.0,                 // 预留
        };
        // 计算 srcMin/srcMax(SV) 从 rawBytes
        long long svMin = std::numeric_limits<long long>::max();
        long long svMax = std::numeric_limits<long long>::min();
        for (const auto &rb : rawBytes) {
            if (bitsAllocated == 16) {
                for (size_t i = 0; i + 1 < rb.size(); i += 2) {
                    int16_t v = static_cast<int16_t>(
                            (uint16_t) rb[i] | ((uint16_t) rb[i + 1] << 8));
                    if (v < svMin) svMin = v;
                    if (v > svMax) svMax = v;
                }
            } else {
                for (uint8_t b : rb) {
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
    return JNI_TRUE;
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
        // CT 序列级处理管线（ct-opencv-raw-buffer-windowing-prd）
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
        //    [[B,       outDisplays（每片一个 byte[] = RGBA8888）
        //    double[],   outWindowStats
        //    int[],      outCropAndOut
        //    int[],      outHistogram
        //    int[]       outUsedFlags
        //   )Z
        {"processCtSeries",
                "([[BIIIIDDIFIIIIDDIIIDDFF[[B[D[I[I[I)Z",
                (void *) native_processCtSeries},
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
