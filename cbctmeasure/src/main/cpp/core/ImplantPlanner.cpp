// 种植体规划算法实现（PRD 5.3.2 S-01 ~ S-06）。
//
// 纯 C++ 体数据采样 + 解析几何，无 VTK / 无 JNI：
//   - 圆柱与螺纹轮廓取代 vtkCylinderSource + vtkTransform（见模块 CMakeLists 的
//     "不链接 VTK"决策），叠加层由 Android Canvas 按同一套世界坐标绘制；
//   - 安全距离取代 vtkImplicitPolyDataDistance（PRD 5.3.5 允许"点到线段/管道的
//     距离公式"），此处用线段-线段最近距离解析解，精度可控且可离线单测。

#include "include/ImplantPlanner.h"
#include "include/MeasureMath.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>

#define TAG "CbctMeasureCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

const double ImplantPlanner::kMinBoneHeightMm = 10.0;
const double ImplantPlanner::kMinBoneWidthMm = 6.0;
const double ImplantPlanner::kMinNerveDistMm = 2.0;
const double ImplantPlanner::kMinSpacingMm = 3.0;
const double ImplantPlanner::kMarginalFactor = 1.2;

ImplantPlanner::ImplantPlanner(const VolumeRef &vol, double boneHuThreshold)
        : vol_(vol), boneHu_(boneHuThreshold) {}

Vec3 ImplantPlanner::axisFromAngles(double pitchDeg, double yawDeg) {
    const double p = MeasureMath::degToRad(pitchDeg);
    const double y = MeasureMath::degToRad(yawDeg);
    // 基准姿态 (0,0,-1)：先绕 X 转 pitch，再绕 Y 转 yaw（右手系，推导见头文件）
    return Vec3(-std::cos(p) * std::sin(y), std::sin(p), -std::cos(p) * std::cos(y));
}

void ImplantPlanner::refreshGeometry(Implant &im) {
    const double len = im.lengthMm > 0.0 ? im.lengthMm : im.depthMm;
    im.axis = axisFromAngles(im.pitchDeg, im.yawDeg);
    im.tip = im.entry + im.axis * len;
}

/**
 * 沿 dir 方向从 q 开始累计"连续骨段"长度（mm）。
 * allowGapSteps 允许跨过的非骨步数：皮质骨内壁、部分容积体素会造成单步掉出，
 * 不允许跨隙会把骨高度系统性低估一个体素，允许过大会跨过真正的骨髓腔，
 * 取 2 步（<= 一个体素直径）是折中。
 */
double ImplantPlanner::marchBone(const Vec3 &q, const Vec3 &dir, double maxMm,
                                 int allowGapSteps) const {
    if (!vol_.valid() || maxMm <= 0.0) return 0.0;
    double step = std::min(vol_.spacingX(), std::min(vol_.spacingY(), vol_.spacingZ()));
    if (step <= 1e-6) step = 1.0;
    step *= 0.5;                                    // 亚体素步长，量化误差 <= 半步长

    double dist = 0.0;
    int gap = 0;
    bool inBone = vol_.huTrilinear(q, -2000.0f) >= (float) boneHu_;
    while (dist < maxMm) {
        dist += step;
        const Vec3 p = q + dir * dist;
        if (!vol_.worldInBounds(p)) return inBone ? dist : 0.0;
        const bool b = vol_.huTrilinear(p, -2000.0f) >= (float) boneHu_;
        if (b) { gap = 0; inBone = true; continue; }
        if (!inBone) return 0.0;                    // 起点就不在骨内
        if (++gap > allowGapSteps) return dist - (double) gap * step;
    }
    return dist;
}

double ImplantPlanner::boneHeight(const Implant &im, double *outSurfaceDepth) const {
    if (!vol_.valid()) return 0.0;
    Vec3 axis = im.axis;
    if (axis.length() < 1e-12) axis = axisFromAngles(im.pitchDeg, im.yawDeg);
    const double maxSearch = std::max(vol_.maxDiagonalMm(), 60.0);
    const double step = std::min(vol_.spacingX(), std::min(vol_.spacingY(), vol_.spacingZ())) * 0.5;

    // 相位 1：从入口沿轴前进，跳过软组织/牙龈，找到首个骨体素
    double surface = 0.0;
    bool found = false;
    while (surface < maxSearch) {
        const Vec3 p = im.entry + axis * surface;
        if (!vol_.worldInBounds(p)) break;
        if (vol_.huTrilinear(p, -2000.0f) >= (float) boneHu_) { found = true; break; }
        surface += step;
    }
    if (outSurfaceDepth) *outSurfaceDepth = surface;
    if (!found) return 0.0;

    // 相位 2：从骨面起累计连续骨段 = 可用骨高度
    const Vec3 start = im.entry + axis * surface;
    const double run = marchBone(start, axis, maxSearch - surface, 2);
    return run;
}

/**
 * 在植入点处构造"垂直于轴线"的两个正交方向。
 * 参考轴优先取世界 X（轴层面内），当轴线接近 X 时改用 Y，
 * 保证 u/v 始终落在与解剖颊舌向/近远中向一致的平面内。
 */
static void perpendicularBasis(const Vec3 &axisIn, Vec3 &u, Vec3 &v) {
    const Vec3 axis = axisIn.normalized();
    Vec3 ref(1, 0, 0);
    if (std::fabs(axis.dot(ref)) > 0.9) ref = Vec3(0, 1, 0);
    if (std::fabs(axis.dot(ref)) > 0.9) ref = Vec3(0, 0, 1);
    u = (ref - axis * axis.dot(ref)).normalized();
    v = axis.cross(u).normalized();
}

double ImplantPlanner::boneWidth(const Implant &im, double *outU, double *outV) const {
    if (!vol_.valid()) return 0.0;
    Vec3 axis = im.axis;
    if (axis.length() < 1e-12) axis = axisFromAngles(im.pitchDeg, im.yawDeg);
    // 测量截面取植入深度的 1/3 处：过浅会量到牙槽嵴顶的外扩、
    // 过深则进入根尖区，1/3 处与临床"颈部骨宽度"最接近
    const double at = im.depthMm > 0.0 ? im.depthMm / 3.0 : 3.0;
    const Vec3 q = im.entry + axis * at;
    Vec3 u, v;
    perpendicularBasis(axis, u, v);

    const double maxMm = std::max(vol_.maxDiagonalMm(), 60.0);
    const double wu = marchBone(q, u, maxMm, 1) + marchBone(q, u * -1.0, maxMm, 1);
    const double wv = marchBone(q, v, maxMm, 1) + marchBone(q, v * -1.0, maxMm, 1);
    if (outU) *outU = wu;
    if (outV) *outV = wv;
    // 保守取较小者：桩体能否放入受限于最窄的那个方向
    return std::min(wu, wv);
}

double ImplantPlanner::nerveDistance(const Implant &im, const NervePath &nerve) const {
    if (nerve.points.size() < 2) return -1.0;
    Vec3 axis = im.axis;
    if (axis.length() < 1e-12) axis = axisFromAngles(im.pitchDeg, im.yawDeg);
    const Vec3 tip = (im.tip - im.entry).length() > 1e-9 ? im.tip
                                                         : im.entry + axis * im.lengthMm;
    // 桩体表面到管道中心线：先算轴线-折线最短距离，再减管道半径
    const double d = MeasureMath::polylineSegmentDistance(nerve.points, im.entry, tip);
    if (d < 0.0) return -1.0;
    const double surface = d - nerve.radiusMm;
    LOGD("implant #%d nerve distance: axis=%.2fmm radius=%.2f -> %.2fmm",
         im.id, d, nerve.radiusMm, surface);
    return surface < 0.0 ? 0.0 : surface;
}

double ImplantPlanner::minSpacing(const Implant &im, const std::vector<Implant> &all) const {
    Vec3 axis = im.axis;
    if (axis.length() < 1e-12) axis = axisFromAngles(im.pitchDeg, im.yawDeg);
    const Vec3 tipI = im.entry + axis * im.lengthMm;
    double best = -1.0;
    for (size_t i = 0; i < all.size(); ++i) {
        const Implant &o = all[i];
        if (o.id == im.id) continue;
        Vec3 axisO = o.axis;
        if (axisO.length() < 1e-12) axisO = axisFromAngles(o.pitchDeg, o.yawDeg);
        const Vec3 tipO = o.entry + axisO * o.lengthMm;
        const double axisDist = MeasureMath::segmentSegment(im.entry, tipI, o.entry, tipO);
        // 临床间距指种植体表面之间的距离，因此扣掉两个半径
        const double surf = axisDist - (im.diaMm + o.diaMm) * 0.5;
        if (best < 0.0 || surf < best) best = surf;
    }
    return best;
}

int ImplantPlanner::safetyLevel(double boneH, double boneW, double nerve, bool nerveOk,
                                double spacing, bool spacingOk) {
    int level = SAFE_GREEN;
    auto judge = [&](double value, double threshold) {
        if (value < threshold) level = std::max(level, (int) DANGER_RED);
        else if (value < threshold * kMarginalFactor) level = std::max(level, (int) WARN_YELLOW);
    };
    judge(boneH, kMinBoneHeightMm);
    judge(boneW, kMinBoneWidthMm);
    if (nerveOk) judge(nerve, kMinNerveDistMm);
    if (spacingOk) judge(spacing, kMinSpacingMm);
    return level;
}

std::string ImplantPlanner::warningText(const ImplantSafety &s) {
    std::string out;
    char buf[128];
    auto add = [&](const char *prefix, double value, double threshold, const char *unit) {
        snprintf(buf, sizeof(buf), "%s%.1f%s（安全值 >= %.1f%s）; ",
                 prefix, value, unit, threshold, unit);
        out += buf;
    };
    if (s.boneHeightMm < kMinBoneHeightMm) add("骨高度不足 ", s.boneHeightMm, kMinBoneHeightMm, "mm");
    if (s.boneWidthMm < kMinBoneWidthMm) add("骨宽度不足 ", s.boneWidthMm, kMinBoneWidthMm, "mm");
    if (s.nerveMeasured && s.nerveDistMm < kMinNerveDistMm)
        add("神经管距离危险 ", s.nerveDistMm, kMinNerveDistMm, "mm");
    else if (s.nerveMeasured && s.nerveDistMm < kMinNerveDistMm * kMarginalFactor)
        add("神经管距离临界 ", s.nerveDistMm, kMinNerveDistMm, "mm");
    if (s.spacingMeasured && s.spacingMm < kMinSpacingMm)
        add("种植体间距不足 ", s.spacingMm, kMinSpacingMm, "mm");
    if (out.empty()) {
        snprintf(buf, sizeof(buf), "各指标均在安全范围内（骨高 %.1fmm / 骨宽 %.1fmm）",
                 s.boneHeightMm, s.boneWidthMm);
        out = buf;
    } else if (!s.nerveMeasured) {
        out += "未勾画神经管，S-05 未参与判定; ";
    }
    return out;
}

ImplantSafety ImplantPlanner::evaluate(Implant &im, const std::vector<NervePath> &nerves,
                                       const std::vector<Implant> &all) const {
    refreshGeometry(im);
    ImplantSafety s;
    s.boneHeightMm = boneHeight(im);
    s.boneWidthMm = boneWidth(im, &s.boneWidthAlongU, &s.boneWidthAlongV);
    for (size_t i = 0; i < nerves.size(); ++i) {
        const double d = nerveDistance(im, nerves[i]);
        if (d < 0.0) continue;
        if (!s.nerveMeasured || d < s.nerveDistMm) { s.nerveDistMm = d; s.nerveMeasured = true; }
    }
    const double sp = minSpacing(im, all);
    if (sp >= 0.0) { s.spacingMm = sp; s.spacingMeasured = true; }

    s.level = safetyLevel(s.boneHeightMm, s.boneWidthMm, s.nerveDistMm, s.nerveMeasured,
                          s.spacingMm, s.spacingMeasured);
    s.warnText = warningText(s);

    im.boneHeightMm = s.boneHeightMm;
    im.boneWidthMm = s.boneWidthMm;
    im.nerveDistMm = s.nerveMeasured ? s.nerveDistMm : -1.0;
    im.minSpacingMm = s.spacingMeasured ? s.spacingMm : -1.0;
    im.level = s.level;
    im.warnText = s.warnText;
    LOGD("implant #%d eval: height=%.2f width=%.2f nerve=%.2f(measured=%d) spacing=%.2f level=%d",
         im.id, s.boneHeightMm, s.boneWidthMm, s.nerveDistMm, (int) s.nerveMeasured,
         s.spacingMm, s.level);
    return s;
}

void ImplantPlanner::capsuleOutline(const Implant &im, int segments, std::vector<Vec3> &out) {
    out.clear();
    Vec3 axis = im.axis;
    if (axis.length() < 1e-12) axis = axisFromAngles(im.pitchDeg, im.yawDeg);
    Vec3 u, v;
    perpendicularBasis(axis, u, v);
    const double r = im.diaMm * 0.5;
    const Vec3 tip = im.tip.length() > 0.0 && (im.tip - im.entry).length() > 1e-9
                     ? im.tip : im.entry + axis * im.lengthMm;
    if (segments < 3) segments = 24;
    for (int ring = 0; ring < 2; ++ring) {
        const Vec3 c = ring == 0 ? im.entry : tip;
        for (int i = 0; i < segments; ++i) {
            const double a = (double) i / segments * 2.0 * 3.14159265358979323846;
            out.push_back(c + (u * std::cos(a) + v * std::sin(a)) * r);
        }
    }
}

void ImplantPlanner::threadOutline(const Implant &im, int turns, int pointsPerTurn,
                                   std::vector<Vec3> &out) {
    out.clear();
    if (turns < 1) turns = 6;
    if (pointsPerTurn < 6) pointsPerTurn = 16;
    Vec3 axis = im.axis;
    if (axis.length() < 1e-12) axis = axisFromAngles(im.pitchDeg, im.yawDeg);
    Vec3 u, v;
    perpendicularBasis(axis, u, v);
    const double r = im.diaMm * 0.5;
    // 螺纹从冠方 1mm 起、到根方 1mm 止，避免与桩体端面轮廓重叠
    const double len = im.lengthMm > 2.0 ? im.lengthMm : 10.0;
    const double span = len - 2.0;
    const int total = turns * pointsPerTurn;
    for (int i = 0; i <= total; ++i) {
        const double t = (double) i / (double) total;
        const double a = t * (double) turns * 2.0 * 3.14159265358979323846;
        const Vec3 c = im.entry + axis * (1.0 + span * t);
        out.push_back(c + (u * std::cos(a) + v * std::sin(a)) * r);
    }
}
