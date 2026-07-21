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

    cv::Mat applyWindow8u(const cv::Mat &hu, double c, double w,
                         int photometric);

    cv::Mat applyDisplayClahe(const cv::Mat &gray8u, bool enable,
                             double clip, int tile);

}

#endif //RAWPIXELDEAL_CTSERIESPROCESSOR_H
