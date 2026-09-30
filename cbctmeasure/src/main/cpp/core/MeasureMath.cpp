// 纯几何 / 统计测量算法实现（PRD 5.1、5.3.3）。
//
// 本文件与 Android / JNI / VTK 无关，可在主机上直接编译运行单测，
// 精度断言（AC-01 距离 <= 0.1mm、AC-02 角度 <= 0.5 度、AC-04 体积 <= 1%）
// 全部落在这里。

#include "include/MeasureMath.h"

#include <cmath>
#include <cstdio>

namespace MeasureMath {

    double radToDeg(double rad) { return rad * (180.0 / 3.14159265358979323846); }
    double degToRad(double deg) { return deg * (3.14159265358979323846 / 180.0); }

    double distance(const Vec3 &a, const Vec3 &b) { return (b - a).length(); }

    double angle(const Vec3 &a, const Vec3 &vertex, const Vec3 &c) {
        const Vec3 u = a - vertex;
        const Vec3 v = c - vertex;
        const double lu = u.length(), lv = v.length();
        if (lu < 1e-12 || lv < 1e-12) return 0.0;
        const Vec3 un = u * (1.0 / lu), vn = v * (1.0 / lv);
        const double crossLen = un.cross(vn).length();
        const double dot = un.dot(vn);
        // atan2 而非 acos：0/180 度附近同样稳定（见头文件注释）
        return radToDeg(std::atan2(crossLen, dot));
    }

    double pointToSegment(const Vec3 &p, const Vec3 &a, const Vec3 &b, Vec3 *foot) {
        const Vec3 ab = b - a;
        const double len2 = ab.dot(ab);
        if (len2 < 1e-24) {
            if (foot) *foot = a;
            return distance(p, a);
        }
        double t = (p - a).dot(ab) / len2;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        const Vec3 f = a + ab * t;
        if (foot) *foot = f;
        return distance(p, f);
    }

    double pointToLine(const Vec3 &p, const Vec3 &a, const Vec3 &b, Vec3 *foot) {
        const Vec3 ab = b - a;
        const double len2 = ab.dot(ab);
        if (len2 < 1e-24) {
            if (foot) *foot = a;
            return distance(p, a);
        }
        const double t = (p - a).dot(ab) / len2;
        const Vec3 f = a + ab * t;
        if (foot) *foot = f;
        return distance(p, f);
    }

    double arcLength(const std::vector<Vec3> &pts, bool closed) {
        double sum = 0.0;
        for (size_t i = 1; i < pts.size(); ++i) sum += distance(pts[i - 1], pts[i]);
        if (closed && pts.size() >= 3) sum += distance(pts.back(), pts.front());
        return sum;
    }

    /**
     * 面积/质心：把多边形投影到 Newell 法向的最佳拟合平面后做鞋带公式。
     * 顶点均值 q0 作为平面参考点，使投影坐标量级小、浮点抵消误差低。
     */
    static bool polygonPlanar(const std::vector<Vec3> &pts,
                              double &area, Vec3 &centroid) {
        area = 0.0;
        centroid = Vec3();
        const size_t n = pts.size();
        if (n < 3) return false;

        // Newell 法向
        Vec3 nn(0, 0, 0);
        for (size_t i = 0; i < n; ++i) {
            const Vec3 &cur = pts[i];
            const Vec3 &nxt = pts[(i + 1) % n];
            nn.x += (cur.y - nxt.y) * (cur.z + nxt.z);
            nn.y += (cur.z - nxt.z) * (cur.x + nxt.x);
            nn.z += (cur.x - nxt.x) * (cur.y + nxt.y);
        }
        const double nl = nn.length();
        if (nl < 1e-12) return false;                 // 退化（重合点/自交抵消）
        const Vec3 nv = nn * (1.0 / nl);

        // 法向的正交基：选与法向点积最小的世界轴做叉乘，数值最稳
        Vec3 ref(1, 0, 0);
        if (std::fabs(nv.dot(ref)) > 0.9) ref = Vec3(0, 1, 0);
        if (std::fabs(nv.dot(ref)) > 0.9) ref = Vec3(0, 0, 1);
        const Vec3 u = nv.cross(ref).normalized();
        const Vec3 v = nv.cross(u);

        Vec3 q0(0, 0, 0);
        for (size_t i = 0; i < n; ++i) q0 = q0 + pts[i];
        q0 = q0 * (1.0 / (double) n);

        double shoelace = 0.0, cx = 0.0, cy = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const Vec3 d0 = pts[i] - q0;
            const Vec3 d1 = pts[(i + 1) % n] - q0;
            const double x0 = d0.dot(u), y0 = d0.dot(v);
            const double x1 = d1.dot(u), y1 = d1.dot(v);
            const double w = x0 * y1 - x1 * y0;
            shoelace += w;
            cx += (x0 + x1) * w;
            cy += (y0 + y1) * w;
        }
        const double a = shoelace * 0.5;
        area = std::fabs(a);
        if (std::fabs(shoelace) < 1e-12) {            // 质心退化 -> 顶点均值
            centroid = q0;
            return area > 0.0;
        }
        const double k = 1.0 / (6.0 * a);
        centroid = q0 + u * (cx * k) + v * (cy * k);
        return true;
    }

    double polygonArea(const std::vector<Vec3> &pts) {
        double area = 0.0;
        Vec3 c;
        if (!polygonPlanar(pts, area, c)) {
            // 共线/点不足：面积为 0（UI 侧提示"路径未闭合或过小"）
            return 0.0;
        }
        return area;
    }

    Vec3 polygonCentroid(const std::vector<Vec3> &pts) {
        double area = 0.0;
        Vec3 c;
        if (polygonPlanar(pts, area, c)) return c;
        if (pts.empty()) return Vec3();
        Vec3 sum(0, 0, 0);
        for (size_t i = 0; i < pts.size(); ++i) sum = sum + pts[i];
        return sum * (1.0 / (double) pts.size());
    }

    double lineDeviationDeg(const Vec3 &l1a, const Vec3 &l1b, const Vec3 &l2a, const Vec3 &l2b) {
        const Vec3 d1 = (l1b - l1a).normalized();
        const Vec3 d2 = (l2b - l2a).normalized();
        if (d1.length() < 1e-12 || d2.length() < 1e-12) return 0.0;
        // 方向线无正反语义：点积取绝对值，夹角折到 [0, 90]
        const double c = std::fabs(d1.dot(d2));
        return radToDeg(std::atan2(d1.cross(d2).length(), std::min(1.0, c)));
    }

    /**
     * 两线段最短距离（Eberly 算法，参数域夹紧到 [0,1]）。
     * 五种退化情形分别处理：两短线段、一条退化、端点接触等，
     * 最终都归一到 clamp 后的最近点对，因此不会出现 NaN。
     */
    double segmentSegment(const Vec3 &p0, const Vec3 &p1, const Vec3 &q0, const Vec3 &q1,
                          Vec3 *outA, Vec3 *outB) {
        const Vec3 d1 = p1 - p0;
        const Vec3 d2 = q1 - q0;
        const Vec3 r = p0 - q0;
        const double a = d1.dot(d1);      // >=0
        const double e = d2.dot(d2);
        const double f = d2.dot(r);

        double s = 0.0, t = 0.0;
        if (a <= 1e-24 && e <= 1e-24) {   // 两线段都退化为点
            s = t = 0.0;
        } else if (a <= 1e-24) {          // 第一条退化：点到线段
            s = 0.0;
            t = f / e;
            t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        } else {
            const double c = d1.dot(r);
            if (e <= 1e-24) {             // 第二条退化：点到线段
                t = 0.0;
                s = -c / a;
                s = s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
            } else {
                const double b = d1.dot(d2);
                const double denom = a * e - b * b;   // 恒 >= 0
                if (denom > 1e-12) s = std::min(std::max((b * f - c * e) / denom, 0.0), 1.0);
                t = (b * s + f) / e;
                if (t < 0.0) {
                    t = 0.0;
                    s = std::min(std::max(-c / a, 0.0), 1.0);
                } else if (t > 1.0) {
                    t = 1.0;
                    s = std::min(std::max((b - c) / a, 0.0), 1.0);
                }
            }
        }
        const Vec3 ca = p0 + d1 * s;
        const Vec3 cb = q0 + d2 * t;
        if (outA) *outA = ca;
        if (outB) *outB = cb;
        return distance(ca, cb);
    }

    double polylineSegmentDistance(const std::vector<Vec3> &poly, const Vec3 &a, const Vec3 &b) {
        if (poly.size() < 2) return -1.0;
        double best = 1e300;
        for (size_t i = 1; i < poly.size(); ++i) {
            const double d = segmentSegment(poly[i - 1], poly[i], a, b);
            if (d < best) best = d;
        }
        return best;
    }

    double polylinePolylineDistance(const std::vector<Vec3> &pa, const std::vector<Vec3> &pb) {
        if (pa.size() < 2 || pb.size() < 2) return -1.0;
        double best = 1e300;
        for (size_t i = 1; i < pa.size(); ++i) {
            const double d = polylineSegmentDistance(pb, pa[i - 1], pa[i]);
            if (d >= 0.0 && d < best) best = d;
        }
        return best;
    }

    Vec3 pointAtArcFraction(const std::vector<Vec3> &pts, double fraction) {        if (pts.empty()) return Vec3();
        if (pts.size() == 1) return pts[0];
        double f = fraction;
        f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
        const double total = arcLength(pts, false);
        if (total < 1e-12) return pts[0];
        double target = total * f;
        double acc = 0.0;
        for (size_t i = 1; i < pts.size(); ++i) {
            const double seg = distance(pts[i - 1], pts[i]);
            if (acc + seg >= target) {
                const double t = seg > 1e-12 ? (target - acc) / seg : 0.0;
                return pts[i - 1] + (pts[i] - pts[i - 1]) * t;
            }
            acc += seg;
        }
        return pts.back();
    }

    double midlineOffset(const std::vector<Vec3> &upperArc, const std::vector<Vec3> &lowerArc) {
        if (upperArc.size() < 2 || lowerArc.size() < 2) return 0.0;
        const Vec3 mu = pointAtArcFraction(upperArc, 0.5);
        const Vec3 ml = pointAtArcFraction(lowerArc, 0.5);
        // 只取水平分量：垂直差属于覆合（overbite），不算中线偏移
        return std::sqrt(std::pow(mu.x - ml.x, 2.0) + std::pow(mu.y - ml.y, 2.0));
    }

    void overbiteOverjet(const Vec3 &upperIncisal, const Vec3 &lowerIncisal,
                         int apAxis, double &overjetMm, double &overbiteMm) {
        const Vec3 d = upperIncisal - lowerIncisal;
        double ap = d.x;
        if (apAxis == 1) ap = d.y;
        overjetMm = std::fabs(ap);
        overbiteMm = d.z;
    }

    std::string huSampleText(const VolumeRef &vol, const Vec3 &p) {
        if (!vol.valid()) return "no volume";
        const float hu = vol.huTrilinear(p, -1000.0f);
        char buf[96];
        snprintf(buf, sizeof(buf), "%.0f HU (%s)", (double) hu,
                 VolumeRef::tissueName(hu).c_str());
        return std::string(buf);
    }

    std::string format(double value, int digits) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f", digits, value);
        return std::string(buf);
    }

}   // namespace MeasureMath
