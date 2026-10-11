#ifndef DCMTKDEMO_NERVEAUTOTRACKER_H
#define DCMTKDEMO_NERVEAUTOTRACKER_H

#include <string>
#include <vector>

#include "include/AiTypes.h"
#include "include/Json.h"
#include "include/MeasureTypes.h"

/**
 * AI-02：把"下颌管掩膜"变成"神经管中心线"（PRD 5.6 AI-02 / 建议需求 P-01）。
 *
 * 为什么单独一个模块，而不是塞进 AiCore::connectedComponents：
 *   connectedComponents 产出的是**实例统计**（体积/质心/包围盒/长轴），
 *   它服务的语义是"一颗牙"；而 S-05 要的是一条**可判级的折线**
 *   （ImplantPlanner::nerveDistance 走 MeasureMath::polylineSegmentDistance）。
 *   折线不是长轴：下颌管在 1.2mm 网格上是一条沿前后向延伸、带颊舌向弯曲的管道，
 *   把 PCA 主轴当"中心线"会得到一条直线，牙弓后段的弯曲完全丢失 ——
 *   那种直线在磨牙后区能偏离真实管腔 8mm 以上，直接把安全距离算错。
 *   所以这里做的是"沿最长伸展轴逐层取截面质心"的追踪，不是主轴拟合。
 *
 * 规则与主机侧 scripts/canal.py 的 centerlines() 逐条对应（同源、可交叉验证）：
 *   a) 26 邻域连通域；体素数 < minVoxels 的散点丢弃（不进会话）；
 *   b) 候选按 (体素数降序, 最小线性索引升序) 排序后取前 maxCandidates 个
 *      —— 这个次键是为了让"体素数相同的两块"在主机与真机给出同一顺序，
 *      否则 std::sort 与 scipy 的并列顺序不同，叠加层配色和报告行序会漂；
 *   c) 描记轴 = 该连通域 bbox 跨度最大的轴（并列按 x<y<z 取第一个）。
 *      **不能写死 z**：下颌管的主要伸展方向是前后向（实测 bbox 跨度 42~48 体素），
 *      而 z 只有 26~33；选错轴会得到一堆"每层一个点"的横向抖动折线；
 *   d) 沿描记轴按体素坐标分组，每组取该组体素索引的算术平均，
 *      再按 redToWorld 的同一 convention（(index+0.5)*spacing）转世界毫米
 *      —— 与 AiCore 的质心/统计用同一个坐标函数，保证 S-05 与 S-06 的
 *      输入点在同一坐标系里；
 *   e) 半径 = 各组"截面积 sqrt(A/pi)"的中位数，A = 组内体素数 × 垂直面体素面积。
 *      中位数而不是均值：断裂处与部分容积效应会让少数截面明显偏大，
 *      均值会被它们拉高，而半径直接进入 nerveDistance 的减项（安全判级），
 *      宁可保守取中位数。
 *
 * 坐标系约定与 include/AiTypes.h 顶部一致：模型网格轴序是设备轴序
 * （axis0=Columns=x，axis1=Rows=y，axis2=切片方向=z），
 * 因此本模块输出的 Vec3 可以直接和手描神经管点、种植体轴线点混用。
 */

/** 一条追踪结果（还没变成 NervePath 之前的中间量，取证与转换都用它） */
struct NerveTrack {
    long long voxels = 0;
    int traceAxis = 0;                 // 0=x 1=y 2=z
    double extentVox[3] = {0, 0, 0};   // bbox 体素跨度（hi-lo+1），解释 c) 的选轴依据
    std::vector<Vec3> points;          // 世界毫米，按描记轴坐标升序
    double radiusMm = 0.0;             // e) 的中位数截面积半径
    Vec3 centroid;                     // 全体体素球心均值（报告里区分左右/前后用）
    long long minLinearIndex = 0;      // b) 的次键
    int groupsUsed = 0;                // 折线点数 == 分组数，单独留一份便于取证
};

namespace NerveAutoTracker {

    /**
     * 掩膜 -> 中心线。minVoxels<=0 时取 AiConst::MIN_CANAL_VOXELS，
     * maxCandidates<=0 时取 AiConst::CANAL_MAX_CANDIDATES。
     * 结果已按 b) 排序并截断；无掩膜时返回空表（不报错，调用方按"没追出东西"处理）。
     */
    std::vector<NerveTrack> track(const AiResult &r,
                                  int minVoxels = 0, int maxCandidates = 0);

    /** 追踪结果 -> NervePath（name 前缀 = AiConst::CANAL_AUTO_NOTE，color 用 CANAL_COLOR） */
    std::vector<NervePath> toPaths(const std::vector<NerveTrack> &tracks);

    /** 逐条点的 JSON（AC-08-2 取证：主机脚本与 canal.py 的 centerlines 逐点比对） */
    Json tracksJson(const std::vector<NerveTrack> &tracks);

    /** 一行摘要（UI 状态栏 / 报告 / PDF 引用，口径与 AiCore::summaryText 一致） */
    std::string summaryText(const std::vector<NerveTrack> &tracks,
                            double thresholdUsed, bool measuredRadius);

    /** 折线点数上限：单条管腔的分组数就是该轴体素跨度（1.2mm 网格 <=96），
     *  但叠加层与 SR 都有图元预算，超过时按等弧长抽稀（保留首末点）。 */
    static const int MAX_TRACK_POINTS = 128;

}   // namespace NerveAutoTracker

#endif // DCMTKDEMO_NERVEAUTOTRACKER_H
