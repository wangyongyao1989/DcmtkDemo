#ifndef DCMTKDEMO_CbctSeriesParser_H
#define DCMTKDEMO_CbctSeriesParser_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "CbctVolume.h"

/**
 * CBCT DICOM 序列解析引擎（纯 C++，不依赖 JNI）。
 *
 * 解析流程（对应《Android平台基于DCMTK的CBCT DICOM序列解析技术》方案）：
 *  1. 扫描目录，过滤非 DICOM 文件；
 *  2. Pass A：多线程读取每张切片的元数据（ImagePositionPatient 等）；
 *  3. 按 ImagePositionPatient[2]（Z 轴）升序排序——严禁按文件名排序；
 *  4. 校验切片连续性，Z 间隙超过层厚的位置自动补空白切片；
 *  5. Pass B：多线程读取 16bit 像素（JPEG / JPEG-LS 压缩自动解压），
 *     按 Z 索引写入 Native 堆连续 Volume；
 *  6. 输出 Volume 指针 + 全局元数据，供 MPR 切面提取。
 */
class CbctSeriesParser {
public:
    /// 进度回调：current 已完成数，total 总数（在解析线程池中回调，需线程安全）
    typedef std::function<void(size_t current, size_t total)> ProgressFn;

    /// MPR 平面
    enum MprPlane {
        PLANE_CORONAL = 0,    // 冠状面（固定 Y）
        PLANE_SAGITTAL = 1,   // 矢状面（固定 X）
    };

    /**
     * 解析目录下的 CBCT DICOM 序列，组装为 Volume。
     * 失败返回 nullptr 并填充 err。
     */
    static CbctVolume *loadSeries(const std::string &dir,
                                  const ProgressFn &progress,
                                  std::string &err);

    /// 释放 Volume（等价 delete）
    static void release(CbctVolume *vol);

    /// 提取横断面（Axial，固定 Z=zIndex），窗宽窗位映射为 8bit 灰度展开为 RGBA。
    static bool extractAxial(const CbctVolume *vol, int zIndex,
                             double ww, double wc,
                             std::vector<uint8_t> &outRgba, int &outW, int &outH);

    /// 提取 MPR 切面（冠状/矢状，固定 position 像素坐标），输出图像宽=vol 深度方向。
    static bool extractMpr(const CbctVolume *vol, MprPlane plane, int position,
                          double ww, double wc,
                          std::vector<uint8_t> &outRgba, int &outW, int &outH);

private:
    /// DICOM 窗宽窗位线性映射（PS3.3 C.11.2.1.2）：HU -> 0..255
    static uint8_t applyWindow(double hu, double wc, double ww);

    /// 原始像素 -> HU（含符号位与 Rescale 斜率截距）
    static double toHu(const CbctVolume *vol, Uint16 raw);
};

#endif // DCMTKDEMO_CbctSeriesParser_H
