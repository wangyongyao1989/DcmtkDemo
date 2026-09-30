// ROI 提取与体数据统计实现（PRD 5.2 R-01 ~ R-05，支撑 M-04 / M-08）。
//
// 权重模型与遍历范围策略见 include/RoiExtractor.h 顶部注释。
// 本文件是纯 C++（只读 Volume），可主机侧编译单测：AC-04「体积误差 <= 1%」
// 由 src/host 的球体体积用例直接验证。

#include "include/RoiExtractor.h"
#include "include/MeasureMath.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#define TAG "CbctMeasureCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

static long long monoMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

RoiExtractor::RoiExtractor(const VolumeRef &vol) : vol_(vol) {}

namespace {

    /** 递归深度上限：组合 ROI 允许 A/B/C 各再嵌套一层，防御环引用 */
    const int kMaxDepth = 4;

    /** 体素立方体的 8 个角点（用于 2x2x2 超采样覆盖权重） */
    struct CornerWalker {
        Vec3 center;
        Vec3 half;      // (sx/2, sy/2, sz/2)

        Vec3 corner(int n) const {
            return Vec3(center.x + ((n & 1) ? half.x : -half.x),
                        center.y + ((n & 2) ? half.y : -half.y),
                        center.z + ((n & 4) ? half.z : -half.z));
        }
    };

    /** 几何包含测试（不含 HU 判定） */
    bool geometryContains(const RoiDef &roi, const Vec3 &p) {
        switch (roi.type) {
            case ROI_BOX: {
                const double loX = std::min(roi.boxMin.x, roi.boxMax.x);
                const double hiX = std::max(roi.boxMin.x, roi.boxMax.x);
                const double loY = std::min(roi.boxMin.y, roi.boxMax.y);
                const double hiY = std::max(roi.boxMin.y, roi.boxMax.y);
                const double loZ = std::min(roi.boxMin.z, roi.boxMax.z);
                const double hiZ = std::max(roi.boxMin.z, roi.boxMax.z);
                return p.x >= loX && p.x <= hiX && p.y >= loY && p.y <= hiY &&
                       p.z >= loZ && p.z <= hiZ;
            }
            case ROI_PLANE: {
                const double l2 = roi.planeNormal.dot(roi.planeNormal);
                if (l2 < 1e-12) return true;         // 非法法向：等同于不切割
                return (p - roi.planeOrigin).dot(roi.planeNormal) >= 0.0;
            }
            case ROI_SPHERE:
                return (p - roi.sphereCenter).length() <= roi.sphereRadius;
            default:
                return true;   // R-01 / R-05 本身没有独立几何
        }
    }

    // ------------------------------------------------------------------
    // R-03 截面 ROI 的"单层 + 轮廓"模型
    //
    // 改动前 geometryContains(ROI_PLANE) 只判半空间，多边形完全没参与体素权重，
    // 于是截面 ROI 的量化对象是"平面一侧的整半个体积"。真机实测暴露得很直接：
    // 大盒 #7（coverage 1370246.5）与截面 #12（4 点、152.498cm²）做交集，
    // 得到的 coverage / 体积 / 平均 HU 与 #7 单独统计逐位相同 —— 相当于 B 没参与。
    // 语义上 R-03 是"这一层里我圈的那块"，所以权重改成两段相乘：
    //   层门（硬判定）：体素中心到勾画平面的法向距离 <= 半个沿法向体素步长。
    //     这里刻意不做角点加权：若按 ±半格的角点判 slab，相邻层的 4 个角点
    //     正好落在边界上（边界取 <=，算内部），会得到 0.5 权重，
    //     一层截面被算成两层，体积直接翻倍。
    //   面内（角点加权）：8 个角点投影到平面上做 2D 内外测试，取命中比例，
    //     保留 M-05/AC-03 需要的 2x2x2 超采样边界精度。
    // 投影轴取法向绝对值最大的那一轴并丢弃它：对平面多边形这是双射，
    // 不需要构造 (u,v) 正交基，也没有 sqrt，逐体素代价最低。
    // 没有多边形（<3 点）时保持旧的半空间语义，纯切割面的行为不变。
    // ------------------------------------------------------------------

    /** 截面模型是否生效（R-03 需要 >=3 点轮廓 + 合法法向） */
    bool planeHasContour(const RoiDef &roi) {
        return roi.type == ROI_PLANE && roi.polygon.size() >= 3 &&
               roi.planeNormal.dot(roi.planeNormal) > 1e-12;
    }

    /** 法向绝对值最大的轴：0=x 1=y 2=z（丢弃它做正交投影） */
    int dominantAxis(const Vec3 &n) {
        const double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        if (ax >= ay && ax >= az) return 0;
        if (ay >= az) return 1;
        return 2;
    }

    /** 丢弃 dropAxis 后的平面内二维坐标 */
    void proj2(const Vec3 &p, int dropAxis, double &u, double &v) {
        switch (dropAxis) {
            case 0:  u = p.y; v = p.z; break;
            case 1:  u = p.x; v = p.z; break;
            default: u = p.x; v = p.y; break;
        }
    }

    /** 射线法 2D 多边形内外测试（边界点按"严格小于交点"计数，退化情形不抖） */
    bool polygonContains2D(const std::vector<Vec3> &poly, const Vec3 &p, int dropAxis) {
        double pu, pv;
        proj2(p, dropAxis, pu, pv);
        bool in = false;
        const size_t n = poly.size();
        for (size_t a = 0, b = n - 1; a < n; b = a++) {
            double au, av, bu, bv;
            proj2(poly[a], dropAxis, au, av);
            proj2(poly[b], dropAxis, bu, bv);
            if ((av > pv) != (bv > pv)) {
                const double cross = au + (pv - av) * (bu - au) / (bv - av);
                if (pu < cross) in = !in;
            }
        }
        return in;
    }

    /** 沿法向的半个体素步长（层门阈值）：轴对齐法向时就是该轴 spacing 的一半 */
    double slabHalfThickness(const Vec3 &nUnit, const VolumeRef &vol) {
        const double ax = nUnit.x * vol.spacingX();
        const double ay = nUnit.y * vol.spacingY();
        const double az = nUnit.z * vol.spacingZ();
        return 0.5 * std::sqrt(ax * ax + ay * ay + az * az);
    }

    const RoiDef *findRoi(const std::vector<RoiDef> *pool, int id) {
        if (!pool) return nullptr;
        for (size_t i = 0; i < pool->size(); ++i) {
            if ((*pool)[i].id == id) return &(*pool)[i];
        }
        return nullptr;
    }

    /** 单个 ROI 对体素 (i,j,k) 的覆盖权重（含子 ROI 递归） */
    double roiWeight(const RoiDef &roi, int i, int j, int k,
                     const VolumeRef &vol, const std::vector<RoiDef> *pool,
                     int depth, std::string *error) {
        if (depth > kMaxDepth) {
            if (error && error->empty()) *error = "ROI 组合层级过深（可能存在循环引用）";
            return 0.0;
        }
        switch (roi.type) {
            case ROI_HU_THRESHOLD: {
                Vec3 c;
                vol.indexToWorld(i, j, k, c);
                const float hu = vol.huNearest(c, -2000.0f);
                return (hu >= (float) roi.huMin && hu <= (float) roi.huMax) ? 1.0 : 0.0;
            }
            case ROI_BOX:
            case ROI_PLANE:
            case ROI_SPHERE: {
                Vec3 center;
                vol.indexToWorld(i, j, k, center);
                const CornerWalker w = {center,
                                        Vec3(vol.spacingX() * 0.5, vol.spacingY() * 0.5,
                                             vol.spacingZ() * 0.5)};
                if (roi.type == ROI_PLANE && planeHasContour(roi)) {
                    // 单层门 + 面内角点加权（理由见 planeHasContour 上方注释块）
                    const Vec3 nu = roi.planeNormal.normalized();
                    if (std::fabs((center - roi.planeOrigin).dot(nu)) > slabHalfThickness(nu, vol))
                        return 0.0;
                    const int drop = dominantAxis(nu);
                    int inPlane = 0;
                    for (int n = 0; n < 8; ++n) {
                        if (polygonContains2D(roi.polygon, w.corner(n), drop)) ++inPlane;
                    }
                    return (double) inPlane / 8.0;
                }
                int hit = 0;
                for (int n = 0; n < 8; ++n) if (geometryContains(roi, w.corner(n))) ++hit;
                return (double) hit / 8.0;
            }
            case ROI_COMPOSITE: {
                const RoiDef *a = findRoi(pool, roi.childA);
                if (!a) {
                    if (error && error->empty()) *error = "组合 ROI 缺少子 ROI A";
                    return 0.0;
                }
                double w = roiWeight(*a, i, j, k, vol, pool, depth + 1, error);
                if (roi.childB != 0) {
                    const RoiDef *b = findRoi(pool, roi.childB);
                    if (b) {
                        const double wb = roiWeight(*b, i, j, k, vol, pool, depth + 1, error);
                        if (roi.opAB == OP_UNION) w = std::max(w, wb);
                        else if (roi.opAB == OP_INTERSECT) w = std::min(w, wb);
                        // OP_NONE：忽略 B
                    }
                }
                if (roi.childC != 0) {
                    const RoiDef *c = findRoi(pool, roi.childC);
                    if (c) {
                        const double wc = roiWeight(*c, i, j, k, vol, pool, depth + 1, error);
                        // R-05 的"NOT C"语义：差集；也允许显式 INTERSECT 三重叠
                        w = (roi.opAC == OP_INTERSECT) ? std::min(w, wc) : std::min(w, 1.0 - wc);
                    }
                }
                return w;
            }
        }
        return 0.0;
    }

}   // namespace

double RoiExtractor::weightOfVoxel(const RoiDef &roi, int i, int j, int k,
                                   const std::vector<RoiDef> *pool,
                                   std::string *error) const {
    return roiWeight(roi, i, j, k, vol_, pool, 0, error);
}

bool RoiExtractor::contains(const RoiDef &roi, const Vec3 &world,
                            const std::vector<RoiDef> *pool) const {
    if (!vol_.valid()) return false;
    int i, j, k;
    vol_.worldToIndex(world, i, j, k);
    std::string err;
    return weightOfVoxel(roi, i, j, k, pool, &err) > 0.0;
}

int RoiExtractor::spatialBounds(const RoiDef &roi, const std::vector<RoiDef> *pool,
                                int &i0, int &j0, int &k0, int &i1, int &j1, int &k1) const {
    return spatialBoundsAt(roi, pool, i0, j0, k0, i1, j1, k1, 0);
}

int RoiExtractor::spatialBoundsAt(const RoiDef &roi, const std::vector<RoiDef> *pool,
                                  int &i0, int &j0, int &k0, int &i1, int &j1, int &k1,
                                  int depth) const {
    // 主机侧单测（src/host/test_main.cpp 组 6 的环引用用例）复现的真实缺陷：
    // 下面 ROI_COMPOSITE 分支会递归子 ROI，而组合 ROI 的 childA/childB 来自
    // 持久化 JSON（MeasurementManager::roiToJson/roiFromJson 原样读写），
    // 手工编辑或旧版本数据里出现 A.childA=B 且 B.childA=A 的环时，
    // 这里会无限递归直到栈溢出（macOS 上实测 SIGSEGV，整个进程挂掉）。
    // roiWeight() 有 kMaxDepth 守卫，但 analyze() 先调 spatialBounds()，拦不住。
    // 修复：用同一个 kMaxDepth 上限停止递归，退回"全体积扫描"——体积仍由
    // 权重函数决定（超深的体素一律返回 0 并写入"层级过深"错误文案），
    // 因此非环引用的合法 ROI 行为逐字节不变，只是不再崩溃。
    if (depth > kMaxDepth) return SB_FULL_VOLUME;

    if (!vol_.valid()) return SB_EMPTY;
    int spatial = SB_FULL_VOLUME;

    struct Conv {
        const VolumeRef &v;
        void toIdxFloor(const Vec3 &w, int &i, int &j, int &k) const {
            i = (int) std::floor(w.x / (v.spacingX() > 1e-9 ? v.spacingX() : 1.0));
            j = (int) std::floor(w.y / (v.spacingY() > 1e-9 ? v.spacingY() : 1.0));
            k = (int) std::floor(w.z / (v.spacingZ() > 1e-9 ? v.spacingZ() : 1.0));
        }
    } conv = {vol_};

    // 初值 = 全体素索引闭区间
    {
        int a, b, c;
        conv.toIdxFloor(Vec3(0, 0, 0), a, b, c);
        i0 = a; j0 = b; k0 = c;
        Vec3 hi;
        vol_.boundsMax(hi);
        conv.toIdxFloor(hi, a, b, c);
        i1 = std::max(a, i0); j1 = std::max(b, j0); k1 = std::max(c, k0);
    }

    // 体素的权重看的是它的 8 个角点（center ± 半格），所以空间裁剪后的索引区间必须
    // 比"盒/球包围盒的 floor 区间"再宽一格：
    //   上端 i1 = floor(hi/s) 会漏掉 i1+1 —— 该体素靠近 hi 的那个半角 (i1+0.5)*s
    //   仍在区间内（只要 hi/s 的小数部分 >= 0.5 就会漏）。
    //   下端 floor(lo/s) 不会漏（floor(lo/s) <= ceil(lo/s - 0.5) 恒成立）。
    // 这个"上端少一格"是快速路径等价用例（R-02 #3：厚度小于一个体素的盒）抓出来的
    // 既有缺陷，会让 coverage 少算约 10%。
    auto clipTo = [&](const Vec3 &blo, const Vec3 &bhi) {
        int a0, b0, c0, a1, b1, c1;
        conv.toIdxFloor(blo, a0, b0, c0);
        conv.toIdxFloor(bhi, a1, b1, c1);
        i0 = std::max(i0, std::min(a0, a1));
        j0 = std::max(j0, std::min(b0, b1));
        k0 = std::max(k0, std::min(c0, c1));
        i1 = std::min(i1, std::max(a0, a1) + 1);
        j1 = std::min(j1, std::max(b0, b1) + 1);
        k1 = std::min(k1, std::max(c0, c1) + 1);
    };

    switch (roi.type) {
        case ROI_HU_THRESHOLD:
            spatial = SB_FULL_VOLUME;              // 无空间约束：必须全体积
            break;
        case ROI_BOX:
            clipTo(Vec3(std::min(roi.boxMin.x, roi.boxMax.x), std::min(roi.boxMin.y, roi.boxMax.y),
                        std::min(roi.boxMin.z, roi.boxMax.z)),
                   Vec3(std::max(roi.boxMin.x, roi.boxMax.x), std::max(roi.boxMin.y, roi.boxMax.y),
                        std::max(roi.boxMin.z, roi.boxMax.z)));
            spatial = SB_SPATIAL;
            break;
        case ROI_SPHERE:
            clipTo(Vec3(roi.sphereCenter.x - roi.sphereRadius,
                        roi.sphereCenter.y - roi.sphereRadius,
                        roi.sphereCenter.z - roi.sphereRadius),
                   Vec3(roi.sphereCenter.x + roi.sphereRadius,
                        roi.sphereCenter.y + roi.sphereRadius,
                        roi.sphereCenter.z + roi.sphereRadius));
            spatial = SB_SPATIAL;
            break;
        case ROI_PLANE: {
            if (planeHasContour(roi)) {
                // 有轮廓的截面 ROI：候选集 = 多边形包围盒 ∩ 勾画那一层。
                // 改动前只按半空间裁一半体积，512x512x265 的数据上单层多边形
                // 也要白扫上百万体素（真机实测组合 ROI #14 因此用了 264ms）。
                double bx0 = roi.polygon[0].x, bx1 = bx0;
                double by0 = roi.polygon[0].y, by1 = by0;
                double bz0 = roi.polygon[0].z, bz1 = bz0;
                for (size_t p = 1; p < roi.polygon.size(); ++p) {
                    const Vec3 &q = roi.polygon[p];
                    bx0 = std::min(bx0, q.x); bx1 = std::max(bx1, q.x);
                    by0 = std::min(by0, q.y); by1 = std::max(by1, q.y);
                    bz0 = std::min(bz0, q.z); bz1 = std::max(bz1, q.z);
                }
                clipTo(Vec3(bx0, by0, bz0), Vec3(bx1, by1, bz1));
                const Vec3 nu = roi.planeNormal.normalized();
                const int drop = dominantAxis(nu);
                // 只有轴对齐法向才能把主轴锁到整数层区间；斜法向会斜穿多层，
                // 保持"多边形包围盒"这一层裁剪即可（正确性仍由权重函数保证）。
                if (std::fabs(nu.x) > 0.99 || std::fabs(nu.y) > 0.99 || std::fabs(nu.z) > 0.99) {
                    const double o = drop == 0 ? roi.planeOrigin.x
                                 : drop == 1 ? roi.planeOrigin.y : roi.planeOrigin.z;
                    const double s = std::max(1e-9, drop == 0 ? vol_.spacingX()
                                                  : drop == 1 ? vol_.spacingY()
                                                              : vol_.spacingZ());
                    // 层门的闭区间 |i*s - o| <= 0.5*s 换算成索引：
                    // o/s 恰好在两层正中间时会有两个层，所以两端都用闭区间取整。
                    const int lo = (int) std::ceil(o / s - 0.5);
                    const int hi = (int) std::floor(o / s + 0.5);
                    if (drop == 0) { i0 = std::max(i0, lo); i1 = std::min(i1, hi); }
                    else if (drop == 1) { j0 = std::max(j0, lo); j1 = std::min(j1, hi); }
                    else { k0 = std::max(k0, lo); k1 = std::min(k1, hi); }
                }
                spatial = SB_SPATIAL;
                break;
            }
            // 纯切割面（无轮廓）：半空间裁剪。只对轴对齐法向做整数区间裁剪，
            // 任意法向保持全体积区间（结果仍由权重函数保证正确，只是多扫描）
            // 负方向的截断同样要给上端留一格（理由见 clipTo 的注释）：
            // 权重判定用的是体素的半角端点，frac(origin/spacing)>=0.5 时
            // 索引 cut+1 的体素仍有一个角落在半空间内。
            const Vec3 n = roi.planeNormal.normalized();
            if (std::fabs(n.x) > 0.99) {
                const int cut = (int) std::floor(roi.planeOrigin.x / vol_.spacingX());
                if (n.x > 0) i0 = std::max(i0, cut); else i1 = std::min(i1, cut + 1);
            } else if (std::fabs(n.y) > 0.99) {
                const int cut = (int) std::floor(roi.planeOrigin.y / vol_.spacingY());
                if (n.y > 0) j0 = std::max(j0, cut); else j1 = std::min(j1, cut + 1);
            } else if (std::fabs(n.z) > 0.99) {
                const int cut = (int) std::floor(roi.planeOrigin.z / vol_.spacingZ());
                if (n.z > 0) k0 = std::max(k0, cut); else k1 = std::min(k1, cut + 1);
            }
            spatial = SB_SPATIAL;
            break;
        }
        case ROI_COMPOSITE: {
            // 组合 ROI 的候选区间：交集取各子区间的"交"、并集取"并"、差集只看 A。
            // 改动前只看 A 的包围盒，"大盒 ∩ 一层截面"要把上百万个必然为 0 的
            // 体素各走一遍递归权重（真机实测 组合 #14 因此 264ms，超 PC-01 的 50ms）；
            // 先做区间相交就能把候选集缩到两个子 ROI 真正重叠的部分，
            // 权重口径不变（区间外的体素交集权重本来就是 0）。
            const RoiDef *a = findRoi(pool, roi.childA);
            if (a) {
                int x0, y0, z0, x1, y1, z1;
                const int r = spatialBoundsAt(*a, pool, x0, y0, z0, x1, y1, z1, depth + 1);
                if (r == SB_EMPTY) return SB_EMPTY;
                if (r == SB_SPATIAL) {
                    i0 = x0; j0 = y0; k0 = z0; i1 = x1; j1 = y1; k1 = z1;
                    spatial = SB_SPATIAL;
                }
            }
            if (roi.childB != 0 && roi.opAB != OP_NONE) {
                const RoiDef *b = findRoi(pool, roi.childB);
                int x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
                const int rb = b ? spatialBoundsAt(*b, pool, x0, y0, z0, x1, y1, z1, depth + 1)
                                 : SB_FULL_VOLUME;
                if (roi.opAB == OP_INTERSECT) {
                    if (rb == SB_EMPTY) return SB_EMPTY;
                    if (rb == SB_SPATIAL) {
                        i0 = std::max(i0, x0); j0 = std::max(j0, y0); k0 = std::max(k0, z0);
                        i1 = std::min(i1, x1); j1 = std::min(j1, y1); k1 = std::min(k1, z1);
                        spatial = SB_SPATIAL;
                    }
                } else if (rb != SB_EMPTY) {
                    // 并集：任一方需要全体积扫描（如 R-01 纯阈值）时结果也必须全体积
                    if (rb == SB_SPATIAL && spatial == SB_SPATIAL) {
                        i0 = std::min(i0, x0); j0 = std::min(j0, y0); k0 = std::min(k0, z0);
                        i1 = std::max(i1, x1); j1 = std::max(j1, y1); k1 = std::max(k1, z1);
                    } else {
                        return SB_FULL_VOLUME;
                    }
                }
            }
            // C 只有两种角色：OP_INTERSECT（三重重叠，可继续缩区间）
            // 与 OP_SUBTRACT（要挖掉的部分，不能用来缩小区间）
            if (roi.childC != 0 && roi.opAC == OP_INTERSECT) {
                const RoiDef *c = findRoi(pool, roi.childC);
                int x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
                const int rc = c ? spatialBoundsAt(*c, pool, x0, y0, z0, x1, y1, z1, depth + 1)
                                 : SB_FULL_VOLUME;
                if (rc == SB_EMPTY) return SB_EMPTY;
                if (rc == SB_SPATIAL) {
                    i0 = std::max(i0, x0); j0 = std::max(j0, y0); k0 = std::max(k0, z0);
                    i1 = std::min(i1, x1); j1 = std::min(j1, y1); k1 = std::min(k1, z1);
                    spatial = SB_SPATIAL;
                }
            }
            break;
        }
    }
    if (i0 > i1 || j0 > j1 || k0 > k1) return SB_EMPTY;   // 完全落在体积之外
    return spatial;
}

double RoiExtractor::polygonAreaMm2(const RoiDef &roi, Vec3 *centroid) const {
    if (roi.polygon.size() < 3) return 0.0;
    if (centroid) *centroid = MeasureMath::polygonCentroid(roi.polygon);
    return MeasureMath::polygonArea(roi.polygon);
}

RoiStats RoiExtractor::analyze(const RoiDef &roi, const std::vector<RoiDef> *pool) const {
    RoiStats s;
    if (!vol_.valid() || vol_.data() == nullptr) {
        s.error = "体数据未绑定";
        return s;
    }
    const long long t0 = monoMs();
    s.voxelMm3 = vol_.voxelVolumeMm3();

    int i0, j0, k0, i1, j1, k1;
    const int bounds = spatialBounds(roi, pool, i0, j0, k0, i1, j1, k1);
    if (bounds == SB_EMPTY) {
        s.error = "ROI 与体数据无交集";
        return s;
    }
    if (bounds == SB_FULL_VOLUME) {
        i0 = 0; j0 = 0; k0 = 0;
        i1 = vol_.width() - 1; j1 = vol_.height() - 1; k1 = vol_.depth() - 1;
    }
    i0 = std::max(i0, 0); j0 = std::max(j0, 0); k0 = std::max(k0, 0);
    i1 = std::min(i1, vol_.width() - 1);
    j1 = std::min(j1, vol_.height() - 1);
    k1 = std::min(k1, vol_.depth() - 1);

    double sumW = 0.0, sumHW = 0.0, sumHHW = 0.0;
    double minH = 0.0, maxH = 0.0;
    bool first = true;
    long long full = 0, scanned = 0;
    std::string err;

    const float *data = vol_.data();
    const int W = vol_.width(), H = vol_.height();
    const size_t slice = vol_.sliceSize();
    (void) H;

    // 累加口径只有一处：快速路径与通用路径共用，避免两条循环写出两种统计
    auto accumulate = [&](double w, float hu) {
        sumW += w;
        sumHW += w * hu;
        sumHHW += w * (double) hu * (double) hu;
        if (first) { minH = maxH = hu; first = false; }
        else {
            if (hu < minH) minH = hu;
            if (hu > maxH) maxH = hu;
        }
        if (w >= 1.0) ++full;
    };

    if (roi.type == ROI_BOX) {
        // R-02 轴对齐盒快速路径。
        // 原实现给每个体素构造 8 个角点再逐个做 6 次区间比较；因为
        //   1) indexToWorld 是纯对角缩放（core/VolumeRef.cpp：out = (i*sx, j*sy, k*sz)），
        //   2) 盒本身轴对齐，
        // 角点命中 <=> 该轴的两个半角端点各自落在区间内，于是
        //   hit = hx*hy*hz（hx/hy/hz 取值 0/1/2），weight = hit/8 = (hx/2)(hy/2)(hz/2)。
        // 0/0.5/1 都是二进制精确值，两种算法逐位相等，所以这里只换掉循环结构、
        // 不改变任何统计口径（scanned 也用乘量补齐，保证回归日志对得上）。
        // hy/hz 只随 j/k 变化，提到内层循环之外；整行为 0 时直接跳过整行。
        const double loX = std::min(roi.boxMin.x, roi.boxMax.x);
        const double hiX = std::max(roi.boxMin.x, roi.boxMax.x);
        const double loY = std::min(roi.boxMin.y, roi.boxMax.y);
        const double hiY = std::max(roi.boxMin.y, roi.boxMax.y);
        const double loZ = std::min(roi.boxMin.z, roi.boxMax.z);
        const double hiZ = std::max(roi.boxMin.z, roi.boxMax.z);
        const double sx = vol_.spacingX(), sy = vol_.spacingY(), sz = vol_.spacingZ();
        const double hx0 = sx * 0.5, hy0 = sy * 0.5, hz0 = sz * 0.5;
        const long long nx = (long long) i1 - i0 + 1;
        const long long ny = (long long) j1 - j0 + 1;
        // 每个半角端点都要同时满足 lo<=v<=hi（只比单边会把"已越过另一边"的端点也算命中）。
        // "只比一边"的写法曾被主机等价用例抓到：非对齐盒上快速路径的 coverage
        // 与角点定义相差 320/14091，见 test_main.cpp "R-02 快速路径==角点定义"。
        auto hitOn = [](double lo, double hi, double v0, double v1) {
            return ((v0 >= lo && v0 <= hi) ? 1 : 0) + ((v1 >= lo && v1 <= hi) ? 1 : 0);
        };
        for (int k = k0; k <= k1; ++k) {
            const double cz = (double) k * sz;
            const int hz = hitOn(loZ, hiZ, cz - hz0, cz + hz0);
            if (hz == 0) { scanned += nx * ny; continue; }
            for (int j = j0; j <= j1; ++j) {
                const double cy = (double) j * sy;
                const int hy = hitOn(loY, hiY, cy - hy0, cy + hy0);
                const int rowHit = hy * hz;
                const size_t rowBase = (size_t) k * slice + (size_t) j * W;
                if (rowHit == 0) { scanned += nx; continue; }
                for (int i = i0; i <= i1; ++i) {
                    ++scanned;
                    const double cx = (double) i * sx;
                    const int hx = hitOn(loX, hiX, cx - hx0, cx + hx0);
                    const double w = (double) (hx * rowHit) / 8.0;
                    if (w <= 0.0) continue;
                    accumulate(w, data[rowBase + i]);
                }
            }
        }
    } else {
        for (int k = k0; k <= k1; ++k) {
            for (int j = j0; j <= j1; ++j) {
                const size_t rowBase = (size_t) k * slice + (size_t) j * W;
                for (int i = i0; i <= i1; ++i) {
                    ++scanned;
                    const double w = roiWeight(roi, i, j, k, vol_, pool, 0, &err);
                    if (w <= 0.0) continue;
                    accumulate(w, data[rowBase + i]);
                }
            }
        }
    }

    s.coverageVoxels = sumW;
    s.fullVoxels = full;
    s.scannedVoxels = scanned;
    s.volumeMm3 = sumW * s.voxelMm3;
    s.volumeCm3 = s.volumeMm3 / 1000.0;
    if (sumW > 0.0) {
        s.meanHu = sumHW / sumW;
        const double var = sumHHW / sumW - s.meanHu * s.meanHu;
        s.sdHu = var > 0.0 ? std::sqrt(var) : 0.0;
        s.minHu = minH;
        s.maxHu = maxH;
        s.hasHu = true;
    }
    if (!roi.polygon.empty()) {
        s.areaMm2 = polygonAreaMm2(roi);
        s.areaCm2 = s.areaMm2 / 100.0;
    }
    s.elapsedMs = monoMs() - t0;
    if (!err.empty()) s.error = err;
    if (sumW <= 0.0 && s.error.empty()) s.error = "ROI 内无满足条件的体素";
    s.ok = (sumW > 0.0);

    LOGD("ROI #%d %s analyze: scanned=%lld coverage=%.1f vol=%.3fcm3 mean=%.1fHU in %lld ms",
         roi.id, roi.name.c_str(), scanned, sumW, s.volumeCm3, s.meanHu, s.elapsedMs);
    return s;
}
