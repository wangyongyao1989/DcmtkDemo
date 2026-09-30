#ifndef DCMTKDEMO_MEASUREMATH_H
#define DCMTKDEMO_MEASUREMATH_H

#include <string>
#include <vector>

#include "include/MeasureTypes.h"
#include "include/VolumeRef.h"

/**
 * 纯几何 / 纯统计测量算法（PRD 5.1 M-01 ~ M-08、5.3 S-07 ~ S-10）。
 *
 * 全部函数不触碰 Volume 以外的状态，可主机侧编译并单测（src/host/），
 * 这是 PRD 非功能需求"距离 <= 0.1mm / 角度 <= 0.5 度"的验证入口。
 *
 * 单位约定：输入点一律为世界毫米；长度返回 mm、角度返回度、
 * 面积返回 mm²、体积返回 mm³。cm² / cm³ 的换算只出现在
 * MeasurementManager 组装 MeasureRecord 时，避免两层各除一次 1000。
 */
namespace MeasureMath {

    /** 角度换算（不依赖 M_PI 宏，避免平台 math 定义差异） */
    double radToDeg(double rad);
    double degToRad(double deg);

    /** M-01 两点欧氏距离（mm） */
    double distance(const Vec3 &a, const Vec3 &b);

    /**
     * M-02 三点夹角 ∠ABC（度），B 为顶点。
     * 用 atan2(|a×b|, a·b) 而不是 acos(a·b)：
     * acos 在 0°/180° 附近导数发散，共线点会放大到 1e-8 量级的坐标误差；
     * atan2 形式在全角度域内条件数良好，满足 <= 0.5 度的精度要求。
     * 两向量任一为零长度时返回 0（调用方按"输入不足"处理）。
     */
    double angle(const Vec3 &a, const Vec3 &vertex, const Vec3 &c);

    /** M-03 点 P 到线段 AB 的垂直距离（mm）；foot 可为空，非空时返回垂足 */
    double pointToSegment(const Vec3 &p, const Vec3 &a, const Vec3 &b, Vec3 *foot = nullptr);

    /** M-03 变体：点到无限直线的距离（垂足不在线段内时仍按直线量） */
    double pointToLine(const Vec3 &p, const Vec3 &a, const Vec3 &b, Vec3 *foot = nullptr);

    /**
     * M-06 弧线长度（mm）——累积弦长法（PRD 5.3.5 指定）。
     * closed=true 时自动补上末点->首点那段闭合弦。
     */
    double arcLength(const std::vector<Vec3> &pts, bool closed = false);

    /**
     * M-05 截面面积（mm²）——Newell 法求多边形真实法向，
     * 再把各点投影到法向基 (u,v) 上做鞋带公式。
     * 支持非平面多边形（MPR 描记点常因层厚而略微离开切面），
     * 投影到最佳拟合平面比强行按固定轴投影误差小一个量级。
     */
    double polygonArea(const std::vector<Vec3> &pts);

    /** 多边形质心（Newell 法向同源的面积加权质心；退化时返回顶点均值） */
    Vec3 polygonCentroid(const std::vector<Vec3> &pts);

    /** S-08 牙齿排列角度偏差：两条方向线的夹角，折到 [0, 90] 度 */
    double lineDeviationDeg(const Vec3 &l1a, const Vec3 &l1b, const Vec3 &l2a, const Vec3 &l2b);

    /**
     * 两线段最短距离（S-05 神经管距离、S-06 种植体间距的公共底座）。
     * 采用 Eberly 的标准算法并在参数域 [0,1] 上夹紧，退化（零长线段）
     * 自动降为点-线距离。outA/outB 非空时返回最近点对。
     */
    double segmentSegment(const Vec3 &p0, const Vec3 &p1, const Vec3 &q0, const Vec3 &q1,
                          Vec3 *outA = nullptr, Vec3 *outB = nullptr);

    /** 折线与线段的最短距离（逐段取最小；点数 < 2 返回 -1 表示无效） */
    double polylineSegmentDistance(const std::vector<Vec3> &poly, const Vec3 &a, const Vec3 &b);

    /** 折线与折线的最短距离（S-06 用：两条种植体轴线） */
    double polylinePolylineDistance(const std::vector<Vec3> &pa, const std::vector<Vec3> &pb);

    /** 折线"按弧长取等距点"的中点位置（S-09 中线点定义：弧长 50% 处） */
    Vec3 pointAtArcFraction(const std::vector<Vec3> &pts, double fraction);

    /**
     * S-09 中线偏移（mm）：取上下牙弓弧线各自的中点（弧长 50% 处），
     * 返回两点在水平面（XY）内的距离——垂直分量属于覆合，不计入偏移。
     */
    double midlineOffset(const std::vector<Vec3> &upperArc, const std::vector<Vec3> &lowerArc);

    /**
     * S-10 覆合 / 覆盖（mm）。
     * 上/下切牙切缘点给出后：
     *   overjet（覆盖）= 两点在前后向（apAxis，0=X 1=Y）分量之差
     *   overbite（覆合）= 垂直方向（Z）分量之差（上切缘 - 下切缘，正值=正常覆合）
     */
    void overbiteOverjet(const Vec3 &upperIncisal, const Vec3 &lowerIncisal,
                         int apAxis, double &overjetMm, double &overbiteMm);

    /** M-07 单点 HU 采样 + 组织推断，输出 "HU=312 (cancellous bone)" 形式 */
    std::string huSampleText(const VolumeRef &vol, const Vec3 &p);

    /**
     * HU -> 组织类型（供 UI/SR 复用，与 VolumeRef::tissueName 同源）。
     * 必须 inline：这是头文件里唯一带函数体的自由函数定义，
     * 少了 inline 会在链接期报 duplicate symbol（每个 include 它的 .o 都出一份）。
     */
    inline std::string tissueName(float hu) { return VolumeRef::tissueName(hu); }

    /** 数值格式化工具：固定小数位，报告/叠加层/SR 共用同一渲染口径 */
    std::string format(double value, int digits = 2);

}   // namespace MeasureMath

#endif // DCMTKDEMO_MEASUREMATH_H
