#ifndef DCMTKDEMO_AIENGINE_H
#define DCMTKDEMO_AIENGINE_H

#include <string>
#include <vector>

#include "include/AiCore.h"
#include "include/VolumeRef.h"

/**
 * AI-01 一次推理的编排（PRD 5.6.1 的数据流：HU -> 抽稀 -> 归一化 -> NCDHW
 * -> Session::Run -> ArgMax -> 连通域 -> 统计/叠加）。
 *
 * 本文件是"纯 C++ + 后端抽象"：它不知道 ONNX Runtime 存在，只通过 AiBackend
 * 要一次前向。因此主机单测可以把 AiBackend 换成读预置 .raw 的假实现，
 * 用同一份代码复算真机上的掩膜与统计（AC-08 奇偶校验的实现依据）。
 */
namespace AiEngine {

    /**
     * 跑一次完整分割。任何一步失败都只把原因写进 out.error 并保持
     * ok=false —— 调用方（JNI/UI）不需要处理异常，一期功能不受影响。
     *
     * @param vol        已绑定的体数据（app 分辨率，[z][y][x] 布局见 VolumeRef.h）
     * @param be         神经网络后端
     * @param threshold  牙类判定阈值（<=0 或 >1 时退回 AiConst::TOOTH_THRESHOLD）
     * @param out        结果（内部先 clear()）
     */
    void runSegment(const VolumeRef &vol, AiBackend &be, double threshold, AiResult &out,
                    std::vector<float> *featOut = nullptr,
                    std::vector<float> *probOut = nullptr);

    /**
     * 把一次推理的中间量导出成主机侧可比对的原始字节（AC-08 取证用）。
     * feat / prob / label 三段依次写入同一个文件（little-endian、C 连续），
     * 前面再加一段 8 个 int64 的头（feat 元素数、prob 元素数、label 字节数、
     * modelDim xyz、redDim xyz），主机脚本据此判断形状是否一致。
     * 只有 runSegment 显式要了 featOut/probOut 时才有得可dump（省 14MB 常驻）。
     *
     * @return 写入的总字节数；-1 = 失败（原因写入 err）
     */
    long long dumpParity(const AiResult &out, const std::vector<float> &feat,
                         const std::vector<float> &prob, const std::string &path,
                         std::string &err);

}

#endif // DCMTKDEMO_AIENGINE_H
