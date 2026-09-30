// 测量会话管理器实现（PRD 5.1 / 5.2 / 5.3 / 7.3）。
//
// 数值精度链路（AC-01 ~ AC-04 的落点）：
//   触摸像素 --(displayToRay, vtkCoordinate 逆投影)--> 世界射线
//            --(MeasurePicker 射线步进, 半步长)--> 体素中心（毫米）
//            --(MeasureMath / RoiExtractor 解析计算)--> 结果
// 每一步的量化误差都被显式控制：拾取吸附体素中心（<= 体素对角线/2），
// 长度用解析欧氏距离（无累积误差），体积用分数覆盖权重（<= 1%）。

#include "include/MeasurementManager.h"

#include "include/AiCore.h"
#include "include/AnnotationStore.h"
#include "include/MeasureMath.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#define TAG "CbctMeasure"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace {

    long long epochMs() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    }

    Json vecJson(const Vec3 &v) {
        Json a = Json::makeArray();
        a.push(Json::makeNumber(v.x));
        a.push(Json::makeNumber(v.y));
        a.push(Json::makeNumber(v.z));
        return a;
    }

    Vec3 vecFrom(const Json &a) {
        if (a.size() < 3) return Vec3();
        return Vec3(a[0].asNumber(), a[1].asNumber(), a[2].asNumber());
    }

    Json pointsJson(const std::vector<Vec3> &pts) {
        Json arr = Json::makeArray();
        for (size_t i = 0; i < pts.size(); ++i) arr.push(vecJson(pts[i]));
        return arr;
    }

    std::vector<Vec3> pointsFrom(const Json &arr) {
        std::vector<Vec3> out;
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json &p = arr[i];
            if (p.size() >= 3) out.push_back(vecFrom(p));
        }
        return out;
    }

    /** 种植体安全等级 -> 颜色（S-02：红=不安全 / 黄=临界 / 绿=安全） */
    int safetyColor(int level) {
        if (level == DANGER_RED) return (int) 0xFFFF1744U;
        if (level == WARN_YELLOW) return (int) 0xFFFFC400U;
        return (int) 0xFF00E676U;
    }

    /** ROI 的空间线框（Box 8 顶点；Sphere 三段正交大圆；Plane 半空间不画框） */
    void roiOutline(const RoiDef &roi, std::vector<OverlayPrim> &out) {
        if (roi.type == ROI_BOX) {
            const Vec3 lo(std::min(roi.boxMin.x, roi.boxMax.x), std::min(roi.boxMin.y, roi.boxMax.y),
                          std::min(roi.boxMin.z, roi.boxMax.z));
            const Vec3 hi(std::max(roi.boxMin.x, roi.boxMax.x), std::max(roi.boxMin.y, roi.boxMax.y),
                          std::max(roi.boxMin.z, roi.boxMax.z));
            OverlayPrim box;
            box.kind = OK_BOX;
            box.color = roi.color;
            box.widthPx = 1.6;
            // 顶点顺序固定为 xyz 位掩码（0..7），与 Kotlin 侧的 12 条棱表一致
            for (int n = 0; n < 8; ++n) {
                box.world.push_back(Vec3((n & 1) ? hi.x : lo.x,
                                         (n & 2) ? hi.y : lo.y,
                                         (n & 4) ? hi.z : lo.z));
            }
            out.push_back(box);
        } else if (roi.type == ROI_SPHERE) {
            for (int plane = 0; plane < 3; ++plane) {
                std::vector<Vec3> ring;
                AnnotationStore::ringPolygon(roi.sphereCenter, roi.sphereRadius, plane, 48, ring);
                OverlayPrim c;
                c.kind = OK_POLYGON;
                c.color = roi.color;
                c.widthPx = 1.5;
                c.world = ring;
                out.push_back(c);
            }
        } else if (roi.type == ROI_PLANE) {
            // 半空间没有有限边界：用 origin 处的法线短线指示朝向，避免画一个假方框
            OverlayPrim n;
            n.kind = OK_LINE;
            n.color = roi.color;
            n.widthPx = 2.0;
            const Vec3 dir = roi.planeNormal.normalized();
            n.world.push_back(roi.planeOrigin);
            n.world.push_back(roi.planeOrigin + dir * 8.0);
            n.arrowHead = 1;
            out.push_back(n);
        }
    }

    /**
     * 给 [from, out.size()) 区间内的图元打上归属标记。
     * 每个源对象（ROI / 神经 / 测量 / 种植体）绘制完自己那批图元后立刻调用，
     * 这样 Kotlin 侧点列表某一项就能精确加粗对应图形（PRD 5.1.4）。
     */
    void tagOwner(std::vector<OverlayPrim> &out, size_t from, int kind, int id) {
        for (size_t k = from; k < out.size(); ++k) {
            out[k].ownerKind = kind;
            out[k].ownerId = id;
        }
    }

}   // namespace

MeasurementManager::MeasurementManager()
        : extractor_(vol_), planner_(vol_), picker_(vol_) {
    // R-06（AI 掩膜 ROI）的判定要问本类：只有持有 AiResult 的一方知道
    // "世界坐标 -> 抽稀网格体素 -> 实例号"的映射（网格换算见 AiTypes.h）。
    // 注入的是 this，而本类不会被拷贝/移动（JNI 会话表按指针持有），所以安全。
    extractor_.setAiMaskQuery(this);
}

void MeasurementManager::bindVolume(const CbctVolume *vol) {
    vol_.bind(vol);
    // extractor_ / planner_ / picker_ 持有的是 vol_ 的引用，绑定即生效，无需重建
    LOGD("bindVolume: %d x %d x %d, spacing %.2f/%.2f/%.2f",
         vol_.width(), vol_.height(), vol_.depth(),
         vol_.spacingX(), vol_.spacingY(), vol_.spacingZ());
}

std::string MeasurementManager::unitFor(int type) {
    switch (type) {
        case MT_DISTANCE:
        case MT_POINT_TO_LINE:
        case MT_ARC_LENGTH:
        case MT_ARCH_LENGTH:
        case MT_MIDLINE_OFFSET:
        case MT_OVERBITE: return "mm";
        case MT_ANGLE:
        case MT_TOOTH_ANGULATION: return "deg";
        case MT_ROI_VOLUME: return "cm3";
        case MT_ROI_AREA: return "cm2";
        case MT_HU_SAMPLE:
        case MT_BONE_DENSITY: return "HU";
    }
    return "";
}

/**
 * 按类型计算 value / unit / detailJson。
 * 约定：任何分支（含失败）都会在退出前把 detailJson 写好，
 * 失败原因放在 detailJson.error，UI 直接展示而不必解析 C++ 异常。
 */
bool MeasurementManager::compute(MeasureRecord &rec) const {
    rec.unit = unitFor(rec.type);
    Json detail = Json::makeObject();
    bool ok = false;

    switch (rec.type) {
        case MT_DISTANCE: {
            if (rec.points.size() < 2) {
                detail.set("error", Json::makeString("需要 2 个点"));
            } else {
                rec.value = MeasureMath::distance(rec.points[0], rec.points[1]);
                detail.set("p1", vecJson(rec.points[0]));
                detail.set("p2", vecJson(rec.points[1]));
                ok = true;
            }
            break;
        }
        case MT_ANGLE: {
            if (rec.points.size() < 3) {
                detail.set("error", Json::makeString("需要 3 个点（顶点为第 2 点）"));
            } else {
                rec.value = MeasureMath::angle(rec.points[0], rec.points[1], rec.points[2]);
                detail.set("ab", Json::makeNumber(MeasureMath::distance(rec.points[0], rec.points[1])));
                detail.set("bc", Json::makeNumber(MeasureMath::distance(rec.points[1], rec.points[2])));
                detail.set("ac", Json::makeNumber(MeasureMath::distance(rec.points[0], rec.points[2])));
                ok = true;
            }
            break;
        }
        case MT_POINT_TO_LINE: {
            if (rec.points.size() < 3) {
                detail.set("error", Json::makeString("需要 P/A/B 三个点"));
            } else {
                Vec3 foot;
                rec.value = MeasureMath::pointToSegment(rec.points[0], rec.points[1],
                                                        rec.points[2], &foot);
                detail.set("foot", vecJson(foot));
                // 垂足是否落在线段内决定了这个读数是"垂直距离"还是"到端点距离"
                const Vec3 ab = rec.points[2] - rec.points[1];
                const double len2 = ab.dot(ab);
                const double t = len2 > 1e-12 ? (foot - rec.points[1]).dot(ab) / len2 : 0.0;
                detail.set("footInside", Json::makeBool(t > 0.0 && t < 1.0));
                ok = true;
            }
            break;
        }
        case MT_ARC_LENGTH: {
            if (rec.points.size() < 2) {
                detail.set("error", Json::makeString("至少 2 个采样点"));
            } else {
                rec.value = MeasureMath::arcLength(rec.points, false);
                detail.set("samples", Json::makeNumber((double) rec.points.size()));
                ok = true;
            }
            break;
        }
        case MT_HU_SAMPLE: {
            if (!vol_.valid()) {
                detail.set("error", Json::makeString("未绑定体数据"));
            } else if (rec.points.empty()) {
                detail.set("error", Json::makeString("需要 1 个点"));
            } else {
                // M-07 要求"精确值"：取体素中心的原始 HU，不做插值平滑
                rec.value = (double) vol_.huNearest(rec.points[0], -1000.0f);
                detail.set("tissue", Json::makeString(VolumeRef::tissueName((float) rec.value)));
                int i, j, k;
                vol_.worldToIndex(rec.points[0], i, j, k);
                Json idx = Json::makeArray();
                idx.push(Json::makeNumber(i));
                idx.push(Json::makeNumber(j));
                idx.push(Json::makeNumber(k));
                detail.set("voxel", idx);
                ok = true;
            }
            break;
        }
        case MT_ROI_VOLUME:
        case MT_BONE_DENSITY: {
            const RoiDef *roi = rec.roiId != 0 ? findRoi(rec.roiId) : nullptr;
            if (!roi) {
                detail.set("error", Json::makeString(rec.roiId == 0 ? "未选择 ROI" : "ROI 不存在"));
            } else {
                const RoiStats s = extractor_.analyze(*roi, &rois_);
                if (!s.ok) {
                    detail.set("error", Json::makeString(s.error));
                } else {
                    rec.value = rec.type == MT_ROI_VOLUME ? s.volumeCm3 : s.meanHu;
                    detail.set("roiId", Json::makeNumber(roi->id));
                    detail.set("roiName", Json::makeString(roi->name));
                    detail.set("roiType", Json::makeNumber(roi->type));
                    detail.set("coverageVoxels", Json::makeNumber(s.coverageVoxels));
                    detail.set("fullVoxels", Json::makeNumber((double) s.fullVoxels));
                    detail.set("scannedVoxels", Json::makeNumber((double) s.scannedVoxels));
                    detail.set("voxelMm3", Json::makeNumber(s.voxelMm3));
                    detail.set("volumeMm3", Json::makeNumber(s.volumeMm3));
                    detail.set("volumeCm3", Json::makeNumber(s.volumeCm3));
                    detail.set("meanHu", Json::makeNumber(s.meanHu));
                    detail.set("sdHu", Json::makeNumber(s.sdHu));
                    detail.set("minHu", Json::makeNumber(s.minHu));
                    detail.set("maxHu", Json::makeNumber(s.maxHu));
                    detail.set("elapsedMs", Json::makeNumber((double) s.elapsedMs));
                    if (!s.error.empty()) detail.set("note", Json::makeString(s.error));
                    ok = true;
                }
            }
            break;
        }
        case MT_ROI_AREA: {
            // 两条路径：绑定了带截面的 ROI，或直接把测量点当封闭多边形
            const RoiDef *roi = rec.roiId != 0 ? findRoi(rec.roiId) : nullptr;
            const std::vector<Vec3> *poly = nullptr;
            if (roi && roi->polygon.size() >= 3) poly = &roi->polygon;
            else if (rec.points.size() >= 3) poly = &rec.points;
            if (!poly) {
                detail.set("error", Json::makeString("需要 >= 3 点的封闭路径"));
            } else {
                const double mm2 = MeasureMath::polygonArea(*poly);
                rec.value = mm2 / 100.0;             // mm² -> cm²
                detail.set("areaMm2", Json::makeNumber(mm2));
                detail.set("samples", Json::makeNumber((double) poly->size()));
                detail.set("centroid", vecJson(MeasureMath::polygonCentroid(*poly)));
                detail.set("plane", Json::makeNumber(roi ? roi->plane : MP_AXIAL));
                ok = true;
            }
            break;
        }
        // ---- PRD 5.3.3 正畸评估：S-07 ~ S-10 ----
        // 四类量的输入都是"点选/拖动得到的世界毫米点"，所以复用同一条
        // requiredPoints -> compute 通道，不新增 ToolState（AC-10 的 9 态不变）。
        case MT_ARCH_LENGTH: {
            if (rec.points.size() < 2) {
                detail.set("error", Json::makeString("牙弓弧线至少需要 2 个描记点"));
            } else {
                rec.value = MeasureMath::arcLength(rec.points, false);
                const Vec3 &a = rec.points.front();
                const Vec3 &b = rec.points.back();
                const double chord = MeasureMath::distance(a, b);
                // 弓深：各中间点到首末连线的最大垂直距离（近似牙弓前后深度）
                double deepest = 0.0;
                for (size_t i = 1; i + 1 < rec.points.size(); ++i) {
                    const double dev = MeasureMath::pointToLine(rec.points[i], a, b);
                    if (dev > deepest) deepest = dev;
                }
                detail.set("samples", Json::makeNumber((double) rec.points.size()));
                detail.set("chordMm", Json::makeNumber(chord));
                detail.set("archDepthMm", Json::makeNumber(deepest));
                // 闭合周长 = 弧线 + 首末连线，正畸用它做弓长/弓周长对比
                detail.set("perimeterMm", Json::makeNumber(rec.value + chord));
                ok = true;
            }
            break;
        }
        case MT_TOOTH_ANGULATION: {
            if (rec.points.size() < 4) {
                detail.set("error", Json::makeString("排列角度需要两颗牙的长轴各 2 点（共 4 点）"));
            } else {
                rec.value = MeasureMath::lineDeviationDeg(rec.points[0], rec.points[1],
                                                          rec.points[2], rec.points[3]);
                detail.set("axisA", pointsJson({rec.points[0], rec.points[1]}));
                detail.set("axisB", pointsJson({rec.points[2], rec.points[3]}));
                detail.set("axisALenMm", Json::makeNumber(
                        MeasureMath::distance(rec.points[0], rec.points[1])));
                detail.set("axisBLenMm", Json::makeNumber(
                        MeasureMath::distance(rec.points[2], rec.points[3])));
                // lineDeviationDeg 把无向直线夹角折到 [0,90]，读数永远是"偏差"而非补角
                detail.set("foldedTo90", Json::makeBool(true));
                ok = true;
            }
            break;
        }
        case MT_MIDLINE_OFFSET: {
            if (rec.points.size() < 4) {
                detail.set("error", Json::makeString("中线偏移需要上下中线各 2 点（共 4 点）"));
            } else {
                // 前一半点 = 上颌中线，后一半 = 下颌中线；midlineOffset 取各自弧长 50% 处
                const size_t mid = rec.points.size() / 2;
                const std::vector<Vec3> upper(rec.points.begin(), rec.points.begin() + (long) mid);
                const std::vector<Vec3> lower(rec.points.begin() + (long) mid, rec.points.end());
                rec.value = MeasureMath::midlineOffset(upper, lower);
                const Vec3 mu = MeasureMath::pointAtArcFraction(upper, 0.5);
                const Vec3 ml = MeasureMath::pointAtArcFraction(lower, 0.5);
                detail.set("upperArc", pointsJson(upper));
                detail.set("lowerArc", pointsJson(lower));
                detail.set("upperMid", vecJson(mu));
                detail.set("lowerMid", vecJson(ml));
                // 垂直分量属于覆合，这里单列出来说明"偏移只算水平面内距离"
                detail.set("verticalMm", Json::makeNumber(std::fabs(mu.z - ml.z)));
                ok = true;
            }
            break;
        }
        case MT_OVERBITE: {
            if (rec.points.size() < 2) {
                detail.set("error", Json::makeString("覆合/覆盖需要上、下切牙切缘各 1 点"));
            } else {
                const Vec3 &u = rec.points[0];
                const Vec3 &l = rec.points[1];
                const Vec3 dd = u - l;
                // 体数据已按 Z 轴升序重排（世界 Z = 头脚向），所以覆合固定取 Z；
                // 前后向落在 X 还是 Y 取决于摆位，取水平分量较大的一轴并在明细里说明。
                const int apAxis = std::fabs(dd.x) >= std::fabs(dd.y) ? 0 : 1;
                double overjet = 0.0, overbite = 0.0;
                MeasureMath::overbiteOverjet(u, l, apAxis, overjet, overbite);
                rec.value = std::fabs(overbite);
                detail.set("apAxis", Json::makeNumber(apAxis));
                detail.set("overjetMm", Json::makeNumber(overjet));
                detail.set("overbiteMm", Json::makeNumber(overbite));
                detail.set("apDeltaXMm", Json::makeNumber(dd.x));
                detail.set("apDeltaYMm", Json::makeNumber(dd.y));
                ok = true;
            }
            break;
        }
        default:
            detail.set("error", Json::makeString("未知测量类型"));
            break;
    }

    rec.detailJson = detail.dump(false);
    if (!ok) rec.value = 0.0;
    return ok;
}

MeasureRecord MeasurementManager::addMeasure(int type, const std::vector<Vec3> &points, int roiId,
                                             const std::string &name, unsigned int color,
                                             const std::string &note) {
    MeasureRecord rec;
    rec.id = ++nextId_;
    rec.type = type;
    rec.points = points;
    rec.roiId = roiId;
    rec.name = name.empty() ? ("测量 " + std::to_string(rec.id)) : name;
    rec.color = (int) color;
    rec.note = note;
    rec.visible = true;
    rec.createdAt = epochMs();
    const bool ok = compute(rec);
    records_.push_back(rec);
    LOGD("addMeasure #%d type=%d value=%.4f %s ok=%d", rec.id, type, rec.value,
         rec.unit.c_str(), (int) ok);
    return rec;
}

bool MeasurementManager::recalcMeasure(int id) {
    for (size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].id != id) continue;
        return compute(records_[i]);
    }
    return false;
}

bool MeasurementManager::removeMeasure(int id) {
    for (size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].id == id) {
            records_.erase(records_.begin() + (long) i);
            return true;
        }
    }
    return false;
}

bool MeasurementManager::renameMeasure(int id, const std::string &name) {
    for (size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].id == id) { records_[i].name = name; return true; }
    }
    return false;
}

bool MeasurementManager::setMeasureVisible(int id, bool visible) {
    for (size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].id == id) { records_[i].visible = visible; return true; }
    }
    return false;
}

bool MeasurementManager::setMeasureNote(int id, const std::string &note) {
    for (size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].id == id) { records_[i].note = note; return true; }
    }
    return false;
}

const MeasureRecord *MeasurementManager::findMeasure(int id) const {
    for (size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].id == id) return &records_[i];
    }
    return nullptr;
}

void MeasurementManager::clearMeasures() { records_.clear(); }

PickResult MeasurementManager::pickByRay(const Vec3 &origin, const Vec3 &dir,
                                        const PickRequest &req) const {
    switch (req.mode) {
        case 1:
            return picker_.pickThroughBone(origin, dir, req.skip, req.huThreshold);
        case 2: {
            double lo = req.huLo, hi = req.huHi;
            // UI 没给区间时退化为"当前主阈值 ROI"，再退化为骨窗（200~3000HU）
            if (hi <= lo && !dominantHuRange(lo, hi)) {
                lo = 200.0;
                hi = 3000.0;
            }
            return picker_.pickHuRange(origin, dir, lo, hi);
        }
        case 3:
            return picker_.pickPlane(origin, dir, req.plane, req.planePosition);
        case 4: {
            const RoiDef *roi = findRoi(req.roiId);
            if (!roi) {
                PickResult miss;
                miss.describe = "ROI 不存在：" + std::to_string(req.roiId);
                return miss;
            }
            return picker_.pickRoiSurface(*roi, origin, dir);
        }
        case 5: {
            PickResult r;
            for (size_t i = 0; i < nerves_.size(); ++i) {
                if (nerves_[i].id != req.nerveId) continue;
                int pointIndex = -1;
                Vec3 closest;
                if (!picker_.pickNervePoint(nerves_[i], origin, dir, req.toleranceMm,
                                            pointIndex, closest)) {
                    r.describe = "射线未命中神经管折线（容差 " +
                                 std::to_string((int) req.toleranceMm) + "mm）";
                    return r;
                }
                // 命中点取折线上的锚点本身：描记阶段的语义是"选中最靠近的那个已有点"，
                // 而不是射线上的任意投影，否则连续两次点同一位置会飘出不同坐标。
                r.hit = true;
                r.point = nerves_[i].points[pointIndex];
                r.voxelCenter = picker_.snapToVoxelCenter(r.point);
                vol_.worldToIndex(r.point, r.index[0], r.index[1], r.index[2]);
                r.hu = vol_.huNearest(r.point, 0.0f);
                r.distanceMm = (r.point - origin).length();
                char buf[160];
                snprintf(buf, sizeof(buf), "%s 命中点 #%d (%.1f, %.1f, %.1f) HU=%.0f",
                         nerves_[i].name.c_str(), pointIndex, r.point.x, r.point.y, r.point.z,
                         r.hu);
                r.describe = buf;
                return r;
            }
            r.describe = "神经管路径不存在：" + std::to_string(req.nerveId);
            return r;
        }
        default:
            return picker_.pickSurface(origin, dir, req.huThreshold, req.tMaxMm, req.tMinMm);
    }
}

Vec3 MeasurementManager::snapToVoxelCenter(const Vec3 &p) const {
    if (!vol_.valid()) return p;
    return picker_.snapToVoxelCenter(p);
}

// =============================================================================
// ROI（R-01 ~ R-05）
// =============================================================================
int MeasurementManager::addRoi(const RoiDef &roi) {
    RoiDef r = roi;
    r.id = ++nextId_;
    if (r.name.empty()) r.name = "ROI " + std::to_string(r.id);
    rois_.push_back(r);
    LOGD("addRoi #%d type=%d name=%s", r.id, r.type, r.name.c_str());
    return r.id;
}

bool MeasurementManager::updateRoi(const RoiDef &roi) {
    for (size_t i = 0; i < rois_.size(); ++i) {
        if (rois_[i].id == roi.id) {
            rois_[i] = roi;
            rois_[i].id = roi.id;            // 不允许通过更新改 id
            return true;
        }
    }
    return false;
}

bool MeasurementManager::removeRoi(int id) {
    for (size_t i = 0; i < rois_.size(); ++i) {
        if (rois_[i].id != id) continue;
        rois_.erase(rois_.begin() + (long) i);
        // 关联测量项与组合 ROI 的子引用都要清理，否则重算会静默拿到"ROI 不存在"
        for (size_t n = 0; n < records_.size(); ++n) {
            if (records_[n].roiId == id) {
                records_[n].roiId = 0;
                compute(records_[n]);
            }
        }
        for (size_t m = 0; m < rois_.size(); ++m) {
            RoiDef &r = rois_[m];
            if (r.childA == id) { r.childA = 0; r.opAB = OP_NONE; }
            if (r.childB == id) { r.childB = 0; r.opAB = OP_NONE; }
            if (r.childC == id) { r.childC = 0; r.opAC = OP_NONE; }
        }
        return true;
    }
    return false;
}

bool MeasurementManager::setRoiVisible(int id, bool visible) {
    for (size_t i = 0; i < rois_.size(); ++i) {
        if (rois_[i].id == id) { rois_[i].visible = visible; return true; }
    }
    return false;
}

const RoiDef *MeasurementManager::findRoi(int id) const {
    for (size_t i = 0; i < rois_.size(); ++i) {
        if (rois_[i].id == id) return &rois_[i];
    }
    return nullptr;
}

RoiStats MeasurementManager::statRoi(int id) const {
    const RoiDef *roi = findRoi(id);
    if (!roi) {
        RoiStats s;
        s.error = "ROI 不存在";
        return s;
    }
    return extractor_.analyze(*roi, &rois_);
}

bool MeasurementManager::dominantHuRange(double &lo, double &hi) const {
    // 取第一个阈值型 ROI 作为 VR 分割区间；没有则 false（沿用骨窗不透明度曲线）
    for (size_t i = 0; i < rois_.size(); ++i) {
        if (rois_[i].type == ROI_HU_THRESHOLD && rois_[i].visible &&
            rois_[i].huMax > rois_[i].huMin) {
            lo = rois_[i].huMin;
            hi = rois_[i].huMax;
            return true;
        }
    }
    return false;
}

// =============================================================================
// AI-01 / AI-03（PRD 5.6）
// =============================================================================

void MeasurementManager::adoptAiResult(const AiResult &src) {
    ai_ = src;
    if (!ai_.ok) ai_.instances.clear();
    LOGD("adoptAiResult: ok=%d inst=%zu toothVox=%lld total=%.0fms thr=%.4f",
         ai_.ok ? 1 : 0, ai_.instances.size(), ai_.toothVoxels, ai_.totalMs, ai_.thresholdUsed);
}

void MeasurementManager::clearAi() {
    ai_.clear();
    LOGD("clearAi: 推理结果已丢弃（R-06 ROI 保留，统计将为空并给出原因）");
}

void MeasurementManager::setAiOverlayVisible(bool visible) {
    if (ai_.overlayVisible == visible) return;
    ai_.overlayVisible = visible;
    LOGD("setAiOverlayVisible: %d", visible ? 1 : 0);
}

void MeasurementManager::setAiOverlaySlice(int plane, int position) {
    // 只在变化时打日志：MPR 拖动时每帧都会调，刷屏会淹没别的取证行
    if (aiOverlayPlane_ == plane && aiOverlayPosition_ == position) return;
    aiOverlayPlane_ = plane;
    aiOverlayPosition_ = position;
}

bool MeasurementManager::containsMm(int label, const Vec3 &world) const {
    if (!ai_.hasMask() || label <= 0) return false;
    const AiGrid &g = ai_.grid;
    // 世界 mm -> 抽稀网格索引（floor：与 redToWorld 的"+0.5 球心"互补，
    // 即某个体素中心落在哪个掩膜单元里，就认为它属于那个单元）
    int i = (int) std::floor(world.x / (g.spacing[0] > 1e-9 ? g.spacing[0] : 1.0));
    int j = (int) std::floor(world.y / (g.spacing[1] > 1e-9 ? g.spacing[1] : 1.0));
    int k = (int) std::floor(world.z / (g.spacing[2] > 1e-9 ? g.spacing[2] : 1.0));
    if (i < 0 || j < 0 || k < 0 || i >= g.redDim[0] || j >= g.redDim[1] || k >= g.redDim[2]) {
        return false;           // 补 0 区 / 体积外：不属于任何实例
    }
    // 判据用 inst 而不是 label：R-06 的体积要与 AiInstance.volumeMm3 逐体素相等，
    // 用 label 会把散点（已剔除、无实例号）算进 ROI，两处数字就对不上了。
    return ai_.inst[(size_t) g.redIndex(i, j, k)] == (short) label;
}

bool MeasurementManager::boundsMm(int label, Vec3 &lo, Vec3 &hi) const {
    for (size_t t = 0; t < ai_.instances.size(); ++t) {
        if (ai_.instances[t].id != label) continue;
        lo = ai_.instances[t].bboxMin;
        hi = ai_.instances[t].bboxMax;
        return true;
    }
    return false;
}

int MeasurementManager::aiAutoMeasure(bool withBoneDensity) {
    if (!ai_.hasMask() || ai_.instances.empty()) {
        LOGW("aiAutoMeasure: 没有可用的分割实例");
        return 0;
    }
    int added = 0;
    for (size_t t = 0; t < ai_.instances.size(); ++t) {
        AiInstance &ins = ai_.instances[t];       // 非 const：要把落库 id 回填写回去
        if (ins.roiId != 0) continue;             // 已经落过库，别重复建
        const int archTag = ins.arch == 0 ? 'U' : (ins.arch == 1 ? 'L' : '?');
        char nm[64];
        snprintf(nm, sizeof(nm), "AI 牙 %c%d(#%d)", archTag, ins.toothCountHint, ins.id);

        // 按 aiLabel 认领列表里已有的同一颗牙的掩膜 ROI：恢复归档 + 重跑推理时，旧 ROI
        // 连同它的数值行都在（那些行因掩膜网格不入档而显示"待重算"）。不认领的话每跑一
        // 次就多一套 26 行，报告里双份条目且一半是空的。
        int reuseId = 0;
        for (size_t q = 0; q < rois_.size(); ++q) {
            if (rois_[q].type == ROI_AI_MASK && rois_[q].aiLabel == ins.id) {
                reuseId = rois_[q].id;
                break;
            }
        }

        RoiDef roi;
        roi.type = ROI_AI_MASK;
        roi.aiLabel = ins.id;
        roi.name = std::string(nm) + " 掩膜";
        roi.color = (int) AiConst::PALETTE[(size_t) ((ins.id - 1) % AiConst::PALETTE_COUNT)];
        roi.visible = true;
        int roiId = 0;
        if (reuseId != 0) {
            roi.id = reuseId;
            updateRoi(roi);       // id 不变，名称/配色按本次推理结果刷新
            roiId = reuseId;
        } else {
            roiId = addRoi(roi);
        }
        ins.roiId = roiId;

        if (reuseId != 0) {
            // 只清本函数自己写过的行；先收集 id 再删，避免 erase 时迭代器失效
            std::vector<int> stale;
            for (size_t q = 0; q < records_.size(); ++q) {
                if (records_[q].roiId == reuseId && records_[q].note == AiConst::AUTO_NOTE) {
                    stale.push_back(records_[q].id);
                }
            }
            for (size_t q = 0; q < stale.size(); ++q) removeMeasure(stale[q]);
            if (!stale.empty()) {
                LOGD("aiAutoMeasure: 复用 ROI #%d（aiLabel=%d），清理旧自动测量 %zu 条",
                     reuseId, ins.id, stale.size());
            }
        }

        const MeasureRecord rec = addMeasure(MT_ROI_VOLUME, std::vector<Vec3>(), roiId,
                                             std::string(nm) + " 体积", roi.color,
                                             AiConst::AUTO_NOTE);
        ins.measureId = rec.id;
        ++added;

        if (withBoneDensity) {
            addMeasure(MT_BONE_DENSITY, std::vector<Vec3>(), roiId,
                       std::string(nm) + " 骨密度", roi.color, AiConst::AUTO_NOTE);
            ++added;
        }
    }
    LOGD("aiAutoMeasure: %zu 实例 -> %d 条测量（R-06 掩膜 ROI 复用 M-04/M-08）",
         ai_.instances.size(), added);
    return added;
}

std::vector<AiCandidate> MeasurementManager::aiRecommend(double minGapMm, int maxOut) const {
    std::vector<AiCandidate> out;
    AiPlanner::recommend(ai_, vol_, nerves_, implants_, minGapMm, maxOut, out);
    return out;
}

// =============================================================================
// 种植体方案（S-01 ~ S-06）
// =============================================================================
int MeasurementManager::addImplant(const Implant &im) {
    Implant a = im;
    a.id = ++nextId_;
    if (a.name.empty()) a.name = "种植体 " + std::to_string(a.id);
    ImplantPlanner::refreshGeometry(a);
    implants_.push_back(a);
    recomputePlan();                        // 新桩体会改变既有桩体的间距，整体重算
    LOGD("addImplant #%d entry=(%.1f,%.1f,%.1f) pitch=%.1f yaw=%.1f depth=%.1f dia=%.1f",
         a.id, a.entry.x, a.entry.y, a.entry.z, a.pitchDeg, a.yawDeg, a.depthMm, a.diaMm);
    return a.id;
}

bool MeasurementManager::updateImplant(const Implant &im) {
    for (size_t i = 0; i < implants_.size(); ++i) {
        if (implants_[i].id != im.id) continue;
        Implant a = im;
        a.id = im.id;
        ImplantPlanner::refreshGeometry(a);
        implants_[i] = a;
        recomputePlan();
        return true;
    }
    return false;
}

bool MeasurementManager::removeImplant(int id) {
    for (size_t i = 0; i < implants_.size(); ++i) {
        if (implants_[i].id != id) continue;
        implants_.erase(implants_.begin() + (long) i);
        recomputePlan();
        return true;
    }
    return false;
}

const Implant *MeasurementManager::findImplant(int id) const {
    for (size_t i = 0; i < implants_.size(); ++i) {
        if (implants_[i].id == id) return &implants_[i];
    }
    return nullptr;
}

void MeasurementManager::recomputePlan() {
    if (!vol_.valid()) {
        // 无体数据时仍要刷新几何（tip/axis），保证叠加层可绘制
        for (size_t i = 0; i < implants_.size(); ++i) ImplantPlanner::refreshGeometry(implants_[i]);
        return;
    }
    for (size_t i = 0; i < implants_.size(); ++i) {
        planner_.evaluate(implants_[i], nerves_, implants_);
    }
}

// =============================================================================
// 神经管路径
//
// 路径几何一变就必须重算方案：S-05/S-06 的神经管距离取自 nerves_，
// 描记前它是空的（评估结果为"未勾画"），描记后若不调 recomputePlan，
// 已有的种植体行会一直停留在 nerve=-1，安全等级也不会因新勾画的管道降级。
// 四个入口（新建 / 覆盖 / 删除 / 追加点）各自只多一次 evaluate：
// 无体数据或无种植体时 recomputePlan 直接返回，代价可忽略。
// =============================================================================
int MeasurementManager::addNervePath(const NervePath &path) {
    NervePath p = path;
    p.id = ++nextId_;
    if (p.name.empty()) p.name = "神经管 " + std::to_string(p.id);
    nerves_.push_back(p);
    recomputePlan();
    return p.id;
}

bool MeasurementManager::updateNervePath(const NervePath &path) {
    for (size_t i = 0; i < nerves_.size(); ++i) {
        if (nerves_[i].id != path.id) continue;
        nerves_[i] = path;
        recomputePlan();
        return true;
    }
    return false;
}

bool MeasurementManager::removeNervePath(int id) {
    for (size_t i = 0; i < nerves_.size(); ++i) {
        if (nerves_[i].id != id) continue;
        nerves_.erase(nerves_.begin() + (long) i);
        recomputePlan();
        return true;
    }
    return false;
}

bool MeasurementManager::appendNervePoint(int id, const Vec3 &p) {
    for (size_t i = 0; i < nerves_.size(); ++i) {
        if (nerves_[i].id != id) continue;
        nerves_[i].points.push_back(p);
        recomputePlan();
        return true;
    }
    return false;
}

// =============================================================================
// 叠加图元
// =============================================================================
void MeasurementManager::buildOverlay(std::vector<OverlayPrim> &out) const {
    const size_t aiBegin = out.size();
    if (ai_.hasMask() && ai_.overlayVisible) {
        AiCore::buildOverlay(ai_, aiOverlayPlane_, aiOverlayPosition_, out);
    }
    const size_t aiEnd = out.size();
    // 把 AI 轮廓的归属改写成对应的 R-06 ROI：AiCore 只认识"实例号"，而 UI 列表里
    // 选中的是 ROI —— 不改写的话"选中列表项 -> 掩膜加粗"就断了。没落库的实例
    // 保持 (OW_AI, 实例号)，Kotlin 仍可按 AI 组高亮。
    for (size_t k = aiBegin; k < aiEnd; ++k) {
        OverlayPrim &p = out[k];
        if (p.ownerKind != OW_AI) continue;
        for (size_t t = 0; t < rois_.size(); ++t) {
            if (rois_[t].type == ROI_AI_MASK && rois_[t].aiLabel == p.ownerId) {
                p.ownerKind = OW_ROI;
                p.ownerId = rois_[t].id;
                break;
            }
        }
    }
    for (size_t i = 0; i < rois_.size(); ++i) {
        if (!rois_[i].visible) continue;
        const size_t roiStart = out.size();
        roiOutline(rois_[i], out);
        if (rois_[i].polygon.size() >= 3) {
            OverlayPrim poly;
            poly.kind = OK_POLYGON;
            poly.color = rois_[i].color;
            poly.widthPx = 2.0;
            poly.fill = true;
            poly.world = rois_[i].polygon;
            poly.text = rois_[i].name;
            poly.labelAnchored = 1;
            out.push_back(poly);
        }
        tagOwner(out, roiStart, OW_ROI, rois_[i].id);
    }
    for (size_t i = 0; i < nerves_.size(); ++i) {
        const NervePath &np = nerves_[i];
        if (!np.visible || np.points.size() < 2) continue;
        const size_t nerveStart = out.size();
        OverlayPrim tube;
        tube.kind = OK_TUBE;
        tube.color = np.color;
        tube.widthPx = std::max(2.0, np.radiusMm * 2.0);
        tube.world = np.points;
        tube.text = np.name;
        tube.labelAnchored = 1;
        out.push_back(tube);
        tagOwner(out, nerveStart, OW_NERVE, np.id);
    }
    for (size_t i = 0; i < records_.size(); ++i) {
        const MeasureRecord &r = records_[i];
        if (!r.visible) continue;
        const size_t recStart = out.size();
        if (r.points.size() >= 2) {
            if ((r.type == MT_TOOTH_ANGULATION || r.type == MT_MIDLINE_OFFSET) &&
                r.points.size() >= 4) {
                // S-08 / S-09 的点是"两条无向直线"（两颗牙长轴、上下中线各两点）。
                // 若按通用分支连成 0-1-2-3 折线，看起来像在量一条路径，
                // 与"两条线夹角/两中点偏移"的语义相反，所以这里拆成两条独立线段。
                for (int seg = 0; seg + 1 < 4; seg += 2) {
                    OverlayPrim pair;
                    pair.kind = OK_LINE;
                    pair.color = r.color;
                    pair.widthPx = 2.0;
                    pair.world.push_back(r.points[seg]);
                    pair.world.push_back(r.points[seg + 1]);
                    out.push_back(pair);
                }
            } else {
                OverlayPrim line;
                line.color = r.color;
                line.widthPx = 2.0;
                if (r.type == MT_ROI_AREA && r.points.size() >= 3) {
                    line.kind = OK_POLYGON;
                    line.world = r.points;
                } else if (r.type == MT_ARC_LENGTH || r.type == MT_ANGLE ||
                           r.type == MT_ARCH_LENGTH) {
                    line.kind = OK_POLYLINE;
                    line.world = r.points;
                } else {
                    line.kind = OK_LINE;
                    line.world = r.points;
                }
                out.push_back(line);
            }
        }
        for (size_t n = 0; n < r.points.size(); ++n) {
            OverlayPrim dot;
            dot.kind = OK_POINT;
            dot.color = r.color;
            dot.world.push_back(r.points[n]);
            out.push_back(dot);
        }
        if (!r.points.empty()) {
            OverlayPrim t;
            t.kind = OK_TEXT;
            t.color = r.color;
            // 标签锚在末点：距离类读数习惯贴在第二条线端点旁（PRD 5.1.1 的显示约定）
            t.world.push_back(r.points.back());
            char buf[96];
            snprintf(buf, sizeof(buf), "%s: %s %s", r.name.c_str(),
                     MeasureMath::format(r.value, r.unit == "HU" ? 0 : 2).c_str(),
                     r.unit.c_str());
            t.text = buf;
            if (!r.note.empty()) t.text += "  " + r.note;
            t.labelAnchored = 1;
            out.push_back(t);
        }
        tagOwner(out, recStart, OW_MEASURE, r.id);
    }
}

void MeasurementManager::buildPlanOverlay(std::vector<OverlayPrim> &out) const {
    for (size_t i = 0; i < implants_.size(); ++i) {
        const Implant &im = implants_[i];
        if (!im.visible) continue;
        const size_t implantStart = out.size();
        const int col = safetyColor(im.level);
        std::vector<Vec3> capsule;
        ImplantPlanner::capsuleOutline(im, 24, capsule);
        OverlayPrim body;
        body.kind = OK_CAPSULE;
        body.color = col;
        body.widthPx = 2.2;
        body.world = capsule;
        out.push_back(body);

        std::vector<Vec3> thread;
        ImplantPlanner::threadOutline(im, std::max(4, (int) (im.lengthMm / 1.2)), 12, thread);
        OverlayPrim th;
        th.kind = OK_POLYLINE;
        th.color = col;
        th.widthPx = 1.2;
        th.world = thread;
        out.push_back(th);

        // 轴线（入口 -> 尖端）：临床核对植入方向用
        OverlayPrim axis;
        axis.kind = OK_LINE;
        axis.color = col;
        axis.widthPx = 1.0;
        axis.world.push_back(im.entry);
        axis.world.push_back(im.tip);
        out.push_back(axis);

        OverlayPrim dot;
        dot.kind = OK_POINT;
        dot.color = col;
        dot.world.push_back(im.entry);
        out.push_back(dot);

        OverlayPrim t;
        t.kind = OK_TEXT;
        t.color = col;
        t.world.push_back(im.entry);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s D%.1fxL%.1f 骨高%.1f 骨宽%.1f 神经%.1f 间距%.1f",
                 im.name.c_str(), im.diaMm, im.lengthMm, im.boneHeightMm, im.boneWidthMm,
                 im.nerveDistMm, im.minSpacingMm);
        t.text = buf;
        t.labelAnchored = 1;
        out.push_back(t);

        if (im.level != SAFE_GREEN && !im.warnText.empty()) {
            OverlayPrim w;
            w.kind = OK_TEXT;
            w.color = col;
            w.world.push_back(im.tip);
            w.text = im.warnText;
            w.labelAnchored = 1;
            out.push_back(w);
        }
        tagOwner(out, implantStart, OW_IMPLANT, im.id);
    }

}

// =============================================================================
// 持久化（PRD 8.4）
// =============================================================================
// =============================================================================
// 单对象 JSON <-> 结构体映射（持久化与 JNI 共用，见头文件注释）
// =============================================================================

Json MeasurementManager::roiToJson(const RoiDef &r) {
    Json j = Json::makeObject();
    j.set("id", Json::makeNumber(r.id));
    j.set("type", Json::makeNumber(r.type));
    j.set("name", Json::makeString(r.name));
    j.set("color", Json::makeNumber(r.color));
    j.set("visible", Json::makeBool(r.visible));
    j.set("opacity", Json::makeNumber(r.opacity));
    j.set("huMin", Json::makeNumber(r.huMin));
    j.set("huMax", Json::makeNumber(r.huMax));
    j.set("boxMin", vecJson(r.boxMin));
    j.set("boxMax", vecJson(r.boxMax));
    j.set("planeOrigin", vecJson(r.planeOrigin));
    j.set("planeNormal", vecJson(r.planeNormal));
    j.set("sphereCenter", vecJson(r.sphereCenter));
    j.set("sphereRadius", Json::makeNumber(r.sphereRadius));
    j.set("childA", Json::makeNumber(r.childA));
    j.set("childB", Json::makeNumber(r.childB));
    j.set("childC", Json::makeNumber(r.childC));
    j.set("opAB", Json::makeNumber(r.opAB));
    j.set("opAC", Json::makeNumber(r.opAC));
    j.set("polygon", pointsJson(r.polygon));
    j.set("plane", Json::makeNumber(r.plane));
    j.set("planePosition", Json::makeNumber(r.planePosition));
    j.set("aiLabel", Json::makeNumber(r.aiLabel));      // R-06
    return j;
}

void MeasurementManager::roiFromJson(const Json &jj, RoiDef &r) {
    r.id = jj.at("id").asInt(0);
    r.type = jj.at("type").asInt(ROI_HU_THRESHOLD);
    r.name = jj.at("name").asString();
    r.color = jj.at("color").asInt((int) 0xFFFFC400U);
    r.visible = jj.at("visible").asBool(true);
    r.opacity = jj.at("opacity").asNumber(0.35);
    r.huMin = jj.at("huMin").asNumber(200.0);
    r.huMax = jj.at("huMax").asNumber(3000.0);
    r.boxMin = vecFrom(jj.at("boxMin"));
    r.boxMax = vecFrom(jj.at("boxMax"));
    r.planeOrigin = vecFrom(jj.at("planeOrigin"));
    r.planeNormal = vecFrom(jj.at("planeNormal"));
    r.sphereCenter = vecFrom(jj.at("sphereCenter"));
    r.sphereRadius = jj.at("sphereRadius").asNumber(5.0);
    r.childA = jj.at("childA").asInt(0);
    r.childB = jj.at("childB").asInt(0);
    r.childC = jj.at("childC").asInt(0);
    r.opAB = jj.at("opAB").asInt(OP_INTERSECT);
    r.opAC = jj.at("opAC").asInt(OP_SUBTRACT);
    r.polygon = pointsFrom(jj.at("polygon"));
    r.plane = jj.at("plane").asInt(MP_AXIAL);
    r.planePosition = jj.at("planePosition").asInt(0);
    r.aiLabel = jj.at("aiLabel").asInt(0);              // R-06（旧存档无此键 -> 0）
}

Json MeasurementManager::implantToJson(const Implant &a) {
    Json j = Json::makeObject();
    j.set("id", Json::makeNumber(a.id));
    j.set("name", Json::makeString(a.name));
    j.set("color", Json::makeNumber(a.color));
    j.set("visible", Json::makeBool(a.visible));
    j.set("entry", vecJson(a.entry));
    j.set("pitchDeg", Json::makeNumber(a.pitchDeg));
    j.set("yawDeg", Json::makeNumber(a.yawDeg));
    j.set("depthMm", Json::makeNumber(a.depthMm));
    j.set("diaMm", Json::makeNumber(a.diaMm));
    j.set("lengthMm", Json::makeNumber(a.lengthMm));
    j.set("boneHeightMm", Json::makeNumber(a.boneHeightMm));
    j.set("boneWidthMm", Json::makeNumber(a.boneWidthMm));
    j.set("nerveDistMm", Json::makeNumber(a.nerveDistMm));
    j.set("minSpacingMm", Json::makeNumber(a.minSpacingMm));
    j.set("level", Json::makeNumber(a.level));
    j.set("warnText", Json::makeString(a.warnText));
    j.set("tip", vecJson(a.tip));
    j.set("axis", vecJson(a.axis));
    return j;
}

void MeasurementManager::implantFromJson(const Json &jj, Implant &a) {
    a.id = jj.at("id").asInt(0);
    a.name = jj.at("name").asString();
    a.color = jj.at("color").asInt((int) 0xFF00E676U);
    a.visible = jj.at("visible").asBool(true);
    a.entry = vecFrom(jj.at("entry"));
    a.pitchDeg = jj.at("pitchDeg").asNumber(0.0);
    a.yawDeg = jj.at("yawDeg").asNumber(0.0);
    a.depthMm = jj.at("depthMm").asNumber(10.0);
    a.diaMm = jj.at("diaMm").asNumber(4.0);
    a.lengthMm = jj.at("lengthMm").asNumber(11.0);
    a.boneHeightMm = jj.at("boneHeightMm").asNumber(0.0);
    a.boneWidthMm = jj.at("boneWidthMm").asNumber(0.0);
    a.nerveDistMm = jj.at("nerveDistMm").asNumber(0.0);
    a.minSpacingMm = jj.at("minSpacingMm").asNumber(0.0);
    a.level = jj.at("level").asInt(SAFE_GREEN);
    a.warnText = jj.at("warnText").asString();
}

Json MeasurementManager::measuresJson() const {
    Json root = Json::makeObject();
    root.set("formatVersion", Json::makeNumber(1));
    root.set("kind", Json::makeString("cbctmeasure.records"));
    if (vol_.valid()) {
        root.set("studyInstanceUID", Json::makeString(vol_.studyInstanceUID()));
        root.set("seriesInstanceUID", Json::makeString(vol_.seriesInstanceUID()));
    }

    Json recs = Json::makeArray();
    for (size_t i = 0; i < records_.size(); ++i) {
        const MeasureRecord &r = records_[i];
        Json j = Json::makeObject();
        j.set("id", Json::makeNumber(r.id));
        j.set("type", Json::makeNumber(r.type));
        j.set("name", Json::makeString(r.name));
        j.set("note", Json::makeString(r.note));
        j.set("color", Json::makeNumber(r.color));
        j.set("points", pointsJson(r.points));
        j.set("value", Json::makeNumber(r.value));
        j.set("unit", Json::makeString(r.unit));
        j.set("roiId", Json::makeNumber(r.roiId));
        j.set("visible", Json::makeBool(r.visible));
        j.set("createdAt", Json::makeNumber((double) r.createdAt));
        // detailJson 是"文本内嵌 JSON"：解析成功则展平成对象，失败原样保留字符串，
        // 这样坏掉的明细不会让整个文件读不回来
        const bool detOk = !r.detailJson.empty();
        if (detOk) {
            bool parsed = false;
            const Json d = Json::parse(r.detailJson, &parsed);
            j.set("detail", parsed ? d : Json::makeString(r.detailJson));
        }
        recs.push(j);
    }
    root.set("records", recs);

    Json roiArr = Json::makeArray();
    for (size_t i = 0; i < rois_.size(); ++i) roiArr.push(roiToJson(rois_[i]));
    root.set("rois", roiArr);
    return root;
}

Json MeasurementManager::planJson() const {
    Json root = Json::makeObject();
    root.set("formatVersion", Json::makeNumber(1));
    root.set("kind", Json::makeString("cbctmeasure.plan"));
    if (vol_.valid()) {
        root.set("studyInstanceUID", Json::makeString(vol_.studyInstanceUID()));
        root.set("seriesInstanceUID", Json::makeString(vol_.seriesInstanceUID()));
    }
    Json arr = Json::makeArray();
    for (size_t i = 0; i < implants_.size(); ++i) arr.push(implantToJson(implants_[i]));
    root.set("implants", arr);

    Json nerves = Json::makeArray();
    for (size_t i = 0; i < nerves_.size(); ++i) {
        const NervePath &p = nerves_[i];
        Json j = Json::makeObject();
        j.set("id", Json::makeNumber(p.id));
        j.set("name", Json::makeString(p.name));
        j.set("color", Json::makeNumber(p.color));
        j.set("visible", Json::makeBool(p.visible));
        j.set("radiusMm", Json::makeNumber(p.radiusMm));
        j.set("points", pointsJson(p.points));
        nerves.push(j);
    }
    root.set("nervePaths", nerves);
    return root;
}

bool MeasurementManager::measuresFromJson(const Json &j) {
    const Json &recs = j.at("records");
    if (!recs.isArray()) return false;
    records_.clear();
    for (size_t i = 0; i < recs.size(); ++i) {
        const Json &jj = recs[i];
        MeasureRecord r;
        r.id = jj.at("id").asInt(0);
        r.type = jj.at("type").asInt(MT_DISTANCE);
        r.name = jj.at("name").asString();
        r.note = jj.at("note").asString();
        r.color = jj.at("color").asInt((int) 0xFF00E5FFU);
        r.points = pointsFrom(jj.at("points"));
        r.value = jj.at("value").asNumber(0.0);
        r.unit = jj.at("unit").asString("mm");
        r.roiId = jj.at("roiId").asInt(0);
        r.visible = jj.at("visible").asBool(true);
        r.createdAt = (long long) jj.at("createdAt").asNumber(0.0);
        const Json &d = jj.at("detail");
        if (d.isObject()) r.detailJson = d.dump(false);
        else if (d.type() == Json::JT_STR) r.detailJson = d.asString();
        if (r.id > nextId_) nextId_ = r.id;
        records_.push_back(r);
    }

    const Json &rois = j.at("rois");
    rois_.clear();
    for (size_t i = 0; i < rois.size(); ++i) {
        RoiDef r;
        roiFromJson(rois[i], r);
        if (r.id > nextId_) nextId_ = r.id;
        rois_.push_back(r);
    }
    // 载入后一律按当前体数据重算：旧文件里的数值可能来自不同 spacing 的序列
    for (size_t i = 0; i < records_.size(); ++i) recalcMeasure(records_[i].id);
    LOGD("measures loaded: %zu records, %zu rois", records_.size(), rois_.size());
    return true;
}

bool MeasurementManager::planFromJson(const Json &j) {
    const Json &arr = j.at("implants");
    if (!arr.isArray()) return false;
    implants_.clear();
    for (size_t i = 0; i < arr.size(); ++i) {
        Implant a;
        implantFromJson(arr[i], a);
        if (a.id > nextId_) nextId_ = a.id;
        implants_.push_back(a);
    }
    nerves_.clear();
    const Json &ns = j.at("nervePaths");
    for (size_t i = 0; i < ns.size(); ++i) {
        const Json &jj = ns[i];
        NervePath p;
        p.id = jj.at("id").asInt(0);
        p.name = jj.at("name").asString();
        p.color = jj.at("color").asInt((int) 0xFFFF1744U);
        p.visible = jj.at("visible").asBool(true);
        p.radiusMm = jj.at("radiusMm").asNumber(1.5);
        p.points = pointsFrom(jj.at("points"));
        if (p.id > nextId_) nextId_ = p.id;
        nerves_.push_back(p);
    }
    recomputePlan();
    LOGD("plan loaded: %zu implants, %zu nerve paths", implants_.size(), nerves_.size());
    return true;
}

std::string MeasurementManager::summaryText() const {
    char buf[192];
    int danger = 0, warn = 0;
    for (size_t i = 0; i < implants_.size(); ++i) {
        if (implants_[i].level == DANGER_RED) ++danger;
        else if (implants_[i].level == WARN_YELLOW) ++warn;
    }
    snprintf(buf, sizeof(buf), "测量 %zu 项 / ROI %zu 个 / 种植体 %zu 颗（危险 %d 临界 %d）/ 神经管 %zu 条",
             records_.size(), rois_.size(), implants_.size(), danger, warn, nerves_.size());
    return std::string(buf);
}
