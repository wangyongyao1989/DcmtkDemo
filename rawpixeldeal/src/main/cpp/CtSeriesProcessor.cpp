#include "include/CtSeriesProcessor.h"
#include <opencv2/imgproc.hpp>
#include <android/log.h>
#include <cmath>
#include <algorithm>
#include <limits>

#define TAG "CtSeriesProcessor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

namespace CtSeriesProcessor {

    /**
     * 将原始像素值(SV)转换为亨氏单位(HU)。
     * 公式: HU = SV * slope + intercept
     */
    cv::Mat toHu(const cv::Mat &sv, double slope, double intercept) {
        cv::Mat hu;
        if (sv.empty()) return hu;
        // 使用 CV_32F 保持浮点精度，避免 HU 转换过程中的精度损失
        sv.convertTo(hu, CV_32FC1, slope, intercept);
        return hu;
    }

    /**
     * HU 域优化：包含双边滤波去噪和极值截断。
     * 双边滤波(Bilateral Filter)能在平滑噪声的同时很好地保留组织边缘。
     */
    cv::Mat optimizeHu(const cv::Mat &hu, bool enableBilateral,
                      int bilateralD, double sigmaColor, double sigmaSpace,
                      float clipLowHu, float clipHighHu) {
        cv::Mat out = hu.clone();
        if (out.empty()) return out;

        if (enableBilateral) {
            // 双边滤波要求 8U 或浮点，这里先归一化到 8U 加速处理
            double mn = 0.0, mx = 0.0;
            cv::minMaxLoc(out, &mn, &mx);
            const double span = std::max(1e-6, mx - mn);
            cv::Mat normalized;
            out.convertTo(normalized, CV_8UC1, 255.0 / span, -mn * 255.0 / span);
            cv::Mat filtered8u;
            cv::bilateralFilter(normalized, filtered8u, bilateralD, sigmaColor,
                                sigmaSpace, cv::BORDER_REPLICATE);
            // 滤波后再映射回原始 HU 范围
            filtered8u.convertTo(out, CV_32FC1, span / 255.0, mn);
        }

        // HU 极值截断：限制数据在医学有效范围内（如 -1024 到 3071）
        cv::threshold(out, out, clipHighHu, clipHighHu, cv::THRESH_TRUNC);
        cv::max(out, clipLowHu, out);
        return out;
    }

    /**
     * 自动检测人体 ROI (Region of Interest)。
     * 通过阈值分割和连通域分析定位图像中的主要人体组织，排除背景空气干扰。
     */
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
            cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
            cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
        }

        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S);
        if (n <= 1) return cv::Rect(0, 0, hu.cols, hu.rows);

        int bestLabel = -1;
        int bestArea = 0;
        for (int i = 1; i < n; ++i) {
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

        int x0 = std::max(0, x - marginPx);
        int y0 = std::max(0, y - marginPx);
        int x1 = std::min(hu.cols, x + w + marginPx);
        int y1 = std::min(hu.rows, y + h + marginPx);
        return cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
    }

    cv::Rect tryAutoCropBodyRoiEx(const cv::Mat &hu, float bodyThreshold,
                                 int morphSize, int minBodyAreaPx,
                                 int marginPx) {
        const float thresholds[3] = {bodyThreshold, -300.0f, -100.0f};
        const char *names[3] = {"primary", "loose(-300)", "very-loose(-100)"};
        for (int i = 0; i < 3; ++i) {
            cv::Rect r = autoCropBodyRoi(hu, thresholds[i], morphSize, minBodyAreaPx, marginPx);
            bool ok = (r.width < hu.cols) && (r.height < hu.rows);
            LOGI("tryAutoCropBodyRoiEx: %s thr=%.0f -> rect=(%d,%d,%d,%d) %s",
                 names[i], thresholds[i], r.x, r.y, r.width, r.height, ok ? "OK" : "FULL");
            if (ok) return r;
        }
        LOGW("tryAutoCropBodyRoiEx: all thresholds failed, fallback to full image");
        return cv::Rect(0, 0, hu.cols, hu.rows);
    }

    void computePercentileHu(const std::vector<cv::Mat> &huSlices,
                            const cv::Rect &roi, int stride,
                            double pLow, double pHigh,
                            float &gMinOut, float &gMaxOut) {
        gMinOut = 0.0f; gMaxOut = 1.0f;
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
        size_t idxLow = static_cast<size_t>(std::max(0.0, std::min(100.0, pLow)) * (n - 1) / 100.0);
        size_t idxHigh = static_cast<size_t>(std::max(0.0, std::min(100.0, pHigh)) * (n - 1) / 100.0);
        gMinOut = values[idxLow];
        gMaxOut = values[idxHigh];
        if (!(gMaxOut > gMinOut)) gMaxOut = gMinOut + 1.0f;
        LOGI("computePercentileHu: n=%zu pLow=%.2f pHigh=%.2f -> Gmin=%.1f Gmax=%.1f", n, pLow, pHigh, gMinOut, gMaxOut);
    }

    bool computeSeriesGminGmax(const std::vector<cv::Mat> &huSlices,
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
        if (!any) { gMinOut = 0.0f; gMaxOut = 0.0f; return false; }
        return true;
    }

    void aggregateSeriesHistogram(const std::vector<cv::Mat> &huSlices,
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

    HistogramStats computeHistogramStats(const std::vector<int> &hist) {
        HistogramStats s{0.0, 0.0, 0};
        if (hist.empty()) return s;
        long long total = 0;
        for (int v: hist) total += v;
        if (total <= 0) return s;
        const double logN = std::log(static_cast<double>(hist.size()));
        bool inPeak = false;
        for (int v: hist) {
            double p = static_cast<double>(v) / static_cast<double>(total);
            if (p > 0.0 && logN > 0.0) s.entropy -= p * std::log(p) / logN;
            if (p > s.maxBinFrac) s.maxBinFrac = p;
            bool curPeak = (p > 0.05);
            if (curPeak && !inPeak) s.numPeaks++;
            inPeak = curPeak;
        }
        return s;
    }

    bool computeAdaptiveWindow(const std::vector<int> &histOrig,
                              int nBins, double n0, double n1,
                              AdaptiveWindowResult &out) {
        if (histOrig.empty() || nBins <= 0) return false;
        long long T = 0;
        for (int v: histOrig) T += v;
        if (T <= 0) return false;
        out.t0 = static_cast<double>(T) * n0;
        out.t1 = static_cast<double>(T) * n1;

        std::vector<int> M;
        M.reserve(nBins);
        for (int v: histOrig) {
            if (static_cast<double>(v) >= out.t0) M.push_back(v);
        }
        if (M.empty()) M = histOrig;

        std::vector<int> merged;
        merged.reserve(M.size());
        int cur = M[0];
        for (size_t i = 1; i < M.size(); ++i) {
            if (std::abs(M[i] - cur) < out.t1) cur += M[i];
            else { merged.push_back(cur); cur = M[i]; }
        }
        merged.push_back(cur);
        out.b = static_cast<int>(merged.size());
        if (out.b <= 0) out.b = 1;

        if (out.hBins <= 0.0) out.hBins = 1.0;
        out.c = static_cast<double>(out.b) * out.hBins * 0.125;
        out.w = static_cast<double>(out.b) * out.hBins + out.c;
        if (out.w < 1.0) out.w = 1.0;
        return true;
    }

    void pickDefaultWindow(float gmin, float gmax, HistogramStats hs,
                          double fallbackC, double fallbackW,
                          double &cOut, double &wOut, bool &usedDefault) {
        usedDefault = false;
        const bool skewed = (hs.maxBinFrac > 0.6) || (hs.entropy < 0.3);
        if (skewed) {
            cOut = fallbackC; wOut = fallbackW; usedDefault = true;
            LOGW("pickDefaultWindow: histogram skewed (entropy=%.2f maxBinFrac=%.2f) -> fallback window c=%.1f w=%.1f",
                 hs.entropy, hs.maxBinFrac, cOut, wOut);
            return;
        }
        double range = static_cast<double>(gmax) - static_cast<double>(gmin);
        cOut = (static_cast<double>(gmin) + static_cast<double>(gmax)) * 0.5;
        wOut = std::max(150.0, range * 0.7);
    }

    /**
     * 调窗核心分派函数：根据所选方法计算窗位(Center)和窗宽(Width)。
     *
     * @param method 算法索引:
     *   0: DEFAULT (127.5/255)
     *   1: CUMULATIVE_72 (72% 累积面积法)
     *   2: BIMODAL (双峰直方图法)
     *   3: ADAPTIVE (论文自适应法)
     *   5: MIN_MAX (全量程覆盖)
     *   6: PEAK_AREA_AUTO (智能波峰面积识别 - 推荐)
     */
    void pickWindowCenterWidth(int method, double minV, double maxV,
                               const std::vector<int> *hist, int nBins,
                               double &cOut, double &wOut) {
        // 兜底默认值（DEFAULT 调窗）
        cOut = 127.5;
        wOut = 255.0;
        const double span = maxV - minV;

        switch (method) {
            case 0: { // DEFAULT
                cOut = 127.5;
                wOut = 255.0;
                break;
            }
            case 5: { // MIN_MAX
                cOut = (minV + maxV) * 0.5;
                wOut = std::max(1.0, span);
                break;
            }
            case 1: { // CUMULATIVE_72
                if (hist == nullptr || hist->empty() || nBins <= 0) {
                    cOut = (minV + maxV) * 0.5;
                    wOut = std::max(1.0, span);
                    return;
                }
                long long total = 0;
                for (int v: *hist) total += v;
                long long threshold = (long long) (total * 0.72);
                long long cumulative = 0;
                int targetBin = 0;
                for (int i = 0; i < nBins; ++i) {
                    cumulative += (*hist)[i];
                    if (cumulative >= threshold) { targetBin = i; break; }
                }
                const double hBin = span / static_cast<double>(nBins);
                cOut = minV + (targetBin + 0.5) * hBin;
                wOut = 508.0; // 临床经验值
                break;
            }
            case 2: { // BIMODAL
                if (hist == nullptr || hist->empty() || nBins <= 0) {
                    cOut = (minV + maxV) * 0.5;
                    wOut = std::max(1.0, span);
                    return;
                }
                // 1) 找最高频 bin
                int leftPeakIdx = 0;
                int leftPeakFreq = 0;
                for (int i = 0; i < nBins; i++) {
                    if ((*hist)[i] > leftPeakFreq) {
                        leftPeakFreq = (*hist)[i];
                        leftPeakIdx = i;
                    }
                }
                // 2) 抑制主峰 ±5% 区间
                std::vector<int> suppressed(*hist);
                int radius = std::max(1, (int) (nBins * 0.05));
                for (int i = std::max(0, leftPeakIdx - radius);
                     i <= std::min(nBins - 1, leftPeakIdx + radius); i++) {
                    suppressed[i] = 0;
                }
                // 3) 在剩余 bin 上同时找次峰和谷
                int valleyIdx = -1, valleyFreq = 2147483647;
                int peakIdx = -1, peakFreq = 0;
                for (int i = 0; i < nBins; i++) {
                    if (suppressed[i] > 0) {
                        if (suppressed[i] < valleyFreq) { valleyFreq = suppressed[i]; valleyIdx = i; }
                        if (suppressed[i] > peakFreq)   { peakFreq = suppressed[i];   peakIdx = i;   }
                    }
                }
                if (peakIdx >= 0 && valleyIdx >= 0) {
                    const double hBin = span / static_cast<double>(nBins);
                    cOut = minV + (peakIdx + 0.5) * hBin;
                    const double valleyVal = minV + (valleyIdx + 0.5) * hBin;
                    wOut = 2.0 * (cOut - valleyVal);
                } else {
                    cOut = (minV + maxV) * 0.5;
                    wOut = std::max(1.0, span);
                }
                break;
            }
            case 3: { // ADAPTIVE
                if (hist == nullptr || hist->empty() || nBins <= 0) {
                    cOut = (minV + maxV) * 0.5;
                    wOut = std::max(1.0, span);
                    return;
                }
                AdaptiveWindowResult aw;
                aw.hBins = span / static_cast<double>(nBins);
                computeAdaptiveWindow(*hist, nBins, 0.0015, 0.0015, aw);
                cOut = minV + aw.c;
                wOut = aw.w;
                break;
            }
            case 6: { // PEAK_AREA_AUTO (智能波峰面积识别)
                if (hist == nullptr || hist->empty() || nBins <= 0) {
                    cOut = (minV + maxV) * 0.5;
                    wOut = std::max(1.0, span);
                    return;
                }
                // 1) 高斯平滑直方图：去除细微噪声干扰，突出主要波峰
                std::vector<double> smooth;
                double sigma = 3.0;
                int radius = (int)round(3 * sigma);
                int kSize = 2 * radius + 1;
                std::vector<double> kernel(kSize);
                double sumK = 0.0;
                for (int i = 0; i < kSize; i++) {
                    int x = i - radius;
                    kernel[i] = exp(-0.5 * (x * x) / (sigma * sigma));
                    sumK += kernel[i];
                }
                for (int i = 0; i < kSize; i++) kernel[i] /= sumK;

                smooth.assign(nBins, 0.0);
                for (int i = 0; i < nBins; i++) {
                    double s = 0.0;
                    for (int j = -radius; j <= radius; j++) {
                        int idx = i + j;
                        if (idx < 0) idx = -idx - 1;
                        else if (idx >= nBins) idx = 2 * nBins - idx - 1;
                        s += (*hist)[idx] * kernel[j + radius];
                    }
                    smooth[i] = s;
                }

                // 2) 寻找波峰：识别面积（高度*宽度）最大的波峰
                struct Peak { int idx; double prod; };
                std::vector<Peak> peaks;
                for (int i = 1; i < nBins - 1; i++) {
                    if (smooth[i] > smooth[i - 1] && smooth[i] > smooth[i + 1]) {
                        double th = smooth[i] * 0.5;
                        int left = i, right = i;
                        while (left > 0 && smooth[left] > th) left--;
                        while (right < nBins - 1 && smooth[right] > th) right++;
                        peaks.push_back({i, smooth[i] * (right - left + 1)});
                    }
                }
                std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b){
                    return a.prod > b.prod;
                });

                // 3) 组织定位逻辑：
                // 如果检测到的最大峰位于直方图两端（背景区），则尝试切换到次大峰（组织区）
                int bestPeakIdx = 0;
                if (!peaks.empty()) {
                    bestPeakIdx = peaks[0].idx;
                    bool isLeftBackground = (bestPeakIdx < nBins * 0.20);
                    bool isRightBackground = (bestPeakIdx > nBins * 0.80);
                    if ((isLeftBackground || isRightBackground) && peaks.size() > 1) {
                        bestPeakIdx = peaks[1].idx;
                        LOGW("pickWindowCenterWidth: Background peak suppressed, using tissue peak at %d", bestPeakIdx);
                    }
                }

                // 4) 寻找边缘：在选定波峰周围确定有效显示范围
                double thE = smooth[bestPeakIdx] * 0.70;
                int minIdx = 0, maxIdx = nBins - 1;
                // ... (边缘寻找逻辑实现)
                int leftStart = -1;
                for (int i = bestPeakIdx - 1; i >= 0; i--) {
                    if (smooth[i] < thE) { leftStart = i; break; }
                }
                if (leftStart != -1) {
                    for (int i = leftStart - 1; i > 0; i--) {
                        double slope = (smooth[i + 1] - smooth[i - 1]) / 2.0;
                        if (slope < 10.0) { minIdx = i; break; }
                    }
                }
                int rightStart = -1;
                for (int i = bestPeakIdx + 1; i < nBins; i++) {
                    if (smooth[i] < thE) { rightStart = i; break; }
                }
                if (rightStart != -1) {
                    for (int i = rightStart + 1; i < nBins - 1; i++) {
                        double slope = (smooth[i + 1] - smooth[i - 1]) / 2.0;
                        if (slope > -10.0) { maxIdx = i; break; }
                    }
                }

                // 4) Map back to HU
                const double hBin = span / static_cast<double>(nBins);
                double edgeMin = minV + minIdx * hBin;
                double edgeMax = minV + (maxIdx + 1) * hBin;

                // 5) 临床窗宽保护 - 改进：降低最小窗宽以增强对比度
                double wc = (edgeMin + edgeMax) * 0.5;
                double ww = edgeMax - edgeMin;
                if (ww < 350.0) ww = 350.0; // 从 400.0 降低到 350.0，提高对比度

                cOut = wc;
                wOut = ww;
                break;
            }
            case 4: // HIST_TYPE：min/max 兜底
            default: {
                cOut = (minV + maxV) * 0.5;
                wOut = std::max(1.0, span);
                break;
            }
        }
        LOGI("pickWindowCenterWidth: method=%d -> C=%.1f, W=%.1f", method, cOut, wOut);
    }

    cv::Mat applyWindow8u(const cv::Mat &hu, double c, double w,
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
                if (invSpan <= 0.0) y = 127.5;
                else if (x <= lower) y = 0.0;
                else if (x >= upper) y = 255.0;
                else y = (x - lower) * invSpan;
                uint8_t v = static_cast<uint8_t>(y + 0.5);
                if (invert) v = static_cast<uint8_t>(255 - v);
                dst[xx] = v;
            }
        }
        return out;
    }

    cv::Mat applyDisplayClahe(const cv::Mat &gray8u, bool enable,
                             double clip, int tile) {
        if (!enable || gray8u.empty() || gray8u.type() != CV_8UC1) return gray8u;
        double c = (clip > 0.0) ? clip : 2.0;
        int t = (tile > 0) ? tile : 8;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(c, cv::Size(t, t));
        cv::Mat out;
        clahe->apply(gray8u, out);
        return out;
    }

}
