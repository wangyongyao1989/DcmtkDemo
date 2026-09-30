// AI-03 种植位推荐实现（PRD 5.6.3：规则 + 机器学习混合）。
//
// 分工与 PRD 的对应关系（写报告时不要含糊）：
//   "机器学习"= AI-01 分割给出的每颗牙实例、牙弓归属与长轴（掩膜 PCA）。
//                 缺牙间隙完全是从这些几何量推出来的，没有分割就没有候选。
//   "规则"     = 净间隙阈值筛缺牙位、邻牙长轴加权得植入轴向、牙槽嵴顶高度估计、
//                 按 S-03/S-04 实测骨量反选植体直径与长度、安全打分排序。
//   安全判级没有另起一套：直接调 ImplantPlanner::evaluate，与手动放置的种植体
//   共用 S-02~S-06 的同一阈值表，所以推荐结果落库后数字不会变。

#include "include/AiPlanner.h"

#include "include/AiCore.h"
#include "include/ImplantPlanner.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#define TAG "CbctMeasureAi"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace {

    /** 轴对齐包围盒在方向 t（单位向量）上的投影宽度 = Σ|t_a|*(max-min) */
    double boxExtentAlong(const AiInstance &ins, const Vec3 &t) {
        const double dx = ins.bboxMax.x - ins.bboxMin.x;
        const double dy = ins.bboxMax.y - ins.bboxMin.y;
        const double dz = ins.bboxMax.z - ins.bboxMin.z;
        return std::fabs(t.x) * dx + std::fabs(t.y) * dy + std::fabs(t.z) * dz;
    }

    /** 把牙齿长轴定向为"从冠指向根"（PCA 只给直线方向，符号靠冠/根 z 判定） */
    Vec3 apicalAxis(const AiInstance &ins) {
        Vec3 a = ins.axis.normalized();
        const double apicalZ = ins.rootZ - ins.crownZ;     // 根端相对冠端的 z
        if (std::fabs(apicalZ) > 1e-6 && a.z * apicalZ < 0.0) a = a * (-1.0);
        else if (std::fabs(apicalZ) <= 1e-6 && a.z < 0.0) a = a * (-1.0);   // 退化：默认指向 +z
        return a;
    }

    /** 沿建议轴向在 entry 下 2~6mm 取样的平均 HU（骨密度线索，不是统计口径） */
    double sampleHuUnderGap(const VolumeRef &vol, const Vec3 &entry, const Vec3 &axis) {
        if (!vol.valid() || vol.data() == nullptr) return 0.0;
        double sum = 0.0;
        int n = 0;
        for (double d = 2.0; d <= 6.0 + 1e-9; d += 1.0) {
            const Vec3 p = entry + axis * d;
            if (!vol.worldInBounds(p)) continue;
            sum += (double) vol.huTrilinear(p, 0.0f);
            ++n;
        }
        return n > 0 ? sum / (double) n : 0.0;
    }

    /** 可选植体直径（常见牙科种植体系列；PRD 未指定，取临床通用值） */
    const double kDiaLadder[4] = {3.5, 4.0, 4.5, 5.0};

    /** 需要的最小颊舌/近远中骨宽度余量：植体两侧各 >=1mm（规则来源见头文件注释） */
    const double kMinWallMm = 2.0;
    /** 冠方到下牙槽神经的安全余量（与 ImplantPlanner::kMinNerveDistMm 同源语义） */
    const double kNerveClearanceMm = 2.0;

}   // namespace

namespace AiPlanner {

    void anglesFromAxis(const Vec3 &axis, double &pitchDeg, double &yawDeg) {
        // 正向：ImplantPlanner::axisFromAngles =
        //   (-cos p·sin y, sin p, -cos p·cos y)，p 绕 X、y 绕 Y。
        // 逆变换：p = asin(a.y)；a.x/a.z 消去 cos p 后 y = atan2(-a.x, -a.z)。
        // cos p 接近 0（轴向几乎水平）时 atan2 仍稳定，因为它只看比值不看模长。
        const Vec3 a = axis.normalized();
        const double sy = std::max(-1.0, std::min(1.0, a.y));
        pitchDeg = std::asin(sy) * 180.0 / M_PI;
        yawDeg = std::atan2(-a.x, -a.z) * 180.0 / M_PI;
    }

    void recommend(const AiResult &r, const VolumeRef &vol,
                   const std::vector<NervePath> &nerves,
                   const std::vector<Implant> &existing,
                   double minGapMm, int maxOut,
                   std::vector<AiCandidate> &out) {
        out.clear();
        if (minGapMm <= 0.0) minGapMm = 5.0;
        if (!r.hasMask()) {
            LOGW("recommend: 没有 AI-01 结果，无法生成候选");
            return;
        }

        // ---- 1) 按牙弓分组，弓内按牙位序号排列（archAssign 已给 toothCountHint）----
        std::vector<const AiInstance *> arches[2];
        for (size_t t = 0; t < r.instances.size(); ++t) {
            const AiInstance &ins = r.instances[t];
            if (ins.arch != 0 && ins.arch != 1) continue;
            arches[ins.arch].push_back(&ins);
        }
        for (int a = 0; a < 2; ++a) {
            std::sort(arches[a].begin(), arches[a].end(),
                      [](const AiInstance *x, const AiInstance *y) {
                          if (x->toothCountHint != y->toothCountHint)
                              return x->toothCountHint < y->toothCountHint;
                          return x->archAngleDeg < y->archAngleDeg;
                      });
        }

        ImplantPlanner planner(vol);
        const long long modelN = r.grid.modelCount();

        for (int a = 0; a < 2; ++a) {
            const std::vector<const AiInstance *> &list = arches[a];
            for (size_t t = 0; t + 1 < list.size(); ++t) {
                const AiInstance &A = *list[t];
                const AiInstance &B = *list[t + 1];
                Vec3 chord = B.centroid - A.centroid;
                const double centerDist = chord.length();
                if (centerDist < 1e-6) continue;
                const Vec3 tangent = chord.normalized();
                // 净间隙 = 两牙质心距离 - 两侧包围盒在该方向上的投影宽度之半。
                // 用投影宽度而不是固定牙冠宽度：掩膜包围盒是各向异性的（磨牙宽、
                // 切牙窄），写死常数会把磨牙区的正常邻接误判成缺牙间隙。
                const double gap = centerDist
                        - 0.5 * (boxExtentAlong(A, tangent) + boxExtentAlong(B, tangent));
                if (gap < minGapMm) continue;

                AiCandidate c;
                c.valid = true;
                c.arch = a;
                c.beforeId = A.id;
                c.afterId = B.id;
                c.gapMm = gap;

                // ---- 2) 入口：嵴顶高度 = 邻牙冠端均值再向根方 1mm ----
                // 牙槽嵴顶在临床上约位于邻牙颈缘水平；冠端（archAssign 里定义为
                // "离咬合平面更近的一端"）加 1mm 根方是最省假设的近似。
                const double apicalSign = ((B.rootZ - B.crownZ) + (A.rootZ - A.crownZ)) >= 0.0
                                          ? 1.0 : -1.0;
                const double crownZ = 0.5 * (A.crownZ + B.crownZ);
                c.entry = Vec3(0.5 * (A.centroid.x + B.centroid.x),
                               0.5 * (A.centroid.y + B.centroid.y),
                               crownZ + apicalSign * 1.0);
                if (!vol.worldInBounds(c.entry)) {
                    c.valid = false;
                    c.reason = "嵴顶估计点落在体数据范围外";
                    continue;
                }

                // ---- 3) 轴向：邻牙长轴（指向根方）的加权平均 ----
                Vec3 ax = apicalAxis(A) * (A.voxels > 0 ? (double) A.voxels : 1.0)
                        + apicalAxis(B) * (B.voxels > 0 ? (double) B.voxels : 1.0);
                if (ax.length() < 1e-6) ax = Vec3(0, 0, apicalSign);
                c.axis = ax.normalized();
                anglesFromAxis(c.axis, c.pitchDeg, c.yawDeg);

                // ---- 4) 第一次安全实测（用默认规格），再由骨量反选规格 ----
                Implant tmp;
                tmp.id = 0;
                tmp.name = "AI 候选";
                tmp.entry = c.entry;
                tmp.pitchDeg = c.pitchDeg;
                tmp.yawDeg = c.yawDeg;
                tmp.diaMm = 4.0;
                tmp.lengthMm = 11.0;
                tmp.depthMm = 10.0;
                std::vector<Implant> pool = existing;
                pool.push_back(tmp);
                ImplantSafety s1 = planner.evaluate(tmp, nerves, pool);
                c.boneHeightMm = s1.boneHeightMm;
                c.boneWidthMm = s1.boneWidthMm;
                c.nerveDistMm = s1.nerveMeasured ? s1.nerveDistMm : -1.0;

                double dia = kDiaLadder[0];
                for (int q = 0; q < 4; ++q) {
                    if (c.boneWidthMm - kMinWallMm >= kDiaLadder[q]) dia = kDiaLadder[q];
                }
                double len = c.boneHeightMm - kNerveClearanceMm;
                if (len > 13.0) len = 13.0;
                if (len < 6.0) len = 6.0;
                c.diaMm = dia;
                c.lengthMm = len;
                c.depthMm = std::min(len, std::max(0.0, c.boneHeightMm - kNerveClearanceMm));

                // ---- 5) 规格定了再复算一次：长度变了尖端位置就变了，S-05 必须重测 ----
                tmp.diaMm = c.diaMm;
                tmp.lengthMm = c.lengthMm;
                tmp.depthMm = c.depthMm;
                pool.back() = tmp;
                ImplantSafety s2 = planner.evaluate(tmp, nerves, pool);
                c.boneHeightMm = s2.boneHeightMm;
                c.boneWidthMm = s2.boneWidthMm;
                c.nerveDistMm = s2.nerveMeasured ? s2.nerveDistMm : -1.0;
                c.level = s2.level;
                c.meanHuUnderGap = sampleHuUnderGap(vol, c.entry, c.axis);

                // ---- 6) 打分（权重写死并写进报告，便于复算与审阅）----
                //   骨高 35 / 骨宽 25 / 间隙 25 / 神经距离 15；无神经勾画时该项给 5 分中性值
                double sc = 0.0;
                sc += 35.0 * std::min(1.0, c.boneHeightMm / 12.0);
                sc += 25.0 * std::min(1.0, c.boneWidthMm / 8.0);
                sc += 25.0 * std::min(1.0, gap / 8.0);
                sc += (c.nerveDistMm >= 0.0) ? 15.0 * std::min(1.0, c.nerveDistMm / 5.0) : 5.0;
                c.score = (int) (sc + 0.5);

                char buf[256];
                snprintf(buf, sizeof(buf),
                         "%s缺牙间隙 %.1fmm（AI#%d↔#%d）；骨高 %.1f / 骨宽 %.1f / 间隙下 HU %.0f；建议 Ø%.1f×%.1fmm",
                         a == 0 ? "上颌" : "下颌", gap, A.id, B.id,
                         c.boneHeightMm, c.boneWidthMm, c.meanHuUnderGap, c.diaMm, c.lengthMm);
                c.reason = buf;
                if (c.boneHeightMm < ImplantPlanner::kMinBoneHeightMm) {
                    c.reason += "；骨高不足，需植骨或改短桩";
                }
                if (c.gapMm < 0.0) c.reason += "；间隙为负（邻牙倾斜遮挡）";
                (void) modelN;
                out.push_back(c);
            }
        }

        std::sort(out.begin(), out.end(), [](const AiCandidate &x, const AiCandidate &y) {
            if (x.score != y.score) return x.score > y.score;
            return x.gapMm > y.gapMm;
        });
        if (maxOut > 0 && (int) out.size() > maxOut) out.resize((size_t) maxOut);
        LOGD("recommend: %zu candidates (minGap=%.1f, arch0=%zu arch1=%zu)",
             out.size(), minGapMm, arches[0].size(), arches[1].size());
    }

    void toImplant(const AiCandidate &c, int index, Implant &out) {
        // id 由 MeasurementManager::addImplant 分配，这里不碰
        char nm[64];
        snprintf(nm, sizeof(nm), "AI 种植位 %s%d", c.arch == 0 ? "上" : "下", index + 1);
        out.name = nm;
        out.color = 0xFF00E676U;
        out.visible = true;
        out.entry = c.entry;
        out.pitchDeg = c.pitchDeg;
        out.yawDeg = c.yawDeg;
        out.diaMm = c.diaMm;
        out.lengthMm = c.lengthMm;
        out.depthMm = c.depthMm;
        out.axis = c.axis;
    }

    Json candidatesJson(const std::vector<AiCandidate> &list) {
        Json arr = Json::makeArray();
        for (size_t t = 0; t < list.size(); ++t) {
            const AiCandidate &c = list[t];
            Json o = Json::makeObject();
            o.set("valid", Json::makeBool(c.valid));
            o.set("arch", Json::makeNumber(c.arch));
            o.set("beforeId", Json::makeNumber(c.beforeId));
            o.set("afterId", Json::makeNumber(c.afterId));
            o.set("gap", Json::makeNumber(c.gapMm));
            Json e = Json::makeArray();
            e.push(Json::makeNumber(c.entry.x));
            e.push(Json::makeNumber(c.entry.y));
            e.push(Json::makeNumber(c.entry.z));
            o.set("entry", e);
            Json ax = Json::makeArray();
            ax.push(Json::makeNumber(c.axis.x));
            ax.push(Json::makeNumber(c.axis.y));
            ax.push(Json::makeNumber(c.axis.z));
            o.set("axis", ax);
            o.set("pitch", Json::makeNumber(c.pitchDeg));
            o.set("yaw", Json::makeNumber(c.yawDeg));
            o.set("dia", Json::makeNumber(c.diaMm));
            o.set("length", Json::makeNumber(c.lengthMm));
            o.set("depth", Json::makeNumber(c.depthMm));
            o.set("meanHu", Json::makeNumber(c.meanHuUnderGap));
            o.set("score", Json::makeNumber(c.score));
            o.set("boneHeight", Json::makeNumber(c.boneHeightMm));
            o.set("boneWidth", Json::makeNumber(c.boneWidthMm));
            o.set("nerveDist", Json::makeNumber(c.nerveDistMm));
            o.set("level", Json::makeNumber(c.level));
            o.set("implantId", Json::makeNumber(c.implantId));
            o.set("reason", Json::makeString(c.reason));
            arr.push(o);
        }
        return arr;
    }

    std::string summaryText(const std::vector<AiCandidate> &list) {
        if (list.empty()) return "AI-03: 未发现可用缺牙间隙（净间隙 < 阈值或分割无结果）";
        char buf[160];
        snprintf(buf, sizeof(buf), "AI-03: %zu 个候选位，最高分 %d（%s）",
                 list.size(), list[0].score, list[0].arch == 0 ? "上颌" : "下颌");
        return std::string(buf);
    }

}   // namespace AiPlanner
