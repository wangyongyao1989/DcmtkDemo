// 第三磨牙-下牙槽神经风险度量（升级建议 P-10 的 MVP）。
//
// 纯 C++11 + VolumeRef 只读取样，主机侧可编译单测（src/host/test_main.cpp 的
// 组15），因此"距离算错/皮质骨取样口径写反"这类问题在主机上就能定死，
// 不必等到真机上靠肉眼比对叠加层。

#include "include/NerveRisk.h"
#include "include/MeasureMath.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#define TAG "CbctMeasureCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace {

    /** 骨取样的 HU 口径：PRD R-01「骨分割 >200HU」，与 ImplantPlanner 的默认值同源。 */
    const double kBonesHu = 200.0;

    /** 点到折线（逐段取最小），同时带回最近点 */
    double pointToPolyline(const std::vector<Vec3> &poly, const Vec3 &p, Vec3 *foot) {
        double best = 1e300;
        Vec3 bestFoot;
        for (size_t i = 0; i + 1 < poly.size(); ++i) {
            Vec3 f;
            const double d = MeasureMath::pointToSegment(p, poly[i], poly[i + 1], &f);
            if (d < best) {
                best = d;
                bestFoot = f;
            }
        }
        if (foot) *foot = bestFoot;
        return best;
    }

    /** 世界点到轴对齐盒的距离（盒内为 0）：剪枝用的**下界**，不是距离本身 */
    double aabbDistance(const Vec3 &p, const Vec3 &lo, const Vec3 &hi) {
        double s = 0.0;
        const double dx = std::max(lo.x - p.x, p.x - hi.x);
        const double dy = std::max(lo.y - p.y, p.y - hi.y);
        const double dz = std::max(lo.z - p.z, p.z - hi.z);
        if (dx > 0.0) s += dx * dx;
        if (dy > 0.0) s += dy * dy;
        if (dz > 0.0) s += dz * dz;
        return std::sqrt(s);
    }

    /** 折线的 AABB（剪枝用；含半径余量由调用方决定，这里只给折线本身） */
    void polylineAabb(const std::vector<Vec3> &poly, Vec3 &lo, Vec3 &hi) {
        lo = hi = poly.front();
        for (size_t i = 1; i < poly.size(); ++i) {
            lo.x = std::min(lo.x, poly[i].x);
            lo.y = std::min(lo.y, poly[i].y);
            lo.z = std::min(lo.z, poly[i].z);
            hi.x = std::max(hi.x, poly[i].x);
            hi.y = std::max(hi.y, poly[i].y);
            hi.z = std::max(hi.z, poly[i].z);
        }
    }

    /** 抽稀网格上某个体素的线性下标（越界返回 -1） */
    long long instAt(const AiResult &ai, int i, int j, int k) {
        return ai.grid.redIndex(i, j, k);
    }

    /** 世界坐标 -> 抽稀网格下标（向下取整；redToWorld 用的是球心 convention） */
    int worldToRed(const AiResult &ai, double world, int axis) {
        const double s = ai.grid.spacing[axis];
        if (s <= 0.0) return -1;
        const int idx = (int) std::floor(world / s);
        if (idx < 0 || idx >= ai.grid.redDim[axis]) return -1;
        return idx;
    }

    std::string fnum(double v, int digits) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%.*f", digits, v);
        return std::string(buf);
    }

}   // namespace

// ThirdMolarRisk 是全局作用域的类型，它的成员函数定义也必须在全局作用域；
// 放进 namespace NerveRisk 里会报 "use of undeclared identifier"（成员名不会
// 自动进入该命名空间），这是个很容易重犯的位置错误。
std::string ThirdMolarRisk::summaryText() const {
    if (!measured) {
        return std::string("未测量：") + (error.empty() ? "输入不可用" : error);
    }
    char buf[320];
    std::string tail;
    if (contact) {
        tail = "（神经折线进入牙根掩膜 = 接触）";
    } else if (withinCanalRadius) {
        tail = "（已进入下颌管半径 " + fnum(canalRadiusMm, 1) + "mm 之内）";
    }
    std::string cortex;
    if (!cortexMeasured) {
        cortex = "；管壁区间无可取样段";
    } else {
        cortex = "；管壁区间" + std::string(cortexPresent ? "采到骨" : "未采到骨") +
                 "，最长连续非骨段 " + fnum(cortexGapMm, 1) + "mm";
    }
    snprintf(buf, sizeof(buf), "%s ↔ %s：最近距离 %smm%s%s",
             toothName.c_str(), nerveName.c_str(), fnum(minDistMm, 1).c_str(),
             tail.c_str(), cortex.c_str());
    return std::string(buf) + (error.empty() ? "" : ("（" + error + "）"));
}

namespace NerveRisk {

    const char *gradingCaveat() {
        return "P-10 只给几何事实（距离/接触/管壁区间骨取样），判级阈值待 PRD 定值，"
               "不参与 S-02 安全色，也不改写 S-05。";
    }

    ThirdMolarRisk assess(const VolumeRef &vol, const AiResult &ai, int instanceId,
                          const NervePath &nerve, const std::string &nerveSource) {
        ThirdMolarRisk r;
        r.instanceId = instanceId;
        r.nervePathId = nerve.id;
        r.nerveName = nerve.name;
        r.nerveSource = nerveSource;
        r.canalRadiusMm = nerve.radiusMm;
        r.cortexBoneThresholdHu = kBonesHu;
        r.doseText = "曝光/剂量参数：本序列的 CbctVolume 未携带（DICOM 头未导出 KVP/mAs/"
                     "Dose*），无法据此评估剂量对可重复性的影响";

        // ---- 来源量：即使后面判定为"未测量"也要留下，报告才有可复核性 ----
        Vec3 bMin, bMax;
        vol.boundsMin(bMin);
        vol.boundsMax(bMax);
        r.spacingX = vol.spacingX();
        r.spacingY = vol.spacingY();
        r.spacingZ = vol.spacingZ();
        // FOV 用体数据的实际覆盖范围（含末体素的一个步长），这就是"这次看得多宽"
        r.fovXMm = bMax.x - bMin.x + r.spacingX;
        r.fovYMm = bMax.y - bMin.y + r.spacingY;
        r.fovZMm = bMax.z - bMin.z + r.spacingZ;
        r.seriesUid = vol.seriesInstanceUID();
        r.manufacturer = vol.manufacturer();

        if (!vol.valid()) {
            r.error = "体数据未绑定：无法取样与给出 FOV";
            LOGW("nerveRisk: no volume (instance=%d nerve=%d)", instanceId, nerve.id);
            return r;
        }
        if (!ai.hasMask()) {
            r.error = "本次会话没有 AI-01 牙根掩膜：请先跑牙齿分割（P-10 MVP 用掩膜当牙根）";
            return r;
        }
        if (nerve.points.size() < 2) {
            r.error = "神经管折线少于 2 点：请先描记或运行 AI-02 自动下颌管";
            return r;
        }
        const AiInstance *ins = nullptr;
        for (size_t i = 0; i < ai.instances.size(); ++i) {
            if (ai.instances[i].id == instanceId) { ins = &ai.instances[i]; break; }
        }
        if (ins == nullptr) {
            r.error = "实例 #" + std::to_string(instanceId) + " 不在当前推理结果里";
            return r;
        }
        r.toothName = "AI-01 实例 #" + std::to_string(instanceId);

        const std::vector<Vec3> &poly = nerve.points;
        Vec3 polyLo, polyHi;
        polylineAabb(poly, polyLo, polyHi);

        // ---- a) 接触判定：折线顶点直接落进掩膜体素 ----
        //     这一步必须做在距离循环**之前**：折线穿进掩膜时，"到掩膜表面的距离"
        //     仍然是正数，只有按体素包含关系才读得到真正的接触。
        Vec3 contactCanal, contactRoot;
        for (size_t i = 0; i < poly.size(); ++i) {
            const int gi = worldToRed(ai, poly[i].x, 0);
            const int gj = worldToRed(ai, poly[i].y, 1);
            const int gk = worldToRed(ai, poly[i].z, 2);
            if (gi < 0 || gj < 0 || gk < 0) continue;      // 折线在模型网格之外
            const long long idx = instAt(ai, gi, gj, gk);
            if (idx < 0 || (size_t) idx >= ai.inst.size()) continue;
            if ((int) ai.inst[(size_t) idx] != instanceId) continue;
            r.contact = true;
            contactCanal = poly[i];
            contactRoot = ai.grid.redToWorld(gi, gj, gk);
            break;
        }

        // ---- b) 掩膜 ↔ 折线的最小距离（只扫实例包围盒，折线 AABB 下界剪枝）----
        const AiGrid &g = ai.grid;
        const int i0 = std::max(0, (int) std::floor(ins->bboxMin.x / g.spacing[0]));
        const int j0 = std::max(0, (int) std::floor(ins->bboxMin.y / g.spacing[1]));
        const int k0 = std::max(0, (int) std::floor(ins->bboxMin.z / g.spacing[2]));
        const int i1 = std::min(g.redDim[0] - 1,
                                (int) std::floor(ins->bboxMax.x / g.spacing[0]));
        const int j1 = std::min(g.redDim[1] - 1,
                                (int) std::floor(ins->bboxMax.y / g.spacing[1]));
        const int k1 = std::min(g.redDim[2] - 1,
                                (int) std::floor(ins->bboxMax.z / g.spacing[2]));

        double best = 1e300;
        Vec3 bestRoot, bestCanal;
        long long scanned = 0;
        for (int i = i0; i <= i1; ++i) {
            for (int j = j0; j <= j1; ++j) {
                for (int k = k0; k <= k1; ++k) {
                    const long long idx = g.redIndex(i, j, k);
                    if (idx < 0 || (size_t) idx >= ai.inst.size()) continue;
                    if ((int) ai.inst[(size_t) idx] != instanceId) continue;
                    ++scanned;
                    // 接触时距离恒为 0，不再算折线距离，但**体素数照样统计**：
                    // rootVoxels 是"这一条几何事实基于多少体素"的取证字段，
                    // 若接触分支直接跳过扫描就会写 0，而 0 在别处意味着
                    // "实例不在掩膜里"（见头文件），会把最强的阳性结果读成空数据。
                    if (r.contact) continue;
                    const Vec3 p = g.redToWorld(i, j, k);
                    // 剪枝是严格的：AABB 距高是整个折线距高的下界，
                    // 因此"下界已经 >= 当前最好值"的体素不可能更近。
                    if (aabbDistance(p, polyLo, polyHi) >= best) continue;
                    Vec3 foot;
                    const double d = pointToPolyline(poly, p, &foot);
                    if (d < best) {
                        best = d;
                        bestRoot = p;
                        bestCanal = foot;
                    }
                }
            }
        }

        r.rootVoxels = scanned;
        if (r.contact) {
            r.minDistMm = 0.0;                 // 接触 = 距离 0，不是"很小"
            r.nearestCanal = contactCanal;
            r.nearestRoot = contactRoot;
        } else if (scanned > 0 && best < 1e299) {
            r.minDistMm = best;
            r.nearestCanal = bestCanal;
            r.nearestRoot = bestRoot;
        } else {
            r.error = "实例 #" + std::to_string(instanceId) +
                      " 的包围盒里没有掩膜体素（推理结果与实例表不一致？）";
            return r;
        }
        r.measured = true;
        r.withinCanalRadius = r.minDistMm <= nerve.radiusMm + 1e-12;

        // ---- c) 管壁区间里的皮质骨取样 ----
        //     沿"最近管点 -> 最近根面点"这条射线走，跳过管腔半径以内的部分
        //     （那里本来就该是管腔，不是骨），也不走进根面体素内部。
        //     剩下的区间如果一处骨都没采到，就写 cortexPresent=false ——
        //     这是"这条线上没采到骨"，不等于"皮质骨缺损"的临床结论。
        const double step = std::min(r.spacingX, std::min(r.spacingY, r.spacingZ)) * 0.5;
        if (step <= 0.0) {
            r.error += "；体素间距非法，皮质骨取样跳过";
            return r;
        }
        const double tStart = nerve.radiusMm;
        const double tEnd = r.minDistMm - step;      // 不进根面体素内部
        if (tEnd <= tStart) {
            // 根面已经在管腔半径之内（或区间不足一个步长）：没有"管壁"可采
            r.cortexMeasured = false;
            r.error = r.error.empty() ? "管壁区间不足一个取样步长：根面已进入管腔半径" : r.error;
        } else {
            const Vec3 dir = (r.nearestRoot - r.nearestCanal).normalized();
            int boneHits = 0, samples = 0;
            double run = 0.0, longestRun = 0.0;
            for (double t = tStart; t <= tEnd + 1e-9; t += step) {
                const float hu = vol.huTrilinear(r.nearestCanal + dir * t, -1024.0f);
                ++samples;
                if (hu >= kBonesHu) {
                    ++boneHits;
                    run = 0.0;
                } else {
                    run += step;
                    longestRun = std::max(longestRun, run);
                }
            }
            r.cortexMeasured = samples > 0;
            r.cortexPresent = boneHits > 0;
            r.cortexGapMm = longestRun;
            LOGD("nerveRisk #%d/%d: dist=%.2f contact=%d withinRadius=%d cortex=%d/%d gap=%.2f",
                 instanceId, nerve.id, r.minDistMm, (int) r.contact, (int) r.withinCanalRadius,
                 boneHits, samples, r.cortexGapMm);
        }
        return r;
    }

    Json toJson(const ThirdMolarRisk &r) {
        auto vecJson = [](const Vec3 &v) {
            Json a = Json::makeArray();
            a.push(Json::makeNumber(v.x));
            a.push(Json::makeNumber(v.y));
            a.push(Json::makeNumber(v.z));
            return a;
        };
        Json j = Json::makeObject();
        j.set("id", Json::makeNumber(r.id));
        j.set("instanceId", Json::makeNumber(r.instanceId));
        j.set("nervePathId", Json::makeNumber(r.nervePathId));
        j.set("toothName", Json::makeString(r.toothName));
        j.set("nerveName", Json::makeString(r.nerveName));
        j.set("nerveSource", Json::makeString(r.nerveSource));
        j.set("createdAt", Json::makeNumber((double) r.createdAt));
        j.set("measured", Json::makeBool(r.measured));
        j.set("minDistMm", Json::makeNumber(r.minDistMm));
        j.set("contact", Json::makeBool(r.contact));
        j.set("withinCanalRadius", Json::makeBool(r.withinCanalRadius));
        j.set("nearestRoot", vecJson(r.nearestRoot));
        j.set("nearestCanal", vecJson(r.nearestCanal));
        j.set("rootVoxels", Json::makeNumber((double) r.rootVoxels));
        j.set("canalRadiusMm", Json::makeNumber(r.canalRadiusMm));
        j.set("cortexMeasured", Json::makeBool(r.cortexMeasured));
        j.set("cortexPresent", Json::makeBool(r.cortexPresent));
        j.set("cortexGapMm", Json::makeNumber(r.cortexGapMm));
        j.set("cortexBoneThresholdHu", Json::makeNumber(r.cortexBoneThresholdHu));
        j.set("fovXMm", Json::makeNumber(r.fovXMm));
        j.set("fovYMm", Json::makeNumber(r.fovYMm));
        j.set("fovZMm", Json::makeNumber(r.fovZMm));
        j.set("spacingX", Json::makeNumber(r.spacingX));
        j.set("spacingY", Json::makeNumber(r.spacingY));
        j.set("spacingZ", Json::makeNumber(r.spacingZ));
        j.set("doseText", Json::makeString(r.doseText));
        j.set("seriesUid", Json::makeString(r.seriesUid));
        j.set("manufacturer", Json::makeString(r.manufacturer));
        j.set("error", Json::makeString(r.error));
        // 判级口径随行：归档与 SR 都必须自带这句，否则三年后没人知道它没有阈值
        j.set("gradingCaveat", Json::makeString(gradingCaveat()));
        return j;
    }

    void fromJson(const Json &j, ThirdMolarRisk &out) {
        auto vecFrom = [](const Json &a) {
            Vec3 v;
            if (a.isArray() && a.size() >= 3) {
                v.x = a[0].asNumber(0.0);
                v.y = a[1].asNumber(0.0);
                v.z = a[2].asNumber(0.0);
            }
            return v;
        };
        out.id = (int) j.at("id").asNumber(0);
        out.instanceId = (int) j.at("instanceId").asNumber(0);
        out.nervePathId = (int) j.at("nervePathId").asNumber(0);
        out.toothName = j.at("toothName").asString();
        out.nerveName = j.at("nerveName").asString();
        out.nerveSource = j.at("nerveSource").asString();
        out.createdAt = (long long) j.at("createdAt").asNumber(0);
        out.measured = j.at("measured").asBool(false);
        out.minDistMm = j.at("minDistMm").asNumber(-1.0);
        out.contact = j.at("contact").asBool(false);
        out.withinCanalRadius = j.at("withinCanalRadius").asBool(false);
        out.nearestRoot = vecFrom(j.at("nearestRoot"));
        out.nearestCanal = vecFrom(j.at("nearestCanal"));
        out.rootVoxels = (long long) j.at("rootVoxels").asNumber(0);
        out.canalRadiusMm = j.at("canalRadiusMm").asNumber(0.0);
        out.cortexMeasured = j.at("cortexMeasured").asBool(false);
        out.cortexPresent = j.at("cortexPresent").asBool(false);
        out.cortexGapMm = j.at("cortexGapMm").asNumber(0.0);
        out.cortexBoneThresholdHu = j.at("cortexBoneThresholdHu").asNumber(kBonesHu);
        out.fovXMm = j.at("fovXMm").asNumber(0.0);
        out.fovYMm = j.at("fovYMm").asNumber(0.0);
        out.fovZMm = j.at("fovZMm").asNumber(0.0);
        out.spacingX = j.at("spacingX").asNumber(0.0);
        out.spacingY = j.at("spacingY").asNumber(0.0);
        out.spacingZ = j.at("spacingZ").asNumber(0.0);
        out.doseText = j.at("doseText").asString();
        out.seriesUid = j.at("seriesUid").asString();
        out.manufacturer = j.at("manufacturer").asString();
        out.error = j.at("error").asString();
    }

    Json listJson(const std::vector<ThirdMolarRisk> &list) {
        Json arr = Json::makeArray();
        for (size_t i = 0; i < list.size(); ++i) arr.push(toJson(list[i]));
        return arr;
    }

}   // namespace NerveRisk
