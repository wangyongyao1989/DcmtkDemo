#ifndef DCMTKDEMO_AICORE_H
#define DCMTKDEMO_AICORE_H

#include <string>
#include <vector>

#include "include/AiTypes.h"

/**
 * AI-01 的确定性计算核心（纯 C++，不含 ONNX Runtime、不含 JNI）。
 *
 * 这里做的四件事必须与主机训练/取证脚本逐位可比：
 *   1) 体数据 -> 模型输入特征张量（归一化 + 整数抽稀 + 盒均值带通）；
 *   2) 概率图 -> 二值掩膜（argmax）；
 *   3) 掩膜 -> 连通域实例（26 邻域）+ 每颗牙的体积/质心/长轴/HU 统计；
 *   4) 掩膜 -> MPR 当前层面的正交轮廓 OverlayPrim（世界坐标）。
 * 神经网络本身在 ai/OrtEngine.h，只有那一步依赖 libonnxruntime.so。
 *
 * 主机单测（src/host/）可以直接喂合成数组验证 1)/2)/3)/4)，
 * 因此这一层的任何改动都要先跑
 *   sh cbctmeasure/src/host/build_and_run.sh
 * 再上真机。
 */
/**
 * 神经网络后端的抽象（AI-01 的"可替换部分"）。
 *
 * 真机实现是 ai/OrtEngine（dlopen libonnxruntime.so + OrtApi C 函数表）；
 * 主机单测用一个小假实现返回预置概率图，于是"预处理 + argmax + 连通域 +
 * 轮廓 + 统计"这条占 AI 代码量 90% 的路径可以在 macOS 上跑，
 * 而"只有神经网络那一步依赖 ORT"这个论断才有证据。
 */
struct AiBackend {
    virtual ~AiBackend() {}

    virtual bool ready() const = 0;
    virtual std::string info() const = 0;

    /** 模型输入网格（CDHW 去掉 C）；未就绪时返回 false */
    virtual bool inputDim3(int dim[3]) const = 0;
    /** 输入/输出通道数（与 AiConst::IN_CHANNELS / OUT_CHANNELS 校验用） */
    virtual int inputChannels() const = 0;
    virtual int outputChannels() const = 0;

    /**
     * 一次前向。feat 是 NCDHW 连续 float32；prob 输出为 C-major 连续 float32
     * （prob[c][i][j][k]，长度 outChannels * dim[0]*dim[1]*dim[2]）。
     * 失败返回 false 并把原因写入 err。
     */
    virtual bool infer(const float *feat, long long featCount,
                       std::vector<float> &prob, std::string &err) = 0;
};

namespace AiCore {

    /** 体数据网格 -> 模型网格映射（见 AiGrid 注释） */
    void planGrid(const int appDim[3], const double appSpacing[3],
                  const int modelDim[3], AiGrid &out);

    /**
     * 体数据 -> 6 通道特征张量（NCDHW 的 CDHW 部分，长度
     * IN_CHANNELS * modelDim[0]*modelDim[1]*modelDim[2]，C 序）。
     *
     * 融合"app 分辨率归一化 + 抽稀"为一趟遍历，不在体数据分辨率上开临时缓冲
     * （neck_ct 是 512^3，开一份 float 缓冲就是 278MB，会挤爆渲染线程的堆）；
     * 数值顺序与主机一致：先把每个体素归一化成 float32，再用 double 累加求块均值。
     * 模型张量中抽稀未覆盖的高端区域补 0。
     */
    void buildFeatures(const float *huVolume, const AiGrid &g, std::vector<float> &feat);

    /** 抽稀网格的平均 HU（与特征同一次遍历产出，供 AI ROI 统计与报告使用） */
    void buildReducedHu(const float *huVolume, const AiGrid &g, std::vector<float> &hu);

    /**
     * prob -> label：p(tooth) > threshold 记为牙齿。
     *
     * 默认 threshold = AiConst::TOOTH_THRESHOLD = 0.5，此时与"两类取 argmax"
     * 逐位等价（平局归背景）；但小样本训练的 CNN 输出并不校准，0.5 会给出
     * 高召回低精确的掩膜（主机实测 precision 0.36 / recall 0.82），所以真机
     * 与主机奇偶校验都用训练侧在**验证集**上标定的同一个阈值（由 AiJni 传入，
     * 存进 AiResult::thresholdUsed 供报告引用）。
     */
    void thresholdMask(const float *prob, const AiGrid &g,
                       std::vector<unsigned char> &label, double threshold);

    /**
     * label -> inst（26 邻域连通域，剔除 < MIN_INSTANCE_VOXELS 的散点），
     * 并按体素数降序重编号（1 = 最大块），同时填 AiInstance 的
     * voxels/volumeMm3/centroid/bbox/HU/axis(PCA)。
     */
    void connectedComponents(AiResult &r);

    /** 牙弓归属与沿弓排序（上/下颌按体素 z 分布的最大间隙切分，见实现注释） */
    void archAssign(AiResult &r);

    /**
     * 生成 MPR 当前层面的掩膜轮廓图元。
     * @param plane    MeasurePlane（AXIAL/CORONAL/SAGITTAL）
     * @param position 该平面的体数据层位（体素索引，不是抽稀索引）
     * 图元 owner 一律是 (OW_AI, instanceId)，因此列表选中 -> 图形高亮
     * 与测量项走同一套机制；超过 MAX_CONTOUR_PRIMS 时只保留最大的若干个实例，
     * 避免叠加层帧时间超过 PC-02 的 30FPS 预算。
     */
    void buildOverlay(const AiResult &r, int plane, int position,
                      std::vector<OverlayPrim> &out);

    /** 单层面掩膜：返回 2D 实例编号图 + 两个轴在模型网格中的编号（供反算世界坐标） */
    bool sliceMask(const AiResult &r, int plane, int position,
                   std::vector<short> &mask, int &w, int &h, int &axisA, int &axisB);

    /** 正交轮廓追踪：同色区域的外/内边界闭环（顶点为边界网格交点，非像素中心） */
    void traceContours(const std::vector<short> &mask, int w, int h,
                       std::vector<std::vector<int> > &loops,
                       std::vector<int> &loopLabel);

    /**
     * Dice / 灵敏度 / 特异度（主机与真机共用同一套口径，报告里直接引用）。
     * n 为参与比较的体素数（模型网格体积，含补 0 区时由调用方负责裁剪）。
     */
    double dice(const unsigned char *a, const unsigned char *b, long long n,
                double *sensitivity, double *specificity);

    /** 3x3 实对称矩阵 Jacobi 特征值：返回最大特征值对应的单位特征向量 */
    void pcaPrincipalAxis(const double cov[6], Vec3 &axis);

    /** 一行摘要（UI 状态栏 / 报告用），无结果时返回提示文本 */
    std::string summaryText(const AiResult &r);

    /** prob -> label（用 AiConst::TOOTH_THRESHOLD 的便捷重载，单测与旧调用用） */
    void argmax(const float *prob, const AiGrid &g, std::vector<unsigned char> &label);

    /** 叠加图元数量上限（PC-02 保护） */
    static const int MAX_CONTOUR_PRIMS = 420;
}

#endif // DCMTKDEMO_AICORE_H
