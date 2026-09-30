#ifndef DCMTKDEMO_VOLUMEREF_H
#define DCMTKDEMO_VOLUMEREF_H

#include <string>
#include <vector>

#include "MeasureTypes.h"

struct CbctVolume;   // 前向声明：来自 :cbctdeal 的 CbctVolume.h

/**
 * 只读体数据视图（业务逻辑层唯一的 Volume 入口）。
 *
 * 跨 .so 约束（重要）：
 *   CbctVolume 由 libcbct_native.so（:cbctdeal）分配，本类持有的指针
 *   永远只读、永不释放。生命周期规则与 Java 侧一致：必须先销毁依赖它的
 *   测量会话，再调用 CbctJni.releaseVolume()，否则悬垂。
 *
 * 坐标换算约定（与 CbctVtkRenderer::init 中 SetOrigin(0,0,0) 严格一致）：
 *   world(mm) = index * spacing
 *   index     = world / spacing
 * 因此测量算法全部工作在"世界 = 毫米"空间，结果与 DICOM
 * PixelSpacing / SliceThickness 直接对应，误差只来自浮点本身。
 */
class VolumeRef {
public:
    VolumeRef() = default;

    /** 绑定 :cbctdeal 的体数据（允许 nullptr，所有查询退化为安全默认值） */
    void bind(const CbctVolume *vol);

    bool valid() const { return vol_ != nullptr; }
    const CbctVolume *raw() const { return vol_; }

    /**
     * 体素数组直读指针（[depth][height][width] 连续 float HU，布局见 CbctVolume.h）。
     * 暴露这两个访问器是为了让 core/ 下的统计循环不必 include CbctVolume.h
     * （该头带 DCMTK 依赖，会把主机侧单测一起拖进交叉编译环境）。
     */
    const float *data() const;
    size_t sliceSize() const;

    /**
     * 序列标识（JSON 归档键 / DICOM SR 的 Study 与 Series 引用）。
     * 同样在 VolumeRef.cpp 内取值，理由与 data() 相同。
     */
    std::string studyInstanceUID() const;
    std::string seriesInstanceUID() const;
    /** 首个有效切片文件路径（SR 图像证据引用；可能为空） */
    std::string firstSlicePath() const;
    /** 患者信息（SR 报告用；未解析到则为空串） */
    std::string patientName() const;
    std::string patientID() const;
    std::string patientSex() const;
    std::string patientBirthDate() const;
    std::string studyDate() const;
    std::string seriesDescription() const;
    std::string manufacturer() const;

    int width() const;
    int height() const;
    int depth() const;
    double spacingX() const;
    double spacingY() const;
    double spacingZ() const;
    double voxelVolumeMm3() const;              // spacingX*Y*Z
    void boundsMin(Vec3 &out) const;            // (0,0,0)
    void boundsMax(Vec3 &out) const;            // ((w-1)*sx, (h-1)*sy, (d-1)*sz)
    double maxDiagonalMm() const;

    /** 体素索引 -> 世界毫米 */
    void indexToWorld(int i, int j, int k, Vec3 &out) const;
    /** 世界毫米 -> 体素索引（四舍五入，不钳制，越界由调用方判断） */
    void worldToIndex(const Vec3 &p, int &i, int &j, int &k) const;
    bool indexInBounds(int i, int j, int k) const;
    bool worldInBounds(const Vec3 &p) const;

    /** 最近邻取样（越界返回 fallback）；用于"点按即读数"这类精确单点需求 */
    float huNearest(const Vec3 &p, float fallback = 0.0f) const;

    /**
     * 三线性插值取样。
     * 用于连续量（骨密度曲线、种植体轴线采样）：避免最近邻在 0.3mm 层厚
     * 数据上造成的台阶，使 S-03 骨高度边界更稳定。
     */
    float huTrilinear(const Vec3 &p, float fallback = 0.0f) const;

    /** HU -> 组织类型名称（M-07 输出，PRD 5.1.2） */
    static std::string tissueName(float hu);

private:
    float at(int i, int j, int k) const;

    const CbctVolume *vol_ = nullptr;   // 非拥有，只读
};

#endif // DCMTKDEMO_VOLUMEREF_H
