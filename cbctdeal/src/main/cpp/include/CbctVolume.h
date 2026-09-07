#ifndef DCMTKDEMO_CBCTVOLUME_H
#define DCMTKDEMO_CBCTVOLUME_H

#include <cstddef>
#include <string>

#include "dcmtk/config/osconfig.h"   // DCMTK 编译配置
#include "dcmtk/ofstd/oftypes.h"    // for Uint16

/**
 * CBCT 序列解析结果：Native 堆上的连续 3D 体数据。
 *
 * 内存布局为 [z][y][x] 的连续 Uint16 数组（Native 堆分配），
 * Java 层仅持有指针（jlong），按需提取切面，避免跨层大块拷贝。
 *
 * 结构由 CbctSeriesParser 装配，由 CbctJni（JNI 桥）读写，
 * 生命周期：loadSeries 创建 -> extract* 读取 -> releaseVolume 释放。
 */
struct CbctVolume {
    Uint16 *data = nullptr;          // 连续体数据 [depth][height][width]
    size_t sliceSize = 0;             // 单张切片像素数 = width * height
    int width = 0;                    // x 方向像素数（Columns）
    int height = 0;                   // y 方向像素数（Rows）
    int depth = 0;                    // z 方向切片数（含补全的空白切片）

    double spacingX = 1.0;           // 像素间距 mm（各向同性时与层厚相等）
    double spacingY = 1.0;
    double spacingZ = 1.0;           // 层厚/切片间距 mm

    double slope = 1.0;              // HU = pixel * slope + intercept
    double intercept = 0.0;
    int pixelRepresentation = 0;     // 0=unsigned, 1=signed

    double windowWidth = 4000.0;     // 默认窗宽窗位（缺失时用 CBCT 骨骼窗）
    double windowCenter = 600.0;

    double zMin = 0.0;               // 排序后首/末切片 Z 坐标
    double zMax = 0.0;

    int sliceCount = 0;              // 有效切片帧数（不含补全空白片）
    int skippedFiles = 0;           // 被过滤的非 DICOM / 损坏文件数
    long long elapsedMs = 0;        // 解析耗时

    // 患者与检查信息（取自第一个有效切片）
    std::string patientName;
    std::string patientID;
    std::string patientSex;
    std::string patientBirthDate;
    std::string studyDate;
    std::string modality;
    std::string manufacturer;

    ~CbctVolume();
};

#endif // DCMTKDEMO_CBCTVOLUME_H
