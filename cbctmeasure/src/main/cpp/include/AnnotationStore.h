#ifndef DCMTKDEMO_ANNOTATIONSTORE_H
#define DCMTKDEMO_ANNOTATIONSTORE_H

#include <string>
#include <vector>

#include "include/Json.h"
#include "include/MeasureTypes.h"

/**
 * 标注存储（PRD 5.4 A-01 ~ A-07，7.3 的 AnnotationRenderer 数据侧）。
 *
 * 渲染方式说明：本模块不链接 VTK，标注不生成 vtkActor/vtkBillboardTextActor3D，
 * 而是产出"世界坐标图元"交给 Kotlin 的 MeasureOverlayView：
 *   - 点位经 :cbctdeal 的 projectPoints() 用 vtkCoordinate 投影（与 VTK 相机一致）；
 *   - 环形标记（A-05）在这里就展开成 32 边形顶点，透视下形状正确，
 *     比在屏幕空间画圆更符合"贴附解剖结构"的临床预期；
 *   - 箭头（A-03）用 arrowHead 标志交由 Canvas 画箭头，世界折线本身是线段。
 * A-07 截图标注使用图像像素坐标，不参与三维叠加，仅在报告合成时消费。
 */
class AnnotationStore {
public:
    AnnotationStore();

    /** 追加并返回分配的 id（<=0 表示入参非法） */
    int add(const Annotation &anno);
    bool remove(int id);
    bool update(const Annotation &anno);
    bool setText(int id, const std::string &text);
    bool setColor(int id, unsigned int color);
    bool setVisible(int id, bool visible);
    void clear();

    const std::vector<Annotation> &all() const { return list_; }
    const Annotation *find(int id) const;
    size_t count() const { return list_.size(); }

    /** 生成叠加图元（跳过 visible=false 与 A-07 像素标注） */
    void buildOverlay(std::vector<OverlayPrim> &out, int planeFilter, int planePosition,
                      bool includePixelSpace) const;

    /** 便捷重载：输出全部三维标注 */
    void buildOverlay(std::vector<OverlayPrim> &out) const;

    Json toJson() const;
    bool fromJson(const Json &j);

    /**
     * 单条标注的 JSON <-> 结构体映射（public static）。
     * toJson/fromJson 的循环体与 JNI 层（AnnotationJni.addAnnotation 的入参、
     * 列表回传）共用同一套字段名，避免"存得回来但界面上建不出来"的漂移。
     */
    static Json toJsonItem(const Annotation &a);

    /** 字段缺失时保留 Annotation 结构体默认值 */
    static void fromJsonItem(const Json &j, Annotation &out);

    /** 环形标记展开为多边形顶点（世界 mm）；A-05 与报告缩略图共用 */
    static void ringPolygon(const Vec3 &center, double radiusMm, int plane, int segments,
                            std::vector<Vec3> &out);

private:
    std::vector<Annotation> list_;
    int nextId_ = 0;
};

#endif // DCMTKDEMO_ANNOTATIONSTORE_H
