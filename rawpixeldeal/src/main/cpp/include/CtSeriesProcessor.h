#ifndef RAWPIXELDEAL_CTSERIESPROCESSOR_H
#define RAWPIXELDEAL_CTSERIESPROCESSOR_H

#include <vector>
#include <opencv2/core.hpp>

namespace CtSeriesProcessor {

    struct AdaptiveWindowResult {
        double c = 0.0;
        double w = 1.0;
        double hBins = 1.0;
        double t0 = 0.0;
        double t1 = 0.0;
        int b = 0;
    };

    struct HistogramStats {
        double entropy;
        double maxBinFrac;
        int numPeaks;
    };

    cv::Mat toHu(const cv::Mat &sv, double slope, double intercept);

    cv::Mat optimizeHu(const cv::Mat &hu, bool enableBilateral,
                      int bilateralD, double sigmaColor, double sigmaSpace,
                      float clipLowHu, float clipHighHu);

    cv::Rect tryAutoCropBodyRoiEx(const cv::Mat &hu, float bodyThreshold,
                                 int morphSize, int minBodyAreaPx,
                                 int marginPx);

    void computePercentileHu(const std::vector<cv::Mat> &huSlices,
                            const cv::Rect &roi, int stride,
                            double pLow, double pHigh,
                            float &gMinOut, float &gMaxOut);

    bool computeSeriesGminGmax(const std::vector<cv::Mat> &huSlices,
                              const cv::Rect &roi, int stride,
                              float &gMinOut, float &gMaxOut);

    void aggregateSeriesHistogram(const std::vector<cv::Mat> &huSlices,
                                 const cv::Rect &roi,
                                 double gmin, double gmax, int nBins,
                                 int stride, std::vector<int> &histOut);

    HistogramStats computeHistogramStats(const std::vector<int> &hist);

    bool computeAdaptiveWindow(const std::vector<int> &histOrig,
                              int nBins, double n0, double n1,
                              AdaptiveWindowResult &out);

    void pickDefaultWindow(float gmin, float gmax, HistogramStats hs,
                          double fallbackC, double fallbackW,
                          double &cOut, double &wOut, bool &usedDefault);

    /**
     * 统一的"调窗方法 → (c, w)"分派。
     *
     * 以前 native-lib.cpp 的 native_processMedicalCT 与
     * MedicalCTPreprocess.cpp 的 CTTailorInvertWindowPipeline 各写了一份
     * DEFAULT/72/BIMODAL/ADAPTIVE/MIN_MAX/HIST_TYPE 实现，已出现细微差异。
     * 这里把所有分派集中到一处，调用方只需传 method index + min/max + 可选 hist。
     *
     * @param method      调窗方法索引：-1=百分位自适应，0=DEFAULT(127.5/255)，
     *                    1=CUMULATIVE_72, 2=BIMODAL, 3=ADAPTIVE,
     *                    4=HIST_TYPE(min/max 兜底), 5=MIN_MAX
     * @param minV/maxV   当前数据实际 min/max（HU 或 raw 16-bit）
     * @param hist        直方图（仅 method∈{1,2,3} 需要；其它可为 null）
     * @param nBins       直方图 bin 数
     * @param cOut/wOut   出参：窗位 / 窗宽
     */
    void pickWindowCenterWidth(int method, double minV, double maxV,
                               const std::vector<int> *hist, int nBins,
                               double &cOut, double &wOut);

    cv::Mat applyWindow8u(const cv::Mat &hu, double c, double w,
                         int photometric);

    cv::Mat applyDisplayClahe(const cv::Mat &gray8u, bool enable,
                             double clip, int tile);

}

#endif //RAWPIXELDEAL_CTSERIESPROCESSOR_H
