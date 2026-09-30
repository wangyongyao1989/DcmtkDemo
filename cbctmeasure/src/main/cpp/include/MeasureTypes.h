#ifndef DCMTKDEMO_MEASURETYPES_H
#define DCMTKDEMO_MEASURETYPES_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

/**
 * CBCT 测量与手术规划的数据类型定义（纯 C++，无 JNI / 无 VTK 依赖）。
 *
 * 坐标约定（与 :cbctdeal 的 CbctVtkRenderer::init() 严格一致）：
 *   世界坐标一律为毫米，原点为体数据首体素中心，轴与体素索引同向，
 *   即 world = index * spacing。这样测量值与 DICOM PixelSpacing /
 *   SliceThickness 直接对应，无需再考虑 ImagePositionPatient 旋转
 *   （本工程解析侧已按 Z 轴升序重排并归一化为右手系堆叠）。
 *
 * 枚举值与 Kotlin 侧 com.wangyao.cbctmeasure.model.MeasureEnums 一一对应，
 * 修改任意一侧必须同步另一侧（跨语言只传 int，不传名字）。
 */

/**
 * 测量类型。
 *
 * 0 ~ 7 是 PRD 5.1.2 表 M-01 ~ M-08；
 * 8 ~ 11 是 PRD 5.3.3 表 S-07 ~ S-10（正畸评估，一期范围内）。
 * 后者刻意并入同一张类型表而不是另开一套接口，这样 AC-10 的 9 态手势机
 * 不用扩状态：四类正畸量都走"测距"态的点选/拖动通道，只是 requiredPoints
 * 和 core 的计算分支不同。
 */
enum MeasureType {
    MT_DISTANCE = 0,       // M-01 两点距离（mm）
    MT_ANGLE = 1,          // M-02 三点角度（∠ABC，B 为顶点，度）
    MT_POINT_TO_LINE = 2,  // M-03 点到线段垂直距离（mm）
    MT_ROI_VOLUME = 3,     // M-04 ROI 体积（cm³）
    MT_ROI_AREA = 4,       // M-05 ROI 面积（cm²，MPR 封闭路径）
    MT_ARC_LENGTH = 5,     // M-06 弧线长度（mm，累积弦长）
    MT_HU_SAMPLE = 6,      // M-07 HU 值采样 + 组织类型推断
    MT_BONE_DENSITY = 7,   // M-08 ROI 骨密度统计
    MT_ARCH_LENGTH = 8,    // S-07 牙弓弧线长度（mm，冠状面描记）
    MT_TOOTH_ANGULATION = 9,   // S-08 牙齿排列角度偏差（度，两条方向线）
    MT_MIDLINE_OFFSET = 10,    // S-09 上下颌中线偏移（mm，水平分量）
    MT_OVERBITE = 11,          // S-10 覆合（垂直，主值）+ 覆盖（前后向，明细）
};

/** ROI 定义方式（PRD 5.2.2 表 R-01 ~ R-05；R-06 为本模块 AI-01 扩展） */
enum RoiType {
    ROI_HU_THRESHOLD = 0,  // R-01 HU 阈值范围
    ROI_BOX = 1,           // R-02 空间裁剪盒
    ROI_PLANE = 2,         // R-03 平面裁剪（保留法线正向一侧）
    ROI_SPHERE = 3,        // R-04 球面 ROI
    ROI_COMPOSITE = 4,     // R-05 组合 ROI（子 ROI 交/并/差）
    ROI_AI_MASK = 5,       // R-06 AI-01 分割掩膜实例（PRD 5.6.1）
};

/** 手势/工具状态机（PRD 8.5，共 9 态，对应 AC-10） */
enum ToolState {
    ST_VIEW = 0,              // 浏览：单指旋转 / 双指缩放平移
    ST_MEASURE_DISTANCE = 1,  // 两点距离
    ST_MEASURE_ANGLE = 2,     // 三点角度
    ST_MEASURE_VOLUME = 3,    // 拖拽定义裁剪盒 -> 体积
    ST_MEASURE_AREA = 4,      // MPR 封闭路径 -> 面积
    ST_ROI_EDIT = 5,          // ROI 参数编辑
    ST_IMPLANT_PLACE = 6,     // 种植体定位
    ST_ANNOTATE = 7,          // 自由标注
    ST_NERVE_TRACE = 8,       // 神经管逐层描记
};

/** MPR 平面（与 :cbctdeal CbctVtkJni.PLANE_* 一致） */
enum MeasurePlane {
    MP_AXIAL = 0,
    MP_CORONAL = 1,
    MP_SAGITTAL = 2,
};

/** 标注类型（PRD 5.4.2 表 A-01 ~ A-07） */
enum AnnotationType {
    AN_TEXT_LABEL = 0,     // A-01 文字标签（绑定 3D 点）
    AN_LINE = 1,           // A-02 线段标注（含长度）
    AN_ARROW = 2,          // A-03 箭头标注
    AN_FREE_CURVE = 3,     // A-04 自由曲线（折线）
    AN_RING = 4,           // A-05 环形标记
    AN_MPR_SLICE = 5,      // A-06 MPR 截面标注（绑定平面 + 层位）
    AN_SCREENSHOT = 6,     // A-07 截图标注（图像像素坐标，报告合成用）
};

/** 组合 ROI 的布尔算子（R-05：A AND B AND NOT C） */
enum CombineOp {
    OP_NONE = 0,
    OP_INTERSECT = 1,
    OP_UNION = 2,
    OP_SUBTRACT = 3,
};

/** 种植体安全等级（S-02 颜色语义） */
enum SafetyLevel {
    SAFE_GREEN = 0,    // 全部指标达标
    WARN_YELLOW = 1,   // 临界：任一指标落在 [阈值, 阈值*1.2)
    DANGER_RED = 2,    // 不安全：任一指标低于阈值
};

/** 三维点（mm，世界坐标） */
struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;

    Vec3() = default;
    Vec3(double xx, double yy, double zz) : x(xx), y(yy), z(zz) {}

    Vec3 operator+(const Vec3 &o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3 &o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(double k) const { return Vec3(x * k, y * k, z * k); }

    double dot(const Vec3 &o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3 &o) const {
        return Vec3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x);
    }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        const double l = length();
        return l > 1e-12 ? Vec3(x / l, y / l, z / l) : Vec3(0, 0, 0);
    }
};

/**
 * 单条测量记录。
 * value/unit 为主结果；detailJson 承载派生明细（如骨密度的均值/标准差、
 * 角度测量的三边长），便于报告与 SR 直接取用而不必重算。
 */
struct MeasureRecord {
    int id = 0;
    int type = MT_DISTANCE;
    std::string name;              // UI 可重命名
    std::string note;              // 备注（报告/ SR 输出）
    int color = 0xFF00E5FFU;       // 0xAARRGGBB
    std::vector<Vec3> points;      // 基准点（世界 mm）
    double value = 0.0;
    std::string unit = "mm";
    std::string detailJson;        // 派生明细（JSON 文本）
    int roiId = 0;                 // M-04 / M-05 / M-08 关联的 ROI（0 = 无）
    bool visible = true;
    long long createdAt = 0;       // epoch millis
};

/** ROI 定义（参数按 type 取用；组合 ROI 使用子 ROI 引用；R-06 用 aiLabel） */
struct RoiDef {
    int id = 0;
    int type = ROI_HU_THRESHOLD;
    std::string name;
    int color = 0xFFFFC400U;
    bool visible = true;
    double opacity = 0.35;

    double huMin = 200.0, huMax = 3000.0;      // R-01
    Vec3 boxMin, boxMax;                        // R-02
    Vec3 planeOrigin, planeNormal;              // R-03（法线正向一侧保留）
    Vec3 sphereCenter;                          // R-04
    double sphereRadius = 5.0;

    int childA = 0, childB = 0, childC = 0;     // R-05
    int opAB = OP_INTERSECT, opAC = OP_SUBTRACT;

    /**
     * R-06（AI-01）：掩膜实例号 = AiInstance.id。
     * 之所以只存一个整数而不是掩膜本身：掩膜是"本次推理的产物"，量大且随
     * 体数据/模型变化，它归 MeasurementManager 的 AiResult 持有；ROI 只是
     * 一个引用，因此可以随 measuresJson 一起落盘、被 R-05 组合、被 M-04/M-08 复用。
     * 推理结果被清除后该 ROI 统计为空并在 error 里说明，不会崩溃。
     */
    int aiLabel = 0;

    std::vector<Vec3> polygon;                  // M-05 截面封闭路径
    int plane = MP_AXIAL;                       // 多边形所在平面
    int planePosition = 0;                      // 平面层位（体素索引）
};

/** 种植体（S-01 参数 + S-03~S-06 计算结果） */
struct Implant {
    int id = 0;
    std::string name;
    int color = 0xFF00E676U;
    bool visible = true;

    Vec3 entry;             // 植入点（骨皮质表面入口，世界 mm）
    double pitchDeg = 0.0;  // 俯仰角：绕世界 X 轴（0 = 沿 -Z 轴向根方）
    double yawDeg = 0.0;    // 偏转角：绕世界 Y 轴
    double depthMm = 10.0;  // 植入深度（轴向长度）
    double diaMm = 4.0;     // 直径
    double lengthMm = 11.0; // 桩体全长（可视化长度，>= depth）

    // ---- 计算结果（computeSafety 后填充）----
    double boneHeightMm = 0.0;   // S-03 沿轴可用骨高度
    double boneWidthMm = 0.0;    // S-04 颊舌向骨宽度
    double nerveDistMm = 0.0;    // S-05 桩体到神经管折线最短距离
    double minSpacingMm = 0.0;   // S-06 与相邻种植体最短距离
    int level = SAFE_GREEN;      // S-02 安全等级
    std::string warnText;        // 中文告警摘要（UI/报告直接显示）
    Vec3 tip;                    // 桩体尖端（entry + axis * length）
    Vec3 axis;                   // 单位轴向
};

/** 神经管路径（S-05 依据；矢状面逐层描记的点序列） */
struct NervePath {
    int id = 0;
    std::string name;
    int color = 0xFFFF1744U;
    bool visible = true;
    std::vector<Vec3> points;
    double radiusMm = 1.5;    // 管道显示半径（vtkTubeFilter 的替代：叠加层描边宽度）
};

/** 标注（A-01 ~ A-07） */
struct Annotation {
    int id = 0;
    int type = AN_TEXT_LABEL;
    std::string text;
    int color = 0xFFFFFFFFU;
    bool visible = true;
    int measureId = 0;         // 关联测量项 id（0 = 独立标注）
    std::vector<Vec3> points;  // A-01~A-06：世界 mm；A-07：图像像素坐标
    double radiusMm = 3.0;     // A-05 环形半径（世界 mm；A-07 时为像素）
    int plane = MP_AXIAL;      // A-06 绑定的 MPR 平面
    int planePosition = 0;     // A-06 绑定层位
};

/** 叠加图元（Native 产出、Kotlin Canvas 消费的中间表示） */
struct OverlayPrim {
    int kind = 0;        // 见 OverlayKind
    int color = 0xFF00E5FFU;
    double widthPx = 2.0;
    bool fill = false;
    int arrowHead = 0;   // 1 = 末点绘制箭头（A-03；世界坐标折线无法表达箭头，交给 Canvas）
    std::vector<Vec3> world;   // 顶点（世界 mm；屏幕系图元由 Kotlin 侧换算）
    std::string text;
    int labelAnchored = 0;     // 1 = 文本绘制在 points 末点旁
    /**
     * 归属对象（PRD 5.1.4"列表选中 -> 图形高亮"）。
     * 早期版本是让 Kotlin 在 text 里找 "#id" 子串来匹配，一旦用户重命名或
     * 两条记录 id 互为前缀就会误高亮，所以这里显式带出 kind + id。
     * ownerKind 见 OverlayOwner。
     */
    int ownerKind = 0;
    int ownerId = 0;
};

/** 叠加图元的归属对象类型（0 = 不属于任何可选中对象） */
enum OverlayOwner {
    OW_NONE = 0,
    OW_ROI = 1,
    OW_MEASURE = 2,
    OW_NERVE = 3,
    OW_IMPLANT = 4,
    OW_ANNOTATION = 5,
    OW_AI = 6,          // AI-01 分割实例（id = AiInstance.id）
};

enum OverlayKind {
    OK_POINT = 0,       // 标记点（十字 + 圆）
    OK_LINE = 1,        // 折线
    OK_POLYLINE = 2,    // 开放折线（多段）
    OK_POLYGON = 3,     // 闭合多边形（可选填充）
    OK_BOX = 4,         // 长方体 12 条棱（world 为 8 顶点）
    OK_CAPSULE = 5,     // 圆柱轮廓（world 为两圈顶点）
    OK_TUBE = 6,        // 管道折线（神经管）
    OK_RING = 7,        // 圆环
    OK_TEXT = 8,        // 纯文字
    OK_CIRCLE_PX = 9,   // 屏幕空间圆（半径像素）
};

#endif // DCMTKDEMO_MEASURETYPES_H
