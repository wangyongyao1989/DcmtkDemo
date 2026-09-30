#ifndef DCMTKDEMO_MEASUREPICKER_H
#define DCMTKDEMO_MEASUREPICKER_H

#include <string>
#include <vector>

#include "include/MeasureTypes.h"
#include "include/VolumeRef.h"

/** 一次拾取的完整结果 */
struct PickResult {
    bool hit = false;
    Vec3 point;            // 命中点（射线与首个满足条件位置的交点，世界 mm）
    Vec3 voxelCenter;      // 命中体素的中心（用于"锚定到体素"的精确测量）
    int index[3] = {-1, -1, -1};
    float hu = 0.0f;
    double distanceMm = 0.0;   // 相机原点到命中点的距离（排序 / 拾取容错用）
    std::string describe;      // 人类可读描述（日志与 UI 提示）
};

/**
 * 拾取请求（PRD 5.1.3 / 8.3）。
 * mode 决定读取哪些字段，其余字段一律忽略：
 *   0 骨面拾取      -> huThreshold
 *   1 穿透骨拾取    -> huThreshold, skip
 *   2 HU 区间拾取   -> huLo, huHi（两者都 <=0 时由调用方回退到 dominantHuRange）
 *   3 MPR 平面拾取  -> plane, planePosition
 *   4 ROI 表面拾取  -> roiId
 *   5 神经管折线拾取-> nerveId, toleranceMm
 * 之所以做成结构体而不是"五个 int 参数按 mode 变义"：桥接层与 UI 都能按字段名
 * 传参，避免"穿透拾取的 skip 被当成 plane"这类只有真机才能暴露的错值。
 */
struct PickRequest {
    int mode = 0;
    double huThreshold = 200.0;
    int skip = 0;
    double huLo = 200.0;
    double huHi = 3000.0;
    int plane = MP_AXIAL;
    int planePosition = 0;
    int roiId = 0;
    int nerveId = 0;
    double toleranceMm = 2.0;
    /**
     * 深度窗口（仅骨面拾取用，单位 mm，沿射线自相机原点计）：只在 [tMinMm, tMaxMm]
     * 内找第一个骨面。两者都为 0 表示不限 —— 保持"射线上第一个骨面"的原语义。
     * 拖动描记（M-06 / A-05）用它把采样锁在上一采样的深度层附近，见 MeasurePicker::pickSurface。
     */
    double tMinMm = 0.0;
    double tMaxMm = 0.0;
};

/**
 * 射线拾取（PRD 5.1.3 / 8.3，风险 R-01 的缓解实现）。
 *
 * 为什么不用 vtkCellPicker：本模块不链接 VTK（避免同一进程内出现两份
 * VTK 单例，见 cpp/CMakeLists.txt 的依赖策略注释）。屏幕坐标 -> 世界射线
 * 由 :cbctdeal 暴露的 displayToRay()（内部用 vtkCoordinate 走 VTK 自己的
 * 逆投影，与 vtkWorldPointPicker 同源）给出，本类只负责"射线 <-> 几何"求交，
 * 因此精度与 VTK 的拾取一致，且可以完全离线单测。
 *
 * 步进策略：沿射线在体积包围盒内以 min(spacing)/2 步长行进，
 * 首次满足判定条件即命中 —— 最坏情况（贯穿整个 200mm 对角线）
 * 约 1.3k 步 × 三线性 8 次读 = 万级内存访问，远小于 PRD 的 50ms 预算。
 */
class MeasurePicker {
public:
    explicit MeasurePicker(const VolumeRef &vol);

    /** 射线与体数据包围盒求交（slab 法）；返回进入距离 t0 / 离开距离 t1 */
    bool rayVolumeBox(const Vec3 &origin, const Vec3 &dir, double &t0, double &t1) const;

    /**
     * 表面拾取：命中第一个 HU >= threshold 的体素（默认骨皮质 200HU）。
     * 这是"触摸屏幕点 -> 骨骼表面点"的入口，也是种植体入口点（S-01）的来源。
     *
     * tMinMm / tMaxMm 给出深度窗口（沿射线自 origin 计，0 表示该侧不限）：
     * 拖动描记时上一采样的深度 ±W 即"同一张骨面"，窗口外的命中（手指只挪几像素，
     * 命中点却从肋骨跳到其后 100~500mm 的椎体）会被判为 miss 而丢弃，
     * 累积弦长因此稳定贴合用户画的那条线。
     */
    PickResult pickSurface(const Vec3 &origin, const Vec3 &dir,
                           double huThreshold = 200.0, double maxMm = 0.0,
                           double tMinMm = 0.0) const;

    /**
     * 穿透拾取：命中射线上第 skip 个"由非骨进入骨"的界面（skip=0 即 pickSurface）。
     * 用于"点到骨小梁内部某一层"的选择，避免每次都要旋转视角换侧面。
     */
    PickResult pickThroughBone(const Vec3 &origin, const Vec3 &dir, int skip,
                               double huThreshold = 200.0) const;

    /** 等值面拾取：命中 HU 落在 [lo, hi] 的第一个体素（R-01 阈值预览用） */
    PickResult pickHuRange(const Vec3 &origin, const Vec3 &dir, double lo, double hi) const;

    /**
     * MPR 平面拾取：射线与"过第 position 层、法向为指定轴"的平面求交。
     * 交点若落在体数据矩形内则 hit=true，并吸附到该层（保证 z/y/x 分量精确）。
     */
    PickResult pickPlane(const Vec3 &origin, const Vec3 &dir, int plane, int position) const;

    /** ROI 几何拾取（R-02/R-03/R-04 的拖拽与选择）：命中 ROI 边界表面 */
    PickResult pickRoiSurface(const RoiDef &roi, const Vec3 &origin, const Vec3 &dir) const;

    /** 神经管折线拾取：射线到折线的最近距离 <= tolMm 视为命中第 index 段 */
    bool pickNervePoint(const NervePath &path, const Vec3 &origin, const Vec3 &dir,
                        double tolMm, int &pointIndex, Vec3 &closestOnRay) const;

    /** 把任意点吸附到最近的体素中心（测量基准锚定，消除拖动抖动） */
    Vec3 snapToVoxelCenter(const Vec3 &p) const;

    /** 由 PickResult 生成"点按即读数"的文本（HU + 组织 + 层位） */
    std::string describePick(const PickResult &r) const;

private:
    const VolumeRef &vol_;
};

#endif // DCMTKDEMO_MEASUREPICKER_H
