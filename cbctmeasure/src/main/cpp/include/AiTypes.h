#ifndef DCMTKDEMO_AITYPES_H
#define DCMTKDEMO_AITYPES_H

#include <string>
#include <vector>

#include "include/MeasureTypes.h"

/**
 * AI 辅助分析（PRD 5.6 Phase 2）的数据类型与"主机—真机同源常量"。
 *
 * 本文件里的每一个数字都不是拍脑袋定的：它们与主机侧数据流水线
 * （训练/取证脚本 prep.py）逐条对齐。任何一侧改动都必须同步另一侧，
 * 否则真机推理与主机参考实现就不再是同一个函数，AC-08 的
 * "真机结果 == 主机结果"奇偶校验会静默失效（数值仍能跑，只是分割错位）。
 *
 * 为什么 AI 计算也要放进 core/（纯 C++、无 JNI、无 ORT）：
 *   1) 特征构造 / argmax / 连通域 / 轮廓提取全部是可离线单测的确定性代码，
 *      与 ONNX Runtime 无关；把这三件事和"跑一次神经网络"分开，
 *      才能在主机上用 364 条断言覆盖它们（见 src/host/test_main.cpp）；
 *   2) 真机上只有"跑神经网络"这一步依赖 libonnxruntime.so，
 *      出问题时可以立刻定位是"预处理不一致"还是"推理引擎不一致"。
 *
 * 坐标/轴序约定（与 include/VolumeRef.h、:cbctdeal CbctSeriesParser 严格一致）：
 *   体数据内存布局 [z][y][x]，x=Columns、y=Rows、z=切片；
 *   world(mm) = index * spacing，原点为首体素中心。
 *   ONNX 输入张量为 NCDHW = [1, C, x, y, z]，即空间最内层是 z 轴，
 *   与主机 numpy 数组 (C, axis0=x, axis1=y, axis2=z) 的 C 序展开一致。
 *   —— 这条约定决定了"真机的 x/y 是否就是训练时的 x/y"，
 *      牙科测试序列的 DICOM 写入侧必须保证体数据轴序与主机数组轴序一致。
 */

/** AI-01 输出的类别（argmax 的类别下标） */
enum AiClass {
    AI_CLS_BACKGROUND = 0,
    AI_CLS_TOOTH = 1,
    AI_CLS_COUNT = 2,
};

/**
 * 主机侧 prep.py 的同源常量（见模块 README「AI 层：主机—真机契约」一节）。
 * 数值含义：HU 先截断到 [-1024, 4095]，再线性映射到 [-0.5, +0.5]；
 * 在 1.2 mm 抽稀网格上做 4 个半径（1/2/4/8 体素）的可分离盒均值；
 * 6 通道输入 = 归一化体 + 4 个盒均值 + (r=4 盒均值 - r=8 盒均值) 的带通项。
 */
namespace AiConst {
    // ---- 模型 I/O ----
    static const int IN_CHANNELS = 6;
    static const int OUT_CHANNELS = AI_CLS_COUNT;      // 2
    static const char *const IN_NAME = "feat";
    static const char *const OUT_NAME = "prob";

    // ---- HU 归一化（与 prep.py HU_CLIP_* / HU_NORM_* 一致）----
    static const double HU_CLIP_LO = -1024.0;
    static const double HU_CLIP_HI = 4095.0;
    static const double HU_NORM_BIAS = 1024.0;
    static const double HU_NORM_SCALE = 5120.0;
    static const double HU_NORM_SHIFT = 0.5;

    // ---- 特征盒半径（抽稀网格体素为单位，盒宽 2r+1），c1..c4 ----
    static const int BOX_RADII[4] = {1, 2, 4, 8};
    /** c5 = boxes[BANDPASS_A] - boxes[BANDPASS_B]（半径 4 减半径 8） */
    static const int BANDPASS_A = 2;   // 下标：BOX_RADII[2] == 4
    static const int BANDPASS_B = 3;   // 下标：BOX_RADII[3] == 8

    /** 边界语义：边缘复制（越界索引夹到 [0, n-1]），且除数恒为 2r+1 */
    static const bool BOX_BORDER_REPLICATE = true;

    /** argmax 判定：牙齿类概率严格大于背景类概率才算牙齿（平局归背景） */
    static const float TOOTH_THRESHOLD = 0.5f;

    /** 连通域最小体素数：小于该值视为噪声散点，不进掩膜叠加与自动测量 */
    static const int MIN_INSTANCE_VOXELS = 8;

    /** aiAutoMeasure 写进 MeasureRecord.note 的标记：只用于识别"这套行是我生成的"，
     *  重跑推理时按它清理旧行，手工挂在同一掩膜 ROI 上的测量不会被误删。 */
    static const char *const AUTO_NOTE = "AI-01 自动分割";

    /** 叠加层调色板（0xAARRGGBB）：按连通域序号循环取色，相邻牙不同色。
     *  描边用这里的实色；半透明填充由 MeasureOverlayView.drawFilled() 统一
     *  压到 alpha 0.18（与 M-05 截面多边形一致），所以这里不要再带低透明度。 */
    static const unsigned int PALETTE[12] = {
            0xFF00E5FFU, 0xFFFFC400U, 0xFF69F0AEU, 0xFFFF80ABU,
            0xFFB388FFU, 0xFFFFAB40U, 0xFF84FFFFU, 0xFFF4FF81U,
            0xFF8C9EFFU, 0xFFFF6E40U, 0xFFA7FFEBU, 0xFFEEFF41U,
    };
    static const int PALETTE_COUNT = 12;
}

/**
 * 体数据网格 -> 模型网格的映射（一次算好，反向映射轮廓坐标时复用）。
 *
 * 抽稀倍率按轴独立取 factor = ceil(appDim / modelDim)，抽稀后尺寸
 * redDim = ceil(appDim / factor) 必然 <= modelDim，其余部分在模型输入张量的
 * 高端补 0。牙科测试序列（192x128... 见 README）恰好是 2 的整除关系，
 * 因此 factor=2、redDim=modelDim、补 0 为 0，与主机 prep.py 的
 * "sum over 2x2x2 block / 8" 逐位等价；非整除关系（例如既有 neck_ct）只保证
 * "跑得通"，不主张与主机参考一致。
 */
struct AiGrid {
    int appDim[3] = {0, 0, 0};         // 体数据体素数 (x, y, z)
    double appSpacing[3] = {1, 1, 1};  // 体数据间距 mm
    int factor[3] = {1, 1, 1};         // 整数抽稀倍率
    int redDim[3] = {0, 0, 0};         // 抽稀后实际尺寸（<= modelDim）
    int modelDim[3] = {0, 0, 0};       // 模型输入空间尺寸（ONNX 声明值）
    double spacing[3] = {1, 1, 1};     // appSpacing * factor

    long long redCount() const {
        return (long long) redDim[0] * redDim[1] * redDim[2];
    }

    long long modelCount() const {
        return (long long) modelDim[0] * modelDim[1] * modelDim[2];
    }

    /** 抽稀网格线性索引（C 序：x 最外、z 最内），越界返回 -1 */
    long long redIndex(int i, int j, int k) const {
        if (i < 0 || j < 0 || k < 0 || i >= redDim[0] || j >= redDim[1] || k >= redDim[2])
            return -1;
        return ((long long) i * redDim[1] + j) * redDim[2] + k;
    }

    /** 模型张量内的线性索引（含补 0 区域；越界返回 -1） */
    long long modelIndex(int i, int j, int k) const {
        if (i < 0 || j < 0 || k < 0 || i >= modelDim[0] || j >= modelDim[1] || k >= modelDim[2])
            return -1;
        return ((long long) i * modelDim[1] + j) * modelDim[2] + k;
    }

    /** 抽稀体素 (i,j,k) 覆盖的体数据体素区间下标 */
    int appStart(int axis, int idx) const { return idx * factor[axis]; }

    /** 抽稀体素的球心世界坐标（mm） */
    Vec3 redToWorld(int i, int j, int k) const {
        return Vec3(((double) i + 0.5) * spacing[0],
                    ((double) j + 0.5) * spacing[1],
                    ((double) k + 0.5) * spacing[2]);
    }

    /** 抽稀体素边界的世界坐标：模型网格上 i 与 i+1 的分界面在体数据上正好是整 voxel 边界 */
    double redBoundary(int axis, int i) const { return (double) i * spacing[axis]; }
};

/** 单个分割实例（连通域）：AI-01 的"每颗牙"，也是 AI-03 的输入 */
struct AiInstance {
    int id = 0;                    // 1..N（按体素数降序重编号后的序号）
    long long voxels = 0;
    double volumeMm3 = 0.0;
    Vec3 centroid;                 // 世界 mm（体素球心加权平均）
    Vec3 bboxMin, bboxMax;         // 世界 mm
    double meanHu = 0.0, minHu = 0.0, maxHu = 0.0, sdHu = 0.0;
    /** 牙齿长轴：对掩膜体素做 PCA，取最大方差方向（AI-03 用它推植入轴向） */
    Vec3 axis;
    /** 牙弓方位角（度）：在轴位面上相对牙弓中心的极角，按牙弓顺序排列用 */
    double archAngleDeg = 0.0;
    /** 牙弓归属：0 = 上颌，1 = 下颌，-1 = 无法判定（掩膜过少） */
    int arch = -1;
    int toothCountHint = 0;        // 该弓内的序号（1 起），仅用于命名
    /**
     * 冠/根方向的世界 z（咬合平面判定，AI-03 的植入深度用）。
     * 规则：包围盒两端中离咬合平面更近的一端为冠（crown），另一端为根（root）。
     * 连通域阶段为 0（archAssign 之前没有咬合平面信息）。
     */
    double crownZ = 0.0, rootZ = 0.0;
    int roiId = 0;                 // 落库为 ROI 后回填（0 = 未落库）
    int measureId = 0;             // 落库为 M-04 体积测量后回填
    bool merged = false;           // 连通域融合（邻牙粘连）标记，报告里要说明
};

/**
 * 一次 AI-01 推理的完整结果。
 *
 * label 与 inst 的分工（不能只看一个）：
 *   label = argmax(prob) 的原样输出，是"模型说了什么"的证据，
 *           主机/真机奇偶校验（AC-08）比的就是它 + prob；
 *   inst  = 在 label 上做 26 邻域连通域并剔除散点后的实例编号，
 *           叠加层绘制、体积/HU 统计、AI-03 都只看它。
 *   两者不一致是有意为之，报告里必须写清阈值（MIN_INSTANCE_VOXELS）。
 */
struct AiResult {
    bool ok = false;
    std::string error;

    AiGrid grid;
    std::vector<unsigned char> label;   // modelDim 体积，值 0/1
    std::vector<short> inst;            // 同上，0 = 背景/散点，1..N = 实例
    std::vector<float> hu;              // redDim 体积，抽稀网格平均 HU（统计用）
    std::vector<AiInstance> instances;

    // ---- PC-05 取证：延迟与内存 ----
    double prepMs = 0.0;
    double inferMs = 0.0;
    double postMs = 0.0;
    double totalMs = 0.0;
    long long allocBytes = 0;           // 本次推理新增的 Native 堆字节（估算）

    std::string modelName;
    std::string runtimeInfo;            // ORT API 版本 / 线程数 / provider
    /** 实际生效的判定阈值（报告与奇偶校验必须引用它，否则 label 不可复现） */
    double thresholdUsed = AiConst::TOOTH_THRESHOLD;
    long long toothVoxels = 0;          // label==1 的体素数
    bool overlayVisible = true;         // 叠加层开关（上层可关，不影响数据）

    void clear() {
        label.clear();
        inst.clear();
        hu.clear();
        instances.clear();
        toothVoxels = 0;
        ok = false;
        error.clear();
        prepMs = inferMs = postMs = totalMs = 0.0;
        allocBytes = 0;
    }

    bool hasMask() const { return !label.empty() && !inst.empty(); }
};

#endif // DCMTKDEMO_AITYPES_H
