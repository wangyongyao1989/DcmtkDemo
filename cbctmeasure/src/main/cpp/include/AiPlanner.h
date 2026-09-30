#ifndef DCMTKDEMO_AIPLANNER_H
#define DCMTKDEMO_AIPLANNER_H

#include <string>
#include <vector>

#include "include/AiTypes.h"
#include "include/Json.h"
#include "include/MeasureTypes.h"
#include "include/VolumeRef.h"

/**
 * AI-03 种植位推荐的候选（PRD 5.6.3）。
 *
 * 一条候选 = "某个缺牙间隙可以放一颗植体"，字段分两组：
 *   推荐依据（ML + 规则）：相邻牙实例、间隙宽度、建议入口与轴向；
 *   安全评估（复用 S-02~S-06）：Implant 落库后由 ImplantPlanner 重算，
 *   因此这里的 boneHeightMm/level 等就是最终报告里的同一套数字，
 *   不存在"推荐时算一遍、落库后另一遍"的口径分裂。
 */
struct AiCandidate {
    bool valid = false;
    int arch = -1;                 // 0 = 上颌，1 = 下颌
    int beforeId = 0, afterId = 0; // 缺牙间隙两侧的 AI 实例号（0 = 牙列端点，无邻牙）
    double gapMm = 0.0;            // 邻牙之间的净间隙（减去两侧牙冠宽度）
    Vec3 entry;                    // 建议植入点（牙槽嵴顶，世界 mm）
    Vec3 axis;                     // 建议轴向（指向根方，单位向量）
    double pitchDeg = 0.0, yawDeg = 0.0;
    double diaMm = 4.0, lengthMm = 10.0, depthMm = 8.0;   // 建议规格
    double meanHuUnderGap = 0.0;   // 间隙下方骨小梁区的平均 HU（骨密度线索）
    int score = 0;                 // 0~100 规则打分，降序输出
    std::string reason;            // 中文理由（推荐依据 / 淘汰原因，进报告）

    // ---- 落库后由 ImplantPlanner 回填（与 Implant 同一口径）----
    double boneHeightMm = 0.0;
    double boneWidthMm = 0.0;
    double nerveDistMm = -1.0;
    int level = SAFE_GREEN;
    int implantId = 0;             // 采纳为 Implant 后回填（0 = 未采纳）
};

namespace AiPlanner {

    /**
     * 生成候选（AI-03 的"规则 + ML 混合"部分）。
     *
     * ML 成分：牙弓归属、每颗牙的质心/长轴（AI-01 掩膜的 PCA）都来自分割结果，
     *          没有分割就没有间隙检测 —— 这是"ML 提供解剖先验"的落点。
     * 规则成分：净间隙宽度阈值、邻牙长轴加权平均作为植入轴向、
     *          牙槽嵴顶高度估计、按骨宽/骨高选直径与长度、安全打分排序。
     *
     * @param r            AI-01 结果（必须 hasMask()，否则 out 清空并给原因）
     * @param vol          体数据（骨密度采样 + S-03/S-04 复算）
     * @param nerves       已勾画的神经管（S-05；为空则该指标不参与判级）
     * @param existing     方案里已有的种植体（用于避让重复推荐 + S-06）
     * @param minGapMm     最小可接受净间隙（PRD 未给数值，默认 5.0mm 并写进报告）
     * @param maxOut       最多输出几条（按分数降序截断，<=0 表示不限）
     */
    void recommend(const AiResult &r, const VolumeRef &vol,
                   const std::vector<NervePath> &nerves,
                   const std::vector<Implant> &existing,
                   double minGapMm, int maxOut,
                   std::vector<AiCandidate> &out);

    /** 候选 -> Implant（名称/颜色/参数），交给 MeasurementManager::addImplant 落库 */
    void toImplant(const AiCandidate &c, int index, Implant &out);

    /** 由单位轴向反解 pitch/yaw（ImplantPlanner::axisFromAngles 的逆运算） */
    void anglesFromAxis(const Vec3 &axis, double &pitchDeg, double &yawDeg);

    /** 候选表序列化（JNI -> Kotlin -> 报告/存档） */
    Json candidatesJson(const std::vector<AiCandidate> &list);
    std::string summaryText(const std::vector<AiCandidate> &list);
}

#endif // DCMTKDEMO_AIPLANNER_H
