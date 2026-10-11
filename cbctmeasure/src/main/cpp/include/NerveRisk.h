#ifndef DCMTKDEMO_NERVERISK_H
#define DCMTKDEMO_NERVERISK_H

#include <string>
#include <vector>

#include "include/AiTypes.h"
#include "include/Json.h"
#include "include/MeasureTypes.h"
#include "include/VolumeRef.h"

/**
 * 第三磨牙 ↔ 下牙槽神经（下颌管）风险度量（升级建议 P-10 的 MVP）。
 *
 * 为什么**不**复用 S-05：
 *   S-05 的语义是"种植体轴线到神经管的最短距离"，服务对象是植入点判级，
 *   阈值 2mm/临界 1.2 倍都是 PRD 5.3.2 为**种植**写的。阻生第三磨牙的牙根
 *   是一个**有体积的掩膜**而不是一条轴线，且临床上真正要问的是
 *   "拔除时神经会不会被刮到"，与"植入时会不会顶到"不是同一个问题
 *   （外部依据：皮质骨缺损是下颌管与第三磨牙接触后的独立危险因素，
 *    见 doc/CBCT_APPLICATION_UPGRADE_PROPOSALS.md 的 P-10 引文）。
 *   所以这里产出**独立的风险条目**，只给几何事实，不改写 S-05、
 *   也不参与 S-02 的红黄绿判级（用户决策点 4：PRD 没给数字就不自定阈值）。
 *
 * 输出的是三类事实，都能被复核：
 *   1) minDistMm：牙根掩膜体素 ↔ 神经折线的最小距离（点到折线，逐段取最小）；
 *      contact 为 true 表示折线穿过掩膜（存在某个折线顶点落在该实例的体素里）。
 *   2) withinCanalRadius：minDistMm <= 神经管显示半径 —— 这是"根面已进入管腔
 *      占据的空间"的几何陈述，不是判级。
 *   3) 管壁区间里的皮质骨取样：沿"最近管点 → 最近根面点"这条射线，
 *      跳过管腔半径以内的部分，统计剩余区间里骨体素（HU >= 200，
 *      PRD R-01 同源阈值，**不是新阈值**）的连续缺失长度。
 *      cortexPresent=false 只是"这段射线上没采到骨"，不写成"皮质骨缺损"的结论。
 *
 * 剂量/FOV（P-10 明确要求带来源）：CBCT 的可重复性随剂量方案变化，
 * 所以每条风险都写清体数据的 FOV 与体素间距。曝光参数（KVP/mAs/剂量）
 * 在本项目解析的 CbctVolume 里**没有对应字段**，因此 doseText 一律如实写
 * "未记录"，绝不留空让人以为"已核对无异常"。
 */
struct ThirdMolarRisk {
    int id = 0;
    int instanceId = 0;              // AI-01 实例号（牙根掩膜来源）
    int nervePathId = 0;             // 神经管路径号（手描或 AI-02 自动）
    std::string toothName;
    std::string nerveName;
    std::string nerveSource;         // "手工描记" / "AI-02 自动下颌管"
    long long createdAt = 0;         // epoch millis，报告按时间排序用

    bool measured = false;           // false = 没有可用输入（无掩膜/折线点数不足）
    double minDistMm = -1.0;         // -1 = 未测量（读 measured，不要看符号）
    /**
     * 口径说明：minDistMm 是「掩膜**抽稀体素球心** ↔ 神经折线」的最小距离，
     * 不是「牙根表面 ↔ 折线」。因此它与真实表面距离的偏差上界是
     * 一个抽稀体素的对角半长（factor=1、spacing=0.4mm 时约 0.35mm）。
     * 之所以不取表面：表面要在每个体素邻域做梯度判定，成本随掩膜体素数线性
     * 上升且对部分容积效应敏感；球心口径可复核、可主机侧逐值单测。
     * UI 与报告的措辞一律写"根面(按体素球心)最近距离"，不写成表面距离。
     */
    bool contact = false;
    bool withinCanalRadius = false;
    Vec3 nearestRoot;                // 掩膜侧最近点（世界 mm）
    Vec3 nearestCanal;               // 神经侧最近点（世界 mm）
    long long rootVoxels = 0;        // 参与计算的掩膜体素数（取证：0 = 实例不在结果里）
    double canalRadiusMm = 0.0;      // 当时用的管道半径，换半径后这条记录要重算

    bool cortexMeasured = false;
    bool cortexPresent = false;
    double cortexGapMm = 0.0;        // 管壁区间里最长的"连续非骨段"长度
    double cortexBoneThresholdHu = 200.0;   // 写明取样口径（PRD R-01）

    // ---- 来源可复核性 ----
    double fovXMm = 0.0, fovYMm = 0.0, fovZMm = 0.0;
    double spacingX = 0.0, spacingY = 0.0, spacingZ = 0.0;
    std::string doseText;            // 见类注释：本序列没这些字段时如实写"未记录"
    std::string seriesUid;
    std::string manufacturer;

    std::string error;               // 未测量时的原因（UI 直接显示）

    /** 一行结论（PDF/SR/UI 共用同一措辞，避免三处各写一套） */
    std::string summaryText() const;
};

namespace NerveRisk {

    /**
     * 计算一次风险度量。
     *
     * 输入约束与失败口径（都不抛异常，返回 measured=false + error）：
     *   - ai 没有掩膜 / instanceId 不在 ai.instances 里；
     *   - nerve.points.size() < 2（一个点连不成"管"）；
     *   - 体数据未绑定（HU 取样与 FOV 都拿不到）。
     * 复杂度：只扫描该实例的包围盒区域，并用"折线 AABB 下界"做剪枝，
     * 因此一颗磨牙（数千体素）的量级在真机上远低于一次 R-02 体积分析。
     */
    ThirdMolarRisk assess(const VolumeRef &vol, const AiResult &ai, int instanceId,
                          const NervePath &nerve, const std::string &nerveSource);

    Json toJson(const ThirdMolarRisk &r);

    /** 字段缺失时保持结构体默认值（旧归档没有 P-10 条目，读出即 measured=false） */
    void fromJson(const Json &j, ThirdMolarRisk &out);

    /** 列表 -> JSON 数组（plan.json 的 nerveRisks 键） */
    Json listJson(const std::vector<ThirdMolarRisk> &list);

    /** 一条判读提示：告诉读者这些数字**没有**判级口径 */
    const char *gradingCaveat();
}

#endif // DCMTKDEMO_NERVERISK_H
