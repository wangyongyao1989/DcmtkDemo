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
    /**
     * P-09 的修复导向（crown-down）偏差：S-11 水平偏移、S-12 成角。
     * crownMeasured=false 表示没有冠方参考，此时两个数值都是 0，
     * 调用方（报告/SR/UI）必须按"未测量"处理而不是当成 0 偏差。
     * 它们**不进 safetyLevel()**：PRD 未给阈值（见 warningText 的说明行）。
     */
    double crownOffsetMm = 0.0;
    double crownAngleDeg = 0.0;
    double crownAlongAxisMm = 0.0;
    bool crownMeasured = false;
    bool crownAxisFromReference = false;
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

    /**
     * S-06 与相邻种植体的最短表面间距（mm）= 轴线间距 - 两半径；无邻居返回 -1。
     *
     * found 非空时写入"到底有没有邻居"（D-10 修复）：负值间距 = 两植体表面重叠，
     * 是一个**真实测量结果**，必须参与判级；而"-1 = 没有邻居"是未测量哨兵。
     * 两者都用 -1.0 表达会让调用方无法区分，因此判级请读 found，不要读返回值符号。
     */
    double minSpacing(const Implant &implant, const std::vector<Implant> &all,
                      bool *found = nullptr) const;

    /**
     * S-11 / S-12 修复导向（crown-down）偏差（升级建议 P-09）。
     *
     * 定义（都相对植体**无限长轴线**，不用线段，因为冠中心天然在入口的冠方、
     * 落在线段夹范围之外，用点到线段距离会退化成"到入口点的距离"）：
     *   S-11 水平偏差 = 冠中心到轴线的垂直距离（MeasureMath::pointToLine）
     *   S-12 成角偏差 = 植体轴与"冠长轴"的夹角（度）
     *     - crownAxis 非零（A-02/A-03 这类两点标注）：两轴夹角折到 [0, 90]，
     *       因为标注线没有方向语义（首末点谁指向冠方不可知）；
     *     - crownAxis 为零（球面 ROI / 单点标注）：没有冠长轴可用，
     *       退化为"植体轴 ↔ 入口到冠中心连线"的夹角，此时结果带
     *       crownAxisFromReference=false，报告与 UI 必须写出这一点。
     *   两支都把夹角折到 [0,90]：axis 指向根方、冠方参考指向冠方，本来就近似
     *   反向，不折叠的话完全对齐的情况会读成 180°（组14 用例 g2 守着这条）。
     * 另外给出 crownAlongAxisMm = (冠中心-入口) 在轴向上的投影（冠方为负），
     * 它和 S-11 一起能定位偏差朝哪个方向，不新增阈值、不参与判级。
     *
     * 返回 false 表示没有冠方参考（crownCenter 为零向量时同样视为无参考，
     * 因为世界原点恰好是首体素中心，把"没设参考"和"参考在原点"混在一起会
     * 让 S-11 凭空多出一个十几毫米的假偏差）。
     */
    static bool crownDeviation(const Implant &implant, const Vec3 &crownCenter,
                               const Vec3 &crownAxis, double *offsetMm, double *angleDeg,
                               double *alongAxisMm = nullptr, bool *axisFromReference = nullptr);

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
