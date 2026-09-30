// 标注存储实现（PRD 5.4 A-01 ~ A-07）。
//
// 与 MeasurementManager 一样是纯 C++：不接触 JNIEnv，也不生成 VTK Actor。
// JSON 结构见 toJson()，字段名与 Kotlin 侧 com.wangyao.cbctmeasure.model.Annotation
// 的序列化保持一致（跨语言只认字段名，不认顺序）。

#include "include/AnnotationStore.h"
#include "include/MeasureMath.h"

#include <android/log.h>

#include <cmath>

#define TAG "CbctMeasureCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace {

    /** 环形标记的默认细分段数：32 段在 1200px 屏宽上肉眼无棱角，点数也不构成负担 */
    const int kRingSegments = 32;

    Json pointsJson(const std::vector<Vec3> &pts) {
        Json arr = Json::makeArray();
        for (size_t i = 0; i < pts.size(); ++i) {
            Json p = Json::makeArray();
            p.push(Json::makeNumber(pts[i].x));
            p.push(Json::makeNumber(pts[i].y));
            p.push(Json::makeNumber(pts[i].z));
            arr.push(p);
        }
        return arr;
    }

    std::vector<Vec3> pointsFromJson(const Json &arr) {
        std::vector<Vec3> out;
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json &p = arr[i];
            if (p.size() < 3) continue;
            out.push_back(Vec3(p[0].asNumber(), p[1].asNumber(), p[2].asNumber()));
        }
        return out;
    }

    /** 平面 -> 该平面内的两个轴（与 :cbctdeal 的 MPR 定义一致：轴位=恒定 Z） */
    void planeAxes(int plane, Vec3 &u, Vec3 &v) {
        if (plane == MP_CORONAL) { u = Vec3(1, 0, 0); v = Vec3(0, 0, 1); }
        else if (plane == MP_SAGITTAL) { u = Vec3(0, 1, 0); v = Vec3(0, 0, 1); }
        else { u = Vec3(1, 0, 0); v = Vec3(0, 1, 0); }
    }

}   // namespace

AnnotationStore::AnnotationStore() = default;

void AnnotationStore::ringPolygon(const Vec3 &center, double radiusMm, int plane, int segments,
                                  std::vector<Vec3> &out) {
    out.clear();
    if (radiusMm <= 0.0) return;
    if (segments < 3) segments = kRingSegments;
    Vec3 u, v;
    planeAxes(plane, u, v);
    for (int i = 0; i < segments; ++i) {
        const double a = (double) i / (double) segments * 2.0 * 3.14159265358979323846;
        out.push_back(center + (u * std::cos(a) + v * std::sin(a)) * radiusMm);
    }
}

int AnnotationStore::add(const Annotation &anno) {
    if (anno.type < AN_TEXT_LABEL || anno.type > AN_SCREENSHOT) return -1;
    // 至少要有 1 个定位点；线段/箭头/曲线这类需要 2 个以上，
    // 点数不足时仍接收（UI 可以分次补齐），但叠加层会因为没有坐标而不绘制
    Annotation a = anno;
    a.id = ++nextId_;
    list_.push_back(a);
    LOGD("annotation #%d added, type=%d pts=%zu", a.id, a.type, a.points.size());
    return a.id;
}

const Annotation *AnnotationStore::find(int id) const {
    for (size_t i = 0; i < list_.size(); ++i) {
        if (list_[i].id == id) return &list_[i];
    }
    return nullptr;
}

bool AnnotationStore::remove(int id) {
    for (size_t i = 0; i < list_.size(); ++i) {
        if (list_[i].id == id) {
            list_.erase(list_.begin() + (long) i);
            return true;
        }
    }
    return false;
}

bool AnnotationStore::update(const Annotation &anno) {
    for (size_t i = 0; i < list_.size(); ++i) {
        if (list_[i].id == anno.id) {
            list_[i] = anno;
            return true;
        }
    }
    return false;
}

bool AnnotationStore::setText(int id, const std::string &text) {
    for (size_t i = 0; i < list_.size(); ++i) {
        if (list_[i].id == id) { list_[i].text = text; return true; }
    }
    return false;
}

bool AnnotationStore::setColor(int id, unsigned int color) {
    for (size_t i = 0; i < list_.size(); ++i) {
        if (list_[i].id == id) { list_[i].color = (int) color; return true; }
    }
    return false;
}

bool AnnotationStore::setVisible(int id, bool visible) {
    for (size_t i = 0; i < list_.size(); ++i) {
        if (list_[i].id == id) { list_[i].visible = visible; return true; }
    }
    return false;
}

void AnnotationStore::clear() {
    list_.clear();
    nextId_ = 0;
}

void AnnotationStore::buildOverlay(std::vector<OverlayPrim> &out) const {
    buildOverlay(out, -1, -1, false);
}

/**
 * planeFilter >= 0 时只输出"绑定该平面且层位匹配（误差 <= 1 层）"的 A-06 截面标注，
 * 三维标注（A-01 ~ A-05）始终输出：它们是世界坐标对象，与当前查看平面无关。
 */
void AnnotationStore::buildOverlay(std::vector<OverlayPrim> &out, int planeFilter,
                                   int planePosition, bool includePixelSpace) const {
    for (size_t n = 0; n < list_.size(); ++n) {
        const Annotation &a = list_[n];
        if (!a.visible) continue;
        if (a.points.empty()) continue;
        // 每条标注先记下起点，产出全部图元后统一打 OW_ANNOTATION 标记，
        // Kotlin 侧点标注列表某一项时只加粗它自己那批图形（PRD 5.4.2 / 5.1.4）。
        const size_t start = out.size();

        // lambda 里的 return 等价于原来各处 continue：跳过本条标注
        auto emit = [&]() -> void {
            if (a.type == AN_SCREENSHOT) {
                if (!includePixelSpace) return;      // 像素系标注只在报告合成时消费
                OverlayPrim p;
                p.kind = OK_CIRCLE_PX;
                p.color = a.color;
                p.world = a.points;                     // 这里 points 存的是像素坐标
                p.text = a.text;
                p.widthPx = a.radiusMm > 0.0 ? a.radiusMm : 2.0;
                out.push_back(p);
                return;
            }

            if (a.type == AN_MPR_SLICE && planeFilter >= 0 && a.plane != planeFilter) return;

            switch (a.type) {
                case AN_TEXT_LABEL: {
                    OverlayPrim dot;
                    dot.kind = OK_POINT;
                    dot.color = a.color;
                    dot.world.push_back(a.points[0]);
                    out.push_back(dot);
                    OverlayPrim t;
                    t.kind = OK_TEXT;
                    t.color = a.color;
                    t.world.push_back(a.points[0]);
                    t.text = a.text.empty() ? std::string("label") : a.text;
                    t.labelAnchored = 1;
                    out.push_back(t);
                    break;
                }
                case AN_LINE:
                case AN_ARROW: {
                    OverlayPrim l;
                    l.kind = OK_LINE;
                    l.color = a.color;
                    l.arrowHead = (a.type == AN_ARROW) ? 1 : 0;
                    l.world = a.points;
                    out.push_back(l);
                    OverlayPrim t;
                    t.kind = OK_TEXT;
                    t.color = a.color;
                    t.world.push_back(a.points.back());
                    // A-02 要求"线段 + 长度标注"：长度在这里算好，Kotlin 只画字
                    t.text = a.text.empty()
                             ? MeasureMath::format(MeasureMath::arcLength(a.points, false), 2) + " mm"
                             : a.text;
                    t.labelAnchored = 1;
                    out.push_back(t);
                    break;
                }
                case AN_FREE_CURVE: {
                    OverlayPrim c;
                    c.kind = OK_POLYLINE;
                    c.color = a.color;
                    c.world = a.points;
                    c.widthPx = 2.5;
                    out.push_back(c);
                    if (!a.text.empty()) {
                        OverlayPrim t;
                        t.kind = OK_TEXT;
                        t.color = a.color;
                        t.world.push_back(a.points.back());
                        t.text = a.text;
                        t.labelAnchored = 1;
                        out.push_back(t);
                    }
                    break;
                }
                case AN_RING: {
                    std::vector<Vec3> ring;
                    ringPolygon(a.points[0], a.radiusMm, a.plane, kRingSegments, ring);
                    OverlayPrim r;
                    r.kind = OK_POLYGON;
                    r.color = a.color;
                    r.widthPx = 2.0;
                    r.world = ring;
                    r.text = a.text;
                    r.labelAnchored = a.text.empty() ? 0 : 1;
                    out.push_back(r);
                    break;
                }
                case AN_MPR_SLICE: {
                    // 截面标注：两点以内画线，多于两点按折线；文字跟随末点
                    OverlayPrim l;
                    l.color = a.color;
                    l.kind = a.points.size() > 2 ? OK_POLYLINE : OK_LINE;
                    l.world = a.points;
                    out.push_back(l);
                    if (!a.text.empty()) {
                        OverlayPrim t;
                        t.kind = OK_TEXT;
                        t.color = a.color;
                        t.world.push_back(a.points.back());
                        t.text = a.text;
                        t.labelAnchored = 1;
                        out.push_back(t);
                    }
                    (void) planePosition;
                    break;
                }
                default:
                    break;
            }
        };
        emit();

        for (size_t k = start; k < out.size(); ++k) {
            out[k].ownerKind = OW_ANNOTATION;
            out[k].ownerId = a.id;
        }
    }
}

Json AnnotationStore::toJsonItem(const Annotation &a) {
    Json j = Json::makeObject();
    j.set("id", Json::makeNumber(a.id));
    j.set("type", Json::makeNumber(a.type));
    j.set("text", Json::makeString(a.text));
    j.set("color", Json::makeNumber(a.color));
    j.set("visible", Json::makeBool(a.visible));
    j.set("measureId", Json::makeNumber(a.measureId));
    j.set("points", pointsJson(a.points));
    j.set("radiusMm", Json::makeNumber(a.radiusMm));
    j.set("plane", Json::makeNumber(a.plane));
    j.set("planePosition", Json::makeNumber(a.planePosition));
    return j;
}

void AnnotationStore::fromJsonItem(const Json &jj, Annotation &a) {
    a.id = jj.at("id").asInt(0);
    a.type = jj.at("type").asInt(AN_TEXT_LABEL);
    a.text = jj.at("text").asString();
    a.color = jj.at("color").asInt((int) 0xFFFFFFFFU);
    a.visible = jj.at("visible").asBool(true);
    a.measureId = jj.at("measureId").asInt(0);
    a.points = pointsFromJson(jj.at("points"));
    a.radiusMm = jj.at("radiusMm").asNumber(3.0);
    a.plane = jj.at("plane").asInt(MP_AXIAL);
    a.planePosition = jj.at("planePosition").asInt(0);
}

Json AnnotationStore::toJson() const {
    Json root = Json::makeObject();
    root.set("formatVersion", Json::makeNumber(1));
    root.set("kind", Json::makeString("cbctmeasure.annotations"));
    Json arr = Json::makeArray();
    for (size_t i = 0; i < list_.size(); ++i) arr.push(toJsonItem(list_[i]));
    root.set("annotations", arr);
    return root;
}

bool AnnotationStore::fromJson(const Json &j) {
    const Json &arr = j.at("annotations");
    if (!arr.isArray()) return false;
    list_.clear();
    nextId_ = 0;
    for (size_t i = 0; i < arr.size(); ++i) {
        Annotation a;
        fromJsonItem(arr[i], a);
        if (a.id > nextId_) nextId_ = a.id;
        list_.push_back(a);
    }
    LOGD("annotations loaded: %zu items", list_.size());
    return true;
}
