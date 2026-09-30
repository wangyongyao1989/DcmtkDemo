package com.wangyao.cbctmeasure.model

/**
 * 跨语言枚举镜像（PRD 5.1.2 / 5.2.2 / 5.3.2 / 5.4.2 / 8.5）。
 *
 * 数值是协议的一部分：core/MeasureTypes.h 里同名枚举的整型值必须与这里逐位一致，
 * 跨 JNI 只传 int 不传名字。改动任何一侧都要同步另一侧，
 * 且新增类型时 MeasureJni.addMeasure 的 type 与 RoiJni 的 type 都是直通 C++ 的。
 */
object MeasureType {
    const val DISTANCE = 0          // M-01 两点距离（mm）
    const val ANGLE = 1             // M-02 三点角度（∠ABC，B 为顶点，度）
    const val POINT_TO_LINE = 2     // M-03 点到线段垂直距离（mm）
    const val ROI_VOLUME = 3        // M-04 ROI 体积（cm³）
    const val ROI_AREA = 4          // M-05 ROI 截面面积（cm²）
    const val ARC_LENGTH = 5        // M-06 弧线长度（mm，累积弦长）
    const val HU_SAMPLE = 6         // M-07 HU 采样 + 组织类型
    const val BONE_DENSITY = 7      // M-08 ROI 骨密度统计

    // ---- PRD 5.3.3 正畸评估（S-07 ~ S-10，一期范围内）----
    // 刻意并入同一张类型表：四类量都复用"测距"态的点选/拖动通道，
    // 9 态手势机（AC-10）不因此扩容。
    const val ARCH_LENGTH = 8       // S-07 牙弓弧线长度（mm，冠状面描记）
    const val TOOTH_ANGULATION = 9  // S-08 牙齿排列角度偏差（度，两条长轴）
    const val MIDLINE_OFFSET = 10   // S-09 上下颌中线偏移（mm，水平分量）
    const val OVERBITE = 11         // S-10 覆合（主值 mm）+ 覆盖（明细 mm）

    /** UI 名称（PRD 表里的中文项名） */
    fun label(type: Int): String = when (type) {
        DISTANCE -> "距离"
        ANGLE -> "角度"
        POINT_TO_LINE -> "点到线"
        ROI_VOLUME -> "ROI 体积"
        ROI_AREA -> "ROI 面积"
        ARC_LENGTH -> "弧线长度"
        HU_SAMPLE -> "HU 采样"
        BONE_DENSITY -> "骨密度"
        ARCH_LENGTH -> "牙弓弧线"
        TOOTH_ANGULATION -> "排列角度"
        MIDLINE_OFFSET -> "中线偏移"
        OVERBITE -> "覆合/覆盖"
        else -> "未知测量"
    }

    /**
     * 小数位（PRD 6 精度指标：距离 ≤0.1mm、角度 ≤0.5°、体积 ≤1%）。
     * 显示位数按"量化误差不超过指标的一半"取：mm 两位、deg 一位、cm³ 三位。
     */
    fun decimals(type: Int): Int = when (type) {
        ANGLE, TOOTH_ANGULATION -> 1
        ROI_VOLUME, ROI_AREA -> 3
        HU_SAMPLE, BONE_DENSITY -> 0
        else -> 2
    }
}

object RoiType {
    const val HU_THRESHOLD = 0      // R-01 HU 阈值范围
    const val BOX = 1               // R-02 空间裁剪盒
    const val PLANE = 2             // R-03 平面裁剪（保留法线正向一侧）
    const val SPHERE = 3            // R-04 球面 ROI
    const val COMPOSITE = 4         // R-05 组合 ROI（子 ROI 交/并/差）

    fun label(type: Int): String = when (type) {
        HU_THRESHOLD -> "HU 阈值"
        BOX -> "裁剪盒"
        PLANE -> "平面裁剪"
        SPHERE -> "球面 ROI"
        COMPOSITE -> "组合 ROI"
        else -> "未知 ROI"
    }
}

/** 手势/工具状态机的 9 个状态（PRD 8.5，AC-10 验收项） */
object ToolState {
    const val VIEW = 0              // 浏览：单指旋转 / 双指缩放平移
    const val MEASURE_DISTANCE = 1  // 两点距离
    const val MEASURE_ANGLE = 2     // 三点角度
    const val MEASURE_VOLUME = 3    // 拖拽定义裁剪盒 -> 体积
    const val MEASURE_AREA = 4      // MPR 封闭路径 -> 面积
    const val ROI_EDIT = 5          // ROI 参数编辑
    const val IMPLANT_PLACE = 6     // 种植体定位
    const val ANNOTATE = 7          // 自由标注
    const val NERVE_TRACE = 8       // 神经管逐层描记

    const val COUNT = 9

    fun label(state: Int): String = when (state) {
        VIEW -> "浏览"
        MEASURE_DISTANCE -> "测距"
        MEASURE_ANGLE -> "测角"
        MEASURE_VOLUME -> "体积"
        MEASURE_AREA -> "面积"
        ROI_EDIT -> "ROI 编辑"
        IMPLANT_PLACE -> "种植体"
        ANNOTATE -> "标注"
        NERVE_TRACE -> "神经管"
        else -> "未知状态"
    }
}

object AnnotationType {
    const val TEXT_LABEL = 0        // A-01 文字标签（绑定 3D 点）
    const val LINE = 1              // A-02 线段标注（含长度）
    const val ARROW = 2             // A-03 箭头标注
    const val FREE_CURVE = 3        // A-04 自由曲线（折线）
    const val RING = 4              // A-05 环形标记
    const val MPR_SLICE = 5         // A-06 MPR 截面标注（绑定平面 + 层位）
    const val SCREENSHOT = 6        // A-07 截图标注（图像像素坐标）

    fun label(type: Int): String = when (type) {
        TEXT_LABEL -> "文字标签"
        LINE -> "线段"
        ARROW -> "箭头"
        FREE_CURVE -> "自由曲线"
        RING -> "环形标记"
        MPR_SLICE -> "截面标注"
        SCREENSHOT -> "截图标注"
        else -> "标注"
    }
}

/** MPR 平面：与 :cbctdeal 的 CbctVtkJni.PLANE_* 取值一致（0/1/2） */
object MeasurePlane {
    const val AXIAL = 0
    const val CORONAL = 1
    const val SAGITTAL = 2

    fun label(plane: Int): String = when (plane) {
        AXIAL -> "横断面"
        CORONAL -> "冠状面"
        SAGITTAL -> "矢状面"
        else -> "未知平面"
    }
}

/** 拾取模式（core/MeasurePicker.h 的 PickRequest.mode） */
object PickMode {
    const val SURFACE = 0       // 骨面（HU >= huThreshold）
    const val THROUGH_BONE = 1  // 穿透：第 skip 个"非骨->骨"界面
    const val HU_RANGE = 2      // HU 区间首命中
    const val PLANE = 3         // MPR 平面交点
    const val ROI_SURFACE = 4   // ROI 几何表面
    const val NERVE = 5         // 神经管折线（容差 toleranceMm）
}

/** 叠加图元类型（core/MeasureTypes.h 的 OverlayKind） */
object OverlayKind {
    const val POINT = 0         // 标记点（十字 + 圆）
    const val LINE = 1          // 折线（两点/线段）
    const val POLYLINE = 2      // 开放折线
    const val POLYGON = 3       // 闭合多边形（可选填充）
    const val BOX = 4           // 长方体 12 棱（8 顶点，位掩码顺序）
    const val CAPSULE = 5       // 圆柱轮廓（两个圆环顶点）
    const val TUBE = 6          // 管道折线（神经管）
    const val RING = 7          // 圆环
    const val TEXT = 8          // 纯文字
    const val CIRCLE_PX = 9     // 屏幕空间圆（半径像素）

    /** OK_BOX 的 12 条棱：顶点顺序为 xyz 位掩码（0..7），与 Native 侧一致 */
    val BOX_EDGES = intArrayOf(
        0, 1, 1, 3, 3, 2, 2, 0,   // 底面 4 条
        4, 5, 5, 7, 7, 6, 6, 4,   // 顶面 4 条
        0, 4, 1, 5, 3, 7, 2, 6    // 竖棱 4 条
    )
}

/** 图元归属对象（core/MeasureTypes.h 的 OverlayOwner）：选中高亮的精确匹配键 */
object OverlayOwner {
    const val NONE = 0
    const val ROI = 1
    const val MEASURE = 2
    const val NERVE = 3
    const val IMPLANT = 4
    const val ANNOTATION = 5
}

/** 种植体安全等级（S-02 颜色语义） */
object SafetyLevel {
    const val GREEN = 0
    const val YELLOW = 1
    const val RED = 2

    const val COLOR_GREEN = 0xFF00E676.toInt()
    const val COLOR_YELLOW = 0xFFFFC400.toInt()
    const val COLOR_RED = 0xFFFF1744.toInt()

    fun color(level: Int): Int = when (level) {
        RED -> COLOR_RED
        YELLOW -> COLOR_YELLOW
        else -> COLOR_GREEN
    }

    fun label(level: Int): String = when (level) {
        RED -> "不安全"
        YELLOW -> "临界"
        else -> "安全"
    }
}
