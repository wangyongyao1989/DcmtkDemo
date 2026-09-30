#ifndef DCMTKDEMO_ORTENGINE_H
#define DCMTKDEMO_ORTENGINE_H

#include <string>
#include <vector>

#include "include/AiCore.h"     // AiBackend

/**
 * ONNX Runtime 的 C++ 推理封装（PRD 8.6「AI 层」，PRD 5.6 的 Session::Run()）。
 *
 * ============================ 为什么是 dlopen ============================
 * PRD 写的是 `implementation 'com.microsoft.onnxruntime:onnxruntime-android'`
 * 之后在 C++ 侧经 Prefab 使用。实测 1.17.0 的 AAR **没有 prefab 模块**
 * （既没有 prefab/ 也没有 prefab-metadata.json），所以 CMake 里
 * `find_package(onnxruntime CONFIG)` 这条路走不通。AAR 里有的是：
 *   headers 目录（8 个头文件，已 vendor 到 cpp/third_party/onnxruntime/include）
 *   jni/arm64-v8a/libonnxruntime.so（16,033,728 B）
 *   classes.jar（102 KB，ai.onnxruntime 的 Java API，本项目不用）
 *
 * 于是本类用 dlopen 打开"随 AAR 打进 APK 并被系统解到 nativeLibraryDir"的那个
 * .so，只取它唯一需要静态链接的导出符号 OrtGetApiBase，之后的全部调用都走
 * OrtApi 里的 C 函数表。这样做的四个好处：
 *   1) libcbct_measure.so 没有 libonnxruntime.so 的 DT_NEEDED —— 没装/没解出
 *      该 so 时模块照样能加载，AI 功能降级为"未就绪"，一期功能不受影响；
 *   2) 主机侧单测（src/host/）完全不碰 ORT，也不需要 16MB 的 so；
 *   3) 16MB 的二进制不进 git（走 Gradle 依赖，符合仓库里 VTK/DCMTK 的做法）；
 *   4) 换 ORT 版本只改 Gradle 依赖，不动 C++。
 *
 * 线程约定：Env/Session 由 ONNX Runtime 保证可并发 Run，但本类**不加锁**：
 * 与 :cbctdeal 一致，一个会话实例只被 JNI 层的同一个 MeasureSession 持有，
 * Kotlin 侧保证 runSegment 串行（推理在 Dispatchers.Default 的单线程语义下调用）。
 */
class OrtEngine : public AiBackend {
public:
    OrtEngine();
    ~OrtEngine() override;

    OrtEngine(const OrtEngine &) = delete;
    OrtEngine &operator=(const OrtEngine &) = delete;

    /**
     * 加载运行时并建会话。
     * @param soPath     libonnxruntime.so 的**候选列表**，';' 分隔（Kotlin 侧
     *                   ortSoCandidates() 给出：nativeLibraryDir 磁盘形态、
     *                   "<apk>!/lib/<abi>/..." APK 原位映射形态）。本函数按顺序
     *                   试 dlopen，并把真正成功的那条记进 usedSo()。
     * @param modelPath  .onnx 文件的绝对路径（assets/models 拷到 filesDir 后）
     * @param threads    intra-op 线程数（<=0 用默认 4）
     * 失败时 ready() 为 false，err 带可读原因（含每个候选的 dlerror 文本）。
     */
    bool load(const std::string &soPath, const std::string &modelPath,
              int threads, std::string &err);

    void unload();

    bool ready() const override { return session_ != nullptr; }
    std::string info() const override;

    /** 实际 dlopen 成功的那条路径（报告里"运行时怎么加载的"直接引用它） */
    const std::string &usedSo() const { return usedSo_; }

    /** 模型输入张量的 CDHW 部分（跳掉 batch 轴）；不是 5D 时返回 false */
    bool inputDim3(int dim[3]) const override;
    /** 输入通道数（校验与 AiConst::IN_CHANNELS 是否一致） */
    int inputChannels() const override { return inChannels_; }
    /** 输出通道数（校验与 AiConst::OUT_CHANNELS 是否一致） */
    int outputChannels() const override { return outChannels_; }
    const std::string &inputName() const { return inName_; }
    const std::string &outputName() const { return outName_; }

    /**
     * 一次前向：feat 长度必须等于 inputChannels()*prod(inputDim3)，
     * prob 输出长度由本函数 assign 为 outputChannels()*prod(dims)。
     */
    bool infer(const float *feat, long long featCount,
               std::vector<float> &prob, std::string &err) override;

private:
    struct Impl;          // ORT 的不透明类型全部藏在这里，头文件不 include ORT
    Impl *impl_;

    std::string inName_, outName_;
    std::string usedSo_;      // dlopen 成功的候选（可能是 APK 原位映射形态）
    int inDim_[3];
    int inChannels_, outChannels_;
    void *session_;       // OrtSession*
    bool soLoaded_;
};

#endif // DCMTKDEMO_ORTENGINE_H
