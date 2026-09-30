// 射线拾取实现（PRD 5.1.3 / 8.3；风险 R-01 的替代 vtkCellPicker 方案）。
//
// 全部计算只依赖 VolumeRef（世界 mm <-> 体素索引）与射线几何，
// 可主机侧编译单测：拾取点若命中骨面，其 HU 必须 >= 阈值，
// 且与 displayToRay 的逆投影精度共同决定 AC-01 的可复现性。

#include "include/MeasurePicker.h"
#include "include/MeasureMath.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>

#define TAG "CbctMeasureCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace {
    const double kAxisEps = 1e-9;
    const double kSphereR = 1.0e30;
}

MeasurePicker::MeasurePicker(const VolumeRef &vol) : vol_(vol) {}

/**
 * slab 法求射线与轴对齐包围盒的交区间。
 * 包围盒向各方向外扩半个体素：体素代表的是"以中心为锚的小立方体"，
 * 只按中心点连线求交会让贴边触摸永远打不到最外一圈体素。
 */
bool MeasurePicker::rayVolumeBox(const Vec3 &origin, const Vec3 &dir,
                                 double &t0, double &t1) const {
    t0 = 0.0;
    Vec3 lo, hi;
    vol_.boundsMin(lo);
    vol_.boundsMax(hi);
    const double hx = vol_.spacingX() * 0.5, hy = vol_.spacingY() * 0.5, hz = vol_.spacingZ() * 0.5;
    const double bmin[3] = {lo.x - hx, lo.y - hy, lo.z - hz};
    const double bmax[3] = {hi.x + hx, hi.y + hy, hi.z + hz};
    const double o[3] = {origin.x, origin.y, origin.z};
    const double d[3] = {dir.x, dir.y, dir.z};

    t1 = kSphereR;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < kAxisEps) {
            if (o[a] < bmin[a] || o[a] > bmax[a]) return false;   // 平行且在外侧
            continue;
        }
        double inv = 1.0 / d[a];
        double ta = (bmin[a] - o[a]) * inv;
        double tb = (bmax[a] - o[a]) * inv;
        if (ta > tb) { const double t = ta; ta = tb; tb = t; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return false;
    }
    return t1 > 0.0;
}

Vec3 MeasurePicker::snapToVoxelCenter(const Vec3 &p) const {
    int i, j, k;
    vol_.worldToIndex(p, i, j, k);
    Vec3 out;
    vol_.indexToWorld(i, j, k, out);
    return out;
}

/** 行进主体：把命中位置换算成体素中心 / HU / 描述文本 */
static PickResult finalizeHit(const VolumeRef &vol, const Vec3 &origin, const Vec3 &dir,
                              double t, const char *what) {
    PickResult r;
    r.hit = true;
    r.point = origin + dir * t;
    r.distanceMm = t;
    int i, j, k;
    vol.worldToIndex(r.point, i, j, k);
    // 命中位置可能落在体素边界外侧半个体素（包围盒外扩），钳制到有效索引
    i = std::min(std::max(i, 0), vol.width() - 1);
    j = std::min(std::max(j, 0), vol.height() - 1);
    k = std::min(std::max(k, 0), vol.depth() - 1);
    r.index[0] = i; r.index[1] = j; r.index[2] = k;
    vol.indexToWorld(i, j, k, r.voxelCenter);
    r.hu = vol.huNearest(r.voxelCenter, 0.0f);
    char buf[160];
    snprintf(buf, sizeof(buf), "%s at voxel(%d,%d,%d)=%.0fHU t=%.2fmm",
             what, i, j, k, (double) r.hu, t);
    r.describe = buf;
    return r;
}

PickResult MeasurePicker::pickSurface(const Vec3 &origin, const Vec3 &dir,
                                      double huThreshold, double maxMm,
                                      double tMinMm) const {
    PickResult r;
    if (!vol_.valid()) { r.describe = "no volume"; return r; }
    double t0, t1;
    if (!rayVolumeBox(origin, dir, t0, t1)) { r.describe = "ray misses volume"; return r; }
    if (t0 < 0.0) t0 = 0.0;
    if (maxMm > 0.0) t1 = std::min(t1, maxMm);
    // 深度窗口：起搜点被推到窗口外时说明整条射线都不在"同一张骨面"上，直接判未命中
    if (tMinMm > 0.0) t0 = std::max(t0, tMinMm);
    if (t0 > t1) { r.describe = "no surface in depth window"; return r; }
    const double step = std::min(vol_.spacingX(), std::min(vol_.spacingY(), vol_.spacingZ())) * 0.5;
    for (double t = t0; t <= t1; t += step) {
        const Vec3 p = origin + dir * t;
        if (vol_.huTrilinear(p, -2000.0f) >= (float) huThreshold) return finalizeHit(vol_, origin, dir, t, "surface");
    }
    r.describe = "no surface along ray";
    return r;
}

PickResult MeasurePicker::pickThroughBone(const Vec3 &origin, const Vec3 &dir, int skip,
                                          double huThreshold) const {
    PickResult r;
    if (!vol_.valid()) return r;
    double t0, t1;
    if (!rayVolumeBox(origin, dir, t0, t1)) return r;
    if (t0 < 0.0) t0 = 0.0;
    const double step = std::min(vol_.spacingX(), std::min(vol_.spacingY(), vol_.spacingZ())) * 0.5;
    bool inside = false;
    int entries = 0;
    for (double t = t0; t <= t1; t += step) {
        const bool b = vol_.huTrilinear(origin + dir * t, -2000.0f) >= (float) huThreshold;
        if (b && !inside) {                       // 一次"进入骨"事件
            if (entries++ == skip) return finalizeHit(vol_, origin, dir, t, "bone entry");
            inside = true;
        } else if (!b && inside) {
            inside = false;
        }
    }
    return r;
}

PickResult MeasurePicker::pickHuRange(const Vec3 &origin, const Vec3 &dir,
                                      double lo, double hi) const {
    PickResult r;
    if (!vol_.valid()) return r;
    double t0, t1;
    if (!rayVolumeBox(origin, dir, t0, t1)) return r;
    if (t0 < 0.0) t0 = 0.0;
    const double step = std::min(vol_.spacingX(), std::min(vol_.spacingY(), vol_.spacingZ())) * 0.5;
    for (double t = t0; t <= t1; t += step) {
        const float hu = vol_.huTrilinear(origin + dir * t, -2000.0f);
        if (hu >= (float) lo && hu <= (float) hi) {
            return finalizeHit(vol_, origin, dir, t, "hu window");
        }
    }
    return r;
}

PickResult MeasurePicker::pickPlane(const Vec3 &origin, const Vec3 &dir,
                                    int plane, int position) const {
    PickResult r;
    if (!vol_.valid()) return r;
    // 与 :cbctdeal 的平面定义一致：轴位=恒定 Z，冠状=恒定 Y，矢状=恒定 X
    const int axis = (plane == MP_CORONAL) ? 1 : (plane == MP_SAGITTAL ? 0 : 2);
    double sp = axis == 0 ? vol_.spacingX() : (axis == 1 ? vol_.spacingY() : vol_.spacingZ());
    if (sp <= 1e-9) sp = 1.0;   //  spacing 异常时退化为 1mm，避免除零后平面坐标恒为 0
    const double planeCoord = (double) position * sp;
    const double o = axis == 0 ? origin.x : (axis == 1 ? origin.y : origin.z);
    const double d = axis == 0 ? dir.x : (axis == 1 ? dir.y : dir.z);
    if (std::fabs(d) < kAxisEps) { r.describe = "ray parallel to plane"; return r; }
    const double t = (planeCoord - o) / d;
    if (t <= 0.0) { r.describe = "plane behind camera"; return r; }
    const Vec3 p = origin + dir * t;
    // 平面内的两个分量必须落在体数据范围内（交点钳到边界内 1 个像素，容忍指尖误差）
    const int order[3] = {0, 1, 2};
    for (int n = 0; n < 3; ++n) {
        const int a = order[n];
        if (a == axis) continue;
        const double v = a == 0 ? p.x : (a == 1 ? p.y : p.z);
        const double maxV = (a == 0 ? vol_.width() : (a == 1 ? vol_.height() : vol_.depth())) *
                            (a == 0 ? vol_.spacingX() : (a == 1 ? vol_.spacingY() : vol_.spacingZ()));
        if (v < -vol_.spacingX() || v > maxV) { r.describe = "hit outside plane extent"; return r; }
    }
    Vec3 snapped = snapToVoxelCenter(p);
    // 只吸附平面内分量，法向分量必须精确等于该层层位（否则测量点会漂到相邻层）
    if (axis == 0) snapped.x = planeCoord;
    else if (axis == 1) snapped.y = planeCoord;
    else snapped.z = planeCoord;

    r.hit = true;
    r.point = snapped;
    r.voxelCenter = snapped;
    r.distanceMm = t;
    int i, j, k;
    vol_.worldToIndex(snapped, i, j, k);
    r.index[0] = i; r.index[1] = j; r.index[2] = k;
    r.hu = vol_.huNearest(snapped, 0.0f);
    char buf[160];
    snprintf(buf, sizeof(buf), "plane(%d,%d) at voxel(%d,%d,%d)=%.0fHU", plane, position, i, j, k,
             (double) r.hu);
    r.describe = buf;
    LOGD("%s", r.describe.c_str());
    return r;
}

PickResult MeasurePicker::pickRoiSurface(const RoiDef &roi, const Vec3 &origin,
                                         const Vec3 &dir) const {
    PickResult r;
    if (!vol_.valid()) return r;
    switch (roi.type) {
        case ROI_BOX: {
            const Vec3 lo(std::min(roi.boxMin.x, roi.boxMax.x), std::min(roi.boxMin.y, roi.boxMax.y),
                          std::min(roi.boxMin.z, roi.boxMax.z));
            const Vec3 hi(std::max(roi.boxMin.x, roi.boxMax.x), std::max(roi.boxMin.y, roi.boxMax.y),
                          std::max(roi.boxMin.z, roi.boxMax.z));
            double tIn = 0.0, tOut = kSphereR;
            const double o[3] = {origin.x, origin.y, origin.z};
            const double d[3] = {dir.x, dir.y, dir.z};
            const double bmin[3] = {lo.x, lo.y, lo.z};
            const double bmax[3] = {hi.x, hi.y, hi.z};
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(d[a]) < kAxisEps) {
                    if (o[a] < bmin[a] || o[a] > bmax[a]) return r;
                    continue;
                }
                const double inv = 1.0 / d[a];
                double ta = (bmin[a] - o[a]) * inv, tb = (bmax[a] - o[a]) * inv;
                if (ta > tb) std::swap(ta, tb);
                if (ta > tIn) tIn = ta;
                if (tb < tOut) tOut = tb;
            }
            if (tIn > tOut || tOut < 0.0) return r;
            const double t = tIn > 0.0 ? tIn : tOut;   // 起点在盒内时取远侧边界
            r = finalizeHit(vol_, origin, dir, t, "roi box");
            return r;
        }
        case ROI_SPHERE: {
            const Vec3 oc = origin - roi.sphereCenter;
            const double b = oc.dot(dir);
            const double c = oc.dot(oc) - roi.sphereRadius * roi.sphereRadius;
            const double disc = b * b - c;
            if (disc < 0.0) return r;
            const double sq = std::sqrt(disc);
            double t = -b - sq;
            if (t < 0.0) t = -b + sq;               // 起点在球内取远侧
            if (t < 0.0) return r;
            return finalizeHit(vol_, origin, dir, t, "roi sphere");
        }
        case ROI_PLANE: {
            const Vec3 n = roi.planeNormal.normalized();
            const double dn = dir.dot(n);
            if (std::fabs(dn) < kAxisEps) return r;
            const double t = (roi.planeOrigin - origin).dot(n) / dn;
            if (t < 0.0) return r;
            return finalizeHit(vol_, origin, dir, t, "roi plane");
        }
        case ROI_HU_THRESHOLD:
            return pickHuRange(origin, dir, roi.huMin, roi.huMax);
        case ROI_COMPOSITE:
            // 组合 ROI 没有自己的外框几何：按其 HU 窗口做等值面拾取。
            // （B/C 子项只影响统计范围，不影响可视化命中）
            return pickHuRange(origin, dir, roi.huMin, roi.huMax);
    }
    return r;
}

std::string MeasurePicker::describePick(const PickResult &r) const {
    if (!r.hit) return r.describe.empty() ? "未命中" : r.describe;
    char buf[224];
    snprintf(buf, sizeof(buf), "(%.1f, %.1f, %.1f)mm  voxel(%d,%d,%d)  %.0f HU  %s  t=%.2fmm",
             r.point.x, r.point.y, r.point.z, r.index[0], r.index[1], r.index[2],
             (double) r.hu, VolumeRef::tissueName(r.hu).c_str(), r.distanceMm);
    return std::string(buf);
}

bool MeasurePicker::pickNervePoint(const NervePath &path, const Vec3 &origin, const Vec3 &dir,
                                   double tolMm, int &pointIndex, Vec3 &closestOnRay) const {
    if (path.points.size() < 2) return false;
    double t0, t1;
    if (!rayVolumeBox(origin, dir, t0, t1)) return false;
    // 把射线取为体积包围盒内的有限线段再求线段间距：等价于射线距离，
    // 且完全避开"平行/重合线段"分支的除零风险
    const Vec3 a = origin + dir * std::max(t0, 0.0);
    const Vec3 b = origin + dir * t1;
    double best = 1e30;
    int bestIdx = -1;
    for (size_t i = 1; i < path.points.size(); ++i) {
        Vec3 ca, cb;
        const double d = MeasureMath::segmentSegment(a, b, path.points[i - 1], path.points[i], &ca, &cb);
        if (d < best) { best = d; bestIdx = (int) i; closestOnRay = ca; }
    }
    if (bestIdx < 0 || best > tolMm) return false;
    pointIndex = bestIdx;
    LOGD("nerve picked segment #%d dist=%.2fmm", bestIdx, best);
    return true;
}
