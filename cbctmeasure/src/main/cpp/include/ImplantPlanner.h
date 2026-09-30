#ifndef DCMTKDEMO_IMPLANTPLANNER_H
#define DCMTKDEMO_IMPLANTPLANNER_H

#include <string>
#include <vector>

#include "include/MeasureTypes.h"
#include "include/VolumeRef.h"

/** 单颗种植体的完整安全评估结果（S-02 ~ S-06 汇总） */
struct ImplantSafety {
    double boneHeightMm = 0.0;     // S-03
    double boneWidthMm = 0.0;      // S-04（两个正交方向里的较小值，保守）
    double boneWidthAlongU = 0.0;  // 明细：方向 u 的骨量宽度
    double boneWidthAlongV = 0.0;  // 明细：方向 v 的骨量宽度
    double nerveDistMm = -1.0;     // S-05，-1 = 未勾画神经管（不参与判级）
    double spacingMm = -1.0;       // S-06，-1 = 口腔内无其它种植体
    int level = SAFE_GREEN;        // S-02
    std::string warnText;          // 中文告警摘要
    bool nerveMeasured = false;
    bool spacingMeasured = false;
};

/**
 * 种植体规划算法（PRD 5.3.2 S-01 ~ S-06）。
 *
 * 几何定义：
 *   轴线 = entry -> tip，tip = entry + axis * lengthMm，
 *   axis 由 pitchDeg（绕 X）与 yawDeg（绕 Y）从"轴向根方 -Z"旋转得到，
 *   即默认姿态 pitch=yaw=0 时桩体沿 -Z 方向深入（下颌骨 CBCT 的解剖朝向）。
 *
 * 取样的体素步长：min(spacingX, spacingY, spacingZ) / 2，
 * 用亚体素步长积分长度，使 S-03 骨高度的量化误差 <= 半步长（<0.15mm）。
 *
 * "骨"的判定阈值默认 200HU（PRD R-01 骨分割 >200HU），
 * 与 :cbctdeal 的 VR 骨骼不透明度起点同源。
 */
class ImplantPlanner {
public:
    /** PRD 5.3.2 表中的安全阈值（S-02 颜色语义的唯一来源） */
    static const double kMinBoneHeightMm;    // 10.0
    static const double kMinBoneWidthMm;     // 6.0
    static const double kMinNerveDistMm;     // 2.0
    static const double kMinSpacingMm;       // 3.0
    /** 临界区间上界系数：[阈值, 阈值*kMarginalFactor) 判黄 */
    static const double kMarginalFactor;     // 1.2

    explicit ImplantPlanner(const VolumeRef &vol, double boneHuThreshold = 200.0);

    /** 由 pitch/yaw 计算单位轴向（右手系，绕世界 X 再绕世界 Y） */
    static Vec3 axisFromAngles(double pitchDeg, double yawDeg);

    /** 刷新 tip（entry/角度/长度变化后必须调用） */
    static void refreshGeometry(Implant &implant);

    /** S-03 沿轴可用骨高度（mm）；outSurfaceDepth 非空时返回入口到首个骨面的深度 */
    double boneHeight(const Implant &implant, double *outSurfaceDepth = nullptr) const;

    /** S-04 植入处颊舌向骨宽度（mm）：取两正交方向骨量跨度的较小值 */
    double boneWidth(const Implant &implant, double *outU = nullptr, double *outV = nullptr) const;

    /**
     * S-05 桩体到神经管的最短距离（mm）。
     * 用"轴线段 ↔ 神经管折线段"的整体最小距离，比 PRD 字面的"尖端到管道"
     * 更严格（尖端距离是它的一个下界情形），避免斜向植入时低估风险。
     * 返回 -1 表示神经管点数不足。
     */
    double nerveDistance(const Implant &implant, const NervePath &nerve) const;

    /** S-06 与相邻种植体的最短表面间距（mm）= 轴线间距 - 两半径；无邻居返回 -1 */
    double minSpacing(const Implant &implant, const std::vector<Implant> &all) const;

    /**
     * 汇总评估并回填 implant 的计算字段（boneHeightMm / level / warnText 等）。
     * nerves 为空 -> 不参与判级；others 为本方案全部种植体（含自身，内部会跳过）。
     */
    ImplantSafety evaluate(Implant &implant,
                           const std::vector<NervePath> &nerves,
                           const std::vector<Implant> &all) const;

    /** 安全等级判定（供单测直接调用；measured=false 的指标跳过） */
    static int safetyLevel(double boneHeightMm, double boneWidthMm,
                           double nerveDistMm, bool nerveMeasured,
                           double spacingMm, bool spacingMeasured);

    /** 中文告警文案（S-02；报告与 UI 共用同一措辞） */
    static std::string warningText(const ImplantSafety &s);

    /**
     * 圆柱轮廓顶点（叠加层 OK_CAPSULE 消费：world 依次为近端口 ring0、远端口 ring1）。
     * 与 vtkCylinderSource 的替代实现：半径 = dia/2，采样 segments 条母线。
     */
    static void capsuleOutline(const Implant &implant, int segments,
                               std::vector<Vec3> &out);

    /** 种植体螺纹示意线（沿轴的若干圈；segments/rings 控制在 UI 可承受的点数量级） */
    static void threadOutline(const Implant &implant, int turns, int pointsPerTurn,
                              std::vector<Vec3> &out);

private:
    /** 沿方向从 q 出发累计连续骨段长度（mm）；allowGap 允许跨过的非骨间隙（步数） */
    double marchBone(const Vec3 &q, const Vec3 &dir, double maxMm, int allowGapSteps) const;

    const VolumeRef &vol_;
    double boneHu_;
};

#endif // DCMTKDEMO_IMPLANTPLANNER_H
