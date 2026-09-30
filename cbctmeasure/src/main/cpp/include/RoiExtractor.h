#ifndef DCMTKDEMO_ROIEXTRACTOR_H
#define DCMTKDEMO_ROIEXTRACTOR_H

#include <string>
#include <vector>

#include "include/MeasureTypes.h"
#include "include/VolumeRef.h"

/**
 * ROI 统计结果（R-01 ~ R-05 的量化输出，同时服务 M-04 体积与 M-08 骨密度）。
 *
 * coverageVoxels 是"分数覆盖"体素数（见 RoiExtractor 注释），不是整数体素个数，
 * 因此 volumeMm3 = coverageVoxels * 单个体素体积 可满足 M-04 的 <= 1% 误差。
 */
struct RoiStats {
    bool ok = false;
    std::string error;

    double coverageVoxels = 0.0;   // Σ 体素覆盖权重（0~1 之间连续）
    long long fullVoxels = 0;      // 权重 == 1 的体素数（诊断用）
    long long scannedVoxels = 0;   // 实际遍历的候选体素数
    long long elapsedMs = 0;       // 本次统计耗时（性能 NFR 取证）

    double voxelMm3 = 0.0;
    double volumeMm3 = 0.0;
    double volumeCm3 = 0.0;

    double meanHu = 0.0;
    double sdHu = 0.0;             // 总体标准差（按覆盖权重加权）
    double minHu = 0.0;
    double maxHu = 0.0;
    bool hasHu = false;

    double areaMm2 = 0.0;          // 截面 ROI（多边形）面积；非截面为 0
    double areaCm2 = 0.0;
};

/**
 * ROI 提取与体数据统计（PRD 5.2）。
 *
 * 体素权重模型（M-04 精度设计的关键）：
 *   weight = 空间几何权重 × HU 阈值权重
 *   - 空间几何（Box/Plane/Sphere）：对体素立方体的 8 个角点做包含测试，
 *     取"角点在区域内的比例"作为该体素的覆盖权重（等效 2x2x2 超采样）。
 *     纯整数计数在球面/平面边界处误差可达 3%（半径 5mm、体素 0.3mm 时），
 *     不满足 PRD 的 <= 1%；角点加权后按中心极限估计降到 ~0.2%。
 *   - HU 阈值：按体素中心 HU 判定（部分容积效应属于数据本身，
 *     不做亚体素猜测），与 R-01 的"复用不透明度传递函数"语义一致。
 *   - 组合 ROI（R-05）：权重按模糊逻辑合并 ——
 *     交集 min(a,b)、并集 max(a,b)、差集 min(a, 1-b)，
 *     与逐体素"AND / OR / NOT" 在权重 0/1 时完全等价。
 *
 * 遍历范围只覆盖 ROI 的空间包围盒（R-02/R-04 给出的包围盒或全体积），
 * 纯 HU 阈值 ROI（R-01 无空间约束）必须全体积扫描，耗时随体素数线性增长；
 * UI 侧交互预览应搭配空间裁剪盒，显式"重新统计"才允许全体积扫描。
 */
class RoiExtractor {
public:
    explicit RoiExtractor(const VolumeRef &vol);

    /**
     * 统计一个 ROI。pool 用于解析组合 ROI 的子 ROI（可为空，
     * 找不到子项时该项按"不限制"处理并在 error 里给出提示）。
     */
    RoiStats analyze(const RoiDef &roi, const std::vector<RoiDef> *pool = nullptr) const;

    /**
     * 只算截面面积（M-05）：直接用多边形解析面积，不做体素计数。
     * 返回 mm²，同时把质心写入 centroid（可为空）。
     */
    double polygonAreaMm2(const RoiDef &roi, Vec3 *centroid = nullptr) const;

    /** 单点是否落在 ROI 内（拾取高亮用；等价于 weightOf(...) > 0） */
    bool contains(const RoiDef &roi, const Vec3 &world,
                  const std::vector<RoiDef> *pool = nullptr) const;

    /** 体素 (i,j,k) 的覆盖权重，0 表示完全不在 ROI 内 */
    double weightOfVoxel(const RoiDef &roi, int i, int j, int k,
                         const std::vector<RoiDef> *pool,
                         std::string *error) const;

    /**
     * 提取空间包围盒（体素索引，闭区间）。
     * 供叠加层画裁剪盒线框、以及"隔离显示"时限定纹理更新范围。
     * 返回值：
     *   SB_SPATIAL     - 有空间约束，区间有效（Box/Sphere/Plane/Composite）
     *   SB_FULL_VOLUME - 无空间约束（R-01 纯阈值），调用方必须全体积扫描
     *   SB_EMPTY       - 与体数据完全无交集，可直接判定结果为空
     */
    enum BoundsResult {
        SB_EMPTY = -1,
        SB_FULL_VOLUME = 0,
        SB_SPATIAL = 1
    };

    int spatialBounds(const RoiDef &roi, const std::vector<RoiDef> *pool,
                      int &i0, int &j0, int &k0, int &i1, int &j1, int &k1) const;

private:
    double geometricWeight(const RoiDef &roi, int i, int j, int k,
                           const std::vector<RoiDef> *pool, std::string *error) const;

    /**
     * spatialBounds 的实现体：depth = 组合 ROI 的递归层数。
     * 公开签名保持不变，递归只在文件内部进行（子 ROI 的 childA/childB 可能
     * 构成环引用，必须由 depth 上限拦住，否则 analyze() 直接栈溢出崩溃）。
     */
    int spatialBoundsAt(const RoiDef &roi, const std::vector<RoiDef> *pool,
                        int &i0, int &j0, int &k0, int &i1, int &j1, int &k1, int depth) const;

    const VolumeRef &vol_;
};

#endif // DCMTKDEMO_ROIEXTRACTOR_H
