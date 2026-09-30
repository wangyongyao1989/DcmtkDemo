// AI-01 推理编排实现（PRD 5.6.1）。
//
// 本文件只做"取数据 -> 预处理 -> 一次前向 -> 后处理"的顺序编排和耗时/内存取证，
// 不做任何数值计算：数值全部在 core/AiCore.cpp，神经网络在 ai/OrtEngine。
// 这样主机单测换一个假 AiBackend 就能把整条链路（含耗时统计）跑通。

#include "include/AiEngine.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

#define TAG "CbctMeasureAi"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static long long monoNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

namespace AiEngine {

    void runSegment(const VolumeRef &vol, AiBackend &be, double threshold, AiResult &out,
                    std::vector<float> *featOut, std::vector<float> *probOut) {
        out.clear();
        const long long tAll = monoNs();

        if (!vol.valid() || vol.data() == nullptr) {
            out.error = "体数据未绑定";
            LOGE("runSegment: %s", out.error.c_str());
            return;
        }
        if (!be.ready()) {
            out.error = "AI 运行时未就绪（模型或 libonnxruntime.so 未加载）";
            LOGE("runSegment: %s", out.error.c_str());
            return;
        }
        if (!(threshold > 0.0 && threshold < 1.0)) threshold = (double) AiConst::TOOTH_THRESHOLD;
        out.thresholdUsed = threshold;
        out.runtimeInfo = be.info();

        int modelDim[3] = {0, 0, 0};
        if (!be.inputDim3(modelDim) || modelDim[0] <= 0 || modelDim[1] <= 0 || modelDim[2] <= 0) {
            out.error = "模型输入不是 5D(NCDHW) 或维度非法";
            LOGE("runSegment: %s", out.error.c_str());
            return;
        }
        if (be.inputChannels() != AiConst::IN_CHANNELS) {
            char buf[160];
            snprintf(buf, sizeof(buf), "模型输入通道 %d 与特征通道 %d 不一致",
                     be.inputChannels(), AiConst::IN_CHANNELS);
            out.error = buf;
            LOGE("runSegment: %s", out.error.c_str());
            return;
        }
        if (be.outputChannels() != AiConst::OUT_CHANNELS) {
            char buf[160];
            snprintf(buf, sizeof(buf), "模型输出通道 %d 与类别数 %d 不一致",
                     be.outputChannels(), AiConst::OUT_CHANNELS);
            out.error = buf;
            LOGE("runSegment: %s", out.error.c_str());
            return;
        }

        // ---- 网格映射：模型张量是权威，体数据按它分块抽稀 ----
        const int appDim[3] = {vol.width(), vol.height(), vol.depth()};
        const double appSpacing[3] = {vol.spacingX(), vol.spacingY(), vol.spacingZ()};
        AiCore::planGrid(appDim, appSpacing, modelDim, out.grid);
        // 三个维度之间必须有分隔符：连写成 "192192128" 读日志时无法判断是哪三个数
        LOGD("grid: app %dx%dx%d@%.3f/%.3f/%.3f -> model %dx%dx%d factor %dx%dx%d red %dx%dx%d",
             appDim[0], appDim[1], appDim[2], appSpacing[0], appSpacing[1], appSpacing[2],
             modelDim[0], modelDim[1], modelDim[2],
             out.grid.factor[0], out.grid.factor[1], out.grid.factor[2],
             out.grid.redDim[0], out.grid.redDim[1], out.grid.redDim[2]);

        // ---- 1) 预处理 ----
        const long long tPrep = monoNs();
        std::vector<float> feat;
        AiCore::buildFeatures(vol.data(), out.grid, feat);
        AiCore::buildReducedHu(vol.data(), out.grid, out.hu);
        out.prepMs = (double) (monoNs() - tPrep) / 1e6;

        // ---- 2) 前向 ----
        const long long tInfer = monoNs();
        std::vector<float> prob;
        std::string err;
        if (feat.empty() || !be.infer(&feat[0], (long long) feat.size(), prob, err)) {
            out.error = err.empty() ? "推理失败" : err;
            LOGE("runSegment infer: %s", out.error.c_str());
            out.totalMs = (double) (monoNs() - tAll) / 1e6;
            return;
        }
        out.inferMs = (double) (monoNs() - tInfer) / 1e6;
        const long long modelN = out.grid.modelCount();
        if ((long long) prob.size() < (long long) AiConst::OUT_CHANNELS * modelN) {
            char buf[160];
            snprintf(buf, sizeof(buf), "输出张量 %zu 小于期望 %lld",
                     prob.size(), (long long) AiConst::OUT_CHANNELS * modelN);
            out.error = buf;
            LOGE("runSegment: %s", out.error.c_str());
            out.totalMs = (double) (monoNs() - tAll) / 1e6;
            return;
        }

        // ---- 3) 后处理 ----
        const long long tPost = monoNs();
        AiCore::thresholdMask(&prob[0], out.grid, out.label, threshold);
        AiCore::connectedComponents(out);
        AiCore::archAssign(out);
        out.postMs = (double) (monoNs() - tPost) / 1e6;
        if (!out.error.empty()) {
            LOGW("runSegment post warning: %s", out.error.c_str());
        }

        // ---- 4) 取证数据 ----
        out.totalMs = (double) (monoNs() - tAll) / 1e6;
        // allocBytes：本次为结果新增的 Native 堆（feat 是临时量但峰值真实存在，
        // 计入；prob 若被调用方要走也计入）。这是给 PC-05 的"模型+运行时<=80MB"
        // 做减法用的自估量，不是 RSS 实测。
        long long bytes = (long long) feat.size() * 4 + (long long) prob.size() * 4
                + (long long) out.label.size() + (long long) out.inst.size() * 2
                + (long long) out.hu.size() * 4;
        if (!featOut && !probOut) bytes -= (long long) prob.size() * 4;
        out.allocBytes = bytes;
        out.ok = true;

        // 需要奇偶校验中间量时才把它们交给调用方（正常路径不常驻 14MB）
        if (featOut) featOut->swap(feat); else std::vector<float>().swap(feat);
        if (probOut) probOut->swap(prob); else std::vector<float>().swap(prob);

        LOGD("runSegment ok: inst=%zu toothVox=%lld thr=%.4f "
             "prep=%.1fms infer=%.1fms post=%.1fms total=%.1fms alloc=%.1fMB",
             out.instances.size(), out.toothVoxels, threshold,
             out.prepMs, out.inferMs, out.postMs, out.totalMs, out.allocBytes / 1048576.0);
    }

    long long dumpParity(const AiResult &out, const std::vector<float> &feat,
                         const std::vector<float> &prob, const std::string &path,
                         std::string &err) {
        if (!out.hasMask()) {
            err = "没有可用的推理结果";
            return -1;
        }
        std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "无法打开输出文件: " + path;
            return -1;
        }
        const long long head[8] = {
                (long long) feat.size(), (long long) prob.size(), (long long) out.label.size(),
                out.grid.modelDim[0], out.grid.modelDim[1], out.grid.modelDim[2],
                out.grid.redDim[0], out.grid.redDim[1]
        };
        f.write((const char *) head, sizeof(head));
        if (!feat.empty()) f.write((const char *) &feat[0], (long long) feat.size() * 4);
        if (!prob.empty()) f.write((const char *) &prob[0], (long long) prob.size() * 4);
        if (!out.label.empty()) f.write((const char *) &out.label[0], out.label.size());
        // 实例编号也写出来：主机脚本比完 label 之后还要比"剔除散点重编号"的结果，
        // 而重编号顺序（按体素数降序）是最容易在两端不一致的地方。
        if (!out.inst.empty()) f.write((const char *) &out.inst[0], (long long) out.inst.size() * 2);
        f.flush();
        if (!f) {
            err = "写入中断（磁盘满或权限）: " + path;
            return -1;
        }
        const long long total = (long long) sizeof(head)
                + (long long) feat.size() * 4 + (long long) prob.size() * 4
                + (long long) out.label.size() + (long long) out.inst.size() * 2;
        LOGD("dumpParity -> %s (%lld bytes)", path.c_str(), total);
        return total;
    }

}   // namespace AiEngine
