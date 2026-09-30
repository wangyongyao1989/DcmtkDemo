// AI-01 确定性计算核心实现（PRD 5.6.1 / AC-08）。
//
// 本文件不含 ONNX Runtime / JNI / VTK：主机侧可用同一份源码编译单测
// （sh cbctmeasure/src/host/build_and_run.sh），真机与主机因此共享
// "预处理 + 后处理"的全部数值路径；两边不一致时，唯一可能的差异来源
// 就是神经网络本身，这正是 AC-08 奇偶校验要证明的东西。

#include "include/AiCore.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>

#define TAG "CbctMeasureAi"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace AiCore {

    // =========================================================================
    // 网格映射
    // =========================================================================

    void planGrid(const int appDim[3], const double appSpacing[3],
                  const int modelDim[3], AiGrid &out) {
        out = AiGrid();
        for (int a = 0; a < 3; ++a) {
            out.appDim[a] = appDim[a];
            out.appSpacing[a] = appSpacing[a];
            out.modelDim[a] = modelDim[a];
            int md = modelDim[a] > 0 ? modelDim[a] : 1;
            // ceil(appDim / modelDim)：抽稀倍率至少为 1
            int f = (appDim[a] + md - 1) / md;
            if (f < 1) f = 1;
            out.factor[a] = f;
            int rd = (appDim[a] + f - 1) / f;      // ceil：末块允许不足 f 个体素
            if (rd > md) rd = md;                  // 防御（f 取 ceil 后必然 <= md）
            out.redDim[a] = rd;
            out.spacing[a] = appSpacing[a] * (double) f;
        }
    }

    // =========================================================================
    // 特征构造
    // =========================================================================

    /** 与主机 normalize_hu() 一致：先 clip 再线性映射，结果落在 [-0.5, +0.5] */
    static inline float normalizeHu(double hu) {
        double h = hu;
        if (h < AiConst::HU_CLIP_LO) h = AiConst::HU_CLIP_LO;
        if (h > AiConst::HU_CLIP_HI) h = AiConst::HU_CLIP_HI;
        return (float) ((h + AiConst::HU_NORM_BIAS) / AiConst::HU_NORM_SCALE
                        - AiConst::HU_NORM_SHIFT);
    }

    /**
     * app 分辨率 -> 抽稀网格的块均值（一趟遍历，不开 app 分辨率临时缓冲）。
     *
     * 数值顺序与主机 decimate2_boxavg() 对齐：每个体素先归一化成 float32，
     * 块内用 double 累加，除以"实际参与累加的体素数"。当 appDim 能被 factor
     * 整除时（牙科测试序列 factor=2、块大小恒为 8）除数与主机完全一致；
     * 非整除时末块体素数少一些，这是通用路径与固定尺寸参考实现唯一的区别，
     * 报告里只主张整除用例的奇偶校验。
     */
    static void decimateNormalized(const float *vol, const int volDim[3], const AiGrid &g,
                                   std::vector<float> &red /* redDim0*redDim1*redDim2 */) {
        const int wx = volDim[0], wy = volDim[1], wz = volDim[2];
        const long long sliceSize = (long long) wx * wy;
        red.assign((size_t) g.redCount(), 0.0f);
        const int fx = g.factor[0], fy = g.factor[1], fz = g.factor[2];
        for (int i = 0; i < g.redDim[0]; ++i) {
            const int x0 = i * fx;
            int x1 = x0 + fx;
            if (x1 > wx) x1 = wx;
            for (int j = 0; j < g.redDim[1]; ++j) {
                const int y0 = j * fy;
                int y1 = y0 + fy;
                if (y1 > wy) y1 = wy;
                for (int k = 0; k < g.redDim[2]; ++k) {
                    const int z0 = k * fz;
                    int z1 = z0 + fz;
                    if (z1 > wz) z1 = wz;
                    double acc = 0.0;
                    long long cnt = 0;
                    for (int z = z0; z < z1; ++z) {
                        const float *plane = vol + (long long) z * sliceSize;
                        for (int y = y0; y < y1; ++y) {
                            const float *row = plane + (long long) y * wx;
                            for (int x = x0; x < x1; ++x) {
                                acc += (double) normalizeHu((double) row[x]);
                                ++cnt;
                            }
                        }
                    }
                    red[(size_t) g.redIndex(i, j, k)] = cnt > 0 ? (float) (acc / (double) cnt) : 0.0f;
                }
            }
        }
    }

    void buildReducedHu(const float *vol, const AiGrid &g, std::vector<float> &hu) {
        const int wx = g.appDim[0], wy = g.appDim[1], wz = g.appDim[2];
        const long long sliceSize = (long long) wx * wy;
        hu.assign((size_t) g.redCount(), 0.0f);
        const int fx = g.factor[0], fy = g.factor[1], fz = g.factor[2];
        for (int i = 0; i < g.redDim[0]; ++i) {
            const int x0 = i * fx;
            int x1 = x0 + fx;
            if (x1 > wx) x1 = wx;
            for (int j = 0; j < g.redDim[1]; ++j) {
                const int y0 = j * fy;
                int y1 = y0 + fy;
                if (y1 > wy) y1 = wy;
                for (int k = 0; k < g.redDim[2]; ++k) {
                    const int z0 = k * fz;
                    int z1 = z0 + fz;
                    if (z1 > wz) z1 = wz;
                    double acc = 0.0;
                    long long cnt = 0;
                    for (int z = z0; z < z1; ++z) {
                        const float *plane = vol + (long long) z * sliceSize;
                        for (int y = y0; y < y1; ++y) {
                            const float *row = plane + (long long) y * wx;
                            for (int x = x0; x < x1; ++x) {
                                acc += (double) row[x];
                                ++cnt;
                            }
                        }
                    }
                    hu[(size_t) g.redIndex(i, j, k)] = cnt > 0 ? (float) (acc / (double) cnt) : 0.0f;
                }
            }
        }
    }

    /**
     * 可分离盒均值（半径 r，盒宽 2r+1，边缘复制、除数恒为 2r+1）。
     *
     * 与主机 box_mean_replicate() 的差别只有求和方式：主机用 float64 前缀和相减，
     * 这里对每个输出体素直接累加 2r+1 项。两者在 float64 域舍入差约 1e-16，
     * 输出转 float32（有效位 1e-7）后绝大多数体素逐位相同；奇偶校验因此按
     * "最大绝对差 + 标签一致率"判定，而不是逐位 memcmp。
     */
    static void boxMean(const std::vector<float> &in, const AiGrid &g, int r,
                        std::vector<float> &out, std::vector<double> &tmpA,
                        std::vector<double> &tmpB) {
        const int n0 = g.redDim[0], n1 = g.redDim[1], n2 = g.redDim[2];
        const size_t n = (size_t) n0 * n1 * n2;
        const double w = (double) (2 * r + 1);
        tmpA.assign(n, 0.0);
        tmpB.assign(n, 0.0);
        // 三个维度的线性步长（z 是最内层，步长 1，所以直接写 k）
        const int s0 = n1 * n2, s1 = n2;
        for (int j = 0; j < n1; ++j) {
            for (int k = 0; k < n2; ++k) {
                for (int i = 0; i < n0; ++i) {
                    double acc = 0.0;
                    for (int d = -r; d <= r; ++d) {
                        int q = i + d;
                        if (q < 0) q = 0;
                        if (q >= n0) q = n0 - 1;
                        acc += (double) in[(size_t) (q * s0 + j * s1 + k)];
                    }
                    tmpA[(size_t) (i * s0 + j * s1 + k)] = acc / w;
                }
            }
        }
        // axis 1（y）
        for (int i = 0; i < n0; ++i) {
            for (int k = 0; k < n2; ++k) {
                for (int j = 0; j < n1; ++j) {
                    double acc = 0.0;
                    for (int d = -r; d <= r; ++d) {
                        int q = j + d;
                        if (q < 0) q = 0;
                        if (q >= n1) q = n1 - 1;
                        acc += tmpA[(size_t) (i * s0 + q * s1 + k)];
                    }
                    tmpB[(size_t) (i * s0 + j * s1 + k)] = acc / w;
                }
            }
        }
        // axis 2（z）
        out.assign(n, 0.0f);
        for (int i = 0; i < n0; ++i) {
            for (int j = 0; j < n1; ++j) {
                for (int k = 0; k < n2; ++k) {
                    double acc = 0.0;
                    for (int d = -r; d <= r; ++d) {
                        int q = k + d;
                        if (q < 0) q = 0;
                        if (q >= n2) q = n2 - 1;
                        acc += tmpB[(size_t) (i * s0 + j * s1 + q)];
                    }
                    out[(size_t) (i * s0 + j * s1 + k)] = (float) (acc / w);
                }
            }
        }
    }

    void buildFeatures(const float *vol, const AiGrid &g, std::vector<float> &feat) {
        const size_t redN = (size_t) g.redCount();
        const size_t modelN = (size_t) g.modelCount();
        std::vector<float> c0;
        decimateNormalized(vol, g.appDim, g, c0);

        std::vector<float> boxes[4];
        std::vector<double> tA, tB;
        for (int b = 0; b < 4; ++b) {
            boxMean(c0, g, AiConst::BOX_RADII[b], boxes[b], tA, tB);
        }
        const float *chan[AiConst::IN_CHANNELS];
        chan[0] = c0.empty() ? nullptr : &c0[0];
        for (int b = 0; b < 4; ++b) chan[b + 1] = boxes[b].empty() ? nullptr : &boxes[b][0];
        // c5 = box(r=4) - box(r=8)：中程与长程对比之差（单牙尺度 vs 牙弓尺度）
        std::vector<float> band(redN, 0.0f);
        for (size_t t = 0; t < redN; ++t) {
            band[t] = boxes[AiConst::BANDPASS_A][t] - boxes[AiConst::BANDPASS_B][t];
        }
        chan[5] = band.empty() ? nullptr : &band[0];

        // 组装成 NCDHW（此处 N=1）：布局 ((c*dimX + i)*dimY + j)*dimZ + k，
        // 抽稀未覆盖的模型网格高端区域补 0（vector 构造已清零）。
        feat.assign(AiConst::IN_CHANNELS * modelN, 0.0f);
        const long long planeN = (long long) g.modelDim[1] * g.modelDim[2];
        for (int c = 0; c < AiConst::IN_CHANNELS; ++c) {
            float *dst = &feat[(size_t) c * modelN];
            for (int i = 0; i < g.redDim[0]; ++i) {
                for (int j = 0; j < g.redDim[1]; ++j) {
                    const float *src = chan[c] + ((long long) i * g.redDim[1] + j) * g.redDim[2];
                    // 目标行内偏移：模型网格的 y 与 redDim 的 y 同起点，只差行宽
                    float *row = dst + ((long long) i * g.modelDim[1] + j) * g.modelDim[2];
                    memcpy(row, src, (size_t) g.redDim[2] * sizeof(float));
                }
            }
            (void) planeN;
        }
    }

    // =========================================================================
    // argmax
    // =========================================================================

    void thresholdMask(const float *prob, const AiGrid &g,
                       std::vector<unsigned char> &label, double threshold) {
        const size_t n = (size_t) g.modelCount();
        label.assign(n, 0);
        if (!prob) return;
        const float *tooth = prob + n;      // 通道主序：c0=背景, c1=牙齿
        long long hits = 0;
        for (size_t t = 0; t < n; ++t) {
            // 与主机 parity 脚本同一判据：prob[tooth] > threshold（严格大于，
            // 恰好等于阈值的体素归背景）。比较在 float 升到 double 后做，
            // 因为 threshold 是 double，若在 float 侧比较会在阈值附近抖动。
            if ((double) tooth[t] > threshold) {
                label[t] = 1;
                ++hits;
            }
        }
        LOGD("thresholdMask: thr=%.4f tooth voxels=%lld / %zu", threshold, hits, n);
    }

    void argmax(const float *prob, const AiGrid &g, std::vector<unsigned char> &label) {
        // 0.5 时与"两类取 argmax"等价（Softmax 后两类概率和为 1），
        // 保留这个名字是因为单测里读起来更直观。
        thresholdMask(prob, g, label, (double) AiConst::TOOTH_THRESHOLD);
    }

    // =========================================================================
    // 连通域 + 实例统计
    // =========================================================================

    /** 26 邻域偏移表（预先算好，避免三重循环里反复做减法） */
    struct Neighbor26 {
        int d[26][3];

        Neighbor26() {
            int t = 0;
            for (int a = -1; a <= 1; ++a) {
                for (int b = -1; b <= 1; ++b) {
                    for (int c = -1; c <= 1; ++c) {
                        if (a == 0 && b == 0 && c == 0) continue;
                        d[t][0] = a;
                        d[t][1] = b;
                        d[t][2] = c;
                        ++t;
                    }
                }
            }
        }
    };

    void connectedComponents(AiResult &r) {
        const AiGrid &g = r.grid;
        const size_t n = (size_t) g.modelCount();
        r.inst.assign(n, 0);
        r.instances.clear();
        if (r.label.size() != n) {
            r.error = "label/model size mismatch";
            return;
        }
        static const Neighbor26 nb;

        struct Raw {
            long long voxels;
            double sx, sy, sz;
            double sxx, syy, szz, sxy, sxz, syz;   // 二阶矩：长轴 PCA 用（平行轴定理）
            double mMin[3], mMax[3];
            double huSum, huMin, huMax, huSqSum;
        };
        std::vector<int> stackI, stackJ, stackK;
        std::vector<Raw> raws;
        raws.push_back(Raw());   // 下标 0 占位（标签从 1 开始）

        std::vector<int> comps;   // 临时记录每个候选连通域的线性索引
        for (size_t t = 0; t < n; ++t) {
            if (r.label[t] == 0 || r.inst[t] != 0) continue;
            const int i0 = (int) (t / ((long long) g.modelDim[1] * g.modelDim[2]));
            const int rem = (int) (t - (long long) i0 * g.modelDim[1] * g.modelDim[2]);
            const int j0 = rem / g.modelDim[2];
            const int k0 = rem - j0 * g.modelDim[2];
            comps.clear();
            stackI.clear();
            stackJ.clear();
            stackK.clear();
            stackI.push_back(i0);
            stackJ.push_back(j0);
            stackK.push_back(k0);
            r.inst[t] = (short) raws.size();
            long long count = 0;
            while (!stackI.empty()) {
                const int i = stackI.back();
                const int j = stackJ.back();
                const int k = stackK.back();
                stackI.pop_back();
                stackJ.pop_back();
                stackK.pop_back();
                comps.push_back(i * g.modelDim[1] * g.modelDim[2] + j * g.modelDim[2] + k);
                ++count;
                for (int q = 0; q < 26; ++q) {
                    const int a = i + nb.d[q][0], b = j + nb.d[q][1], c = k + nb.d[q][2];
                    if (a < 0 || b < 0 || c < 0 || a >= g.modelDim[0] || b >= g.modelDim[1]
                        || c >= g.modelDim[2]) continue;
                    const size_t s = (size_t) ((long long) a * g.modelDim[1] + b) * g.modelDim[2] + c;
                    if (r.label[s] == 0 || r.inst[s] != 0) continue;
                    r.inst[s] = (short) raws.size();
                    stackI.push_back(a);
                    stackJ.push_back(b);
                    stackK.push_back(c);
                }
            }
            if (count < AiConst::MIN_INSTANCE_VOXELS) {
                // 散点剔除：把临时编号擦回 0，不进实例表（但 label 保持原样，
                // 因为 label 是"模型原始输出"的取证依据）
                for (size_t q = 0; q < comps.size(); ++q) r.inst[comps[q]] = 0;
                continue;
            }
            Raw raw;
            raw.voxels = 0;
            raw.sx = raw.sy = raw.sz = 0.0;
            raw.sxx = raw.syy = raw.szz = raw.sxy = raw.sxz = raw.syz = 0.0;
            raw.huSum = raw.huSqSum = 0.0;
            raw.huMin = std::numeric_limits<double>::max();
            raw.huMax = -std::numeric_limits<double>::max();
            for (int a = 0; a < 3; ++a) {
                raw.mMin[a] = std::numeric_limits<double>::max();
                raw.mMax[a] = -std::numeric_limits<double>::max();
            }
            // 体素统计（面积/质心/包围盒用抽稀网格索引，HU 用同索引的 hu 数组）
            // 注意：comps 存的是 modelDim 线性索引，与 redDim 一致时可直接换算；
            // 补 0 区不会有牙齿标签，所以映射到 hu 一定落在有效范围内。
            for (size_t q = 0; q < comps.size(); ++q) {
                const int idx = comps[q];
                const int i = idx / (g.modelDim[1] * g.modelDim[2]);
                const int rem = idx - i * g.modelDim[1] * g.modelDim[2];
                const int j = rem / g.modelDim[2];
                const int k = rem - j * g.modelDim[2];
                if (i >= g.redDim[0] || j >= g.redDim[1] || k >= g.redDim[2]) continue;
                const Vec3 c = g.redToWorld(i, j, k);
                raw.sx += c.x;
                raw.sy += c.y;
                raw.sz += c.z;
                // 二阶矩就地累加：长轴 PCA 用平行轴定理从原点矩换算，
                // 不再为每颗牙重扫整块掩膜（32 实例 x 59 万体素 = 1900 万次除法，
                // 真机上会让 postMs 比推理本身还慢）
                raw.sxx += c.x * c.x;
                raw.syy += c.y * c.y;
                raw.szz += c.z * c.z;
                raw.sxy += c.x * c.y;
                raw.sxz += c.x * c.z;
                raw.syz += c.y * c.z;
                double bb[3] = {c.x - g.spacing[0] * 0.5, c.y - g.spacing[1] * 0.5,
                                c.z - g.spacing[2] * 0.5};
                double tt[3] = {c.x + g.spacing[0] * 0.5, c.y + g.spacing[1] * 0.5,
                                c.z + g.spacing[2] * 0.5};
                for (int a = 0; a < 3; ++a) {
                    if (bb[a] < raw.mMin[a]) raw.mMin[a] = bb[a];
                    if (tt[a] > raw.mMax[a]) raw.mMax[a] = tt[a];
                }
                const double v = (double) r.hu[(size_t) g.redIndex(i, j, k)];
                raw.huSum += v;
                raw.huSqSum += v * v;
                if (v < raw.huMin) raw.huMin = v;
                if (v > raw.huMax) raw.huMax = v;
                ++raw.voxels;
            }
            raws.push_back(raw);
        }

        // 重新编号：按体素数降序（1 = 最大块），便于"上颌/下颌 + 由前向后"的稳定命名
        std::vector<int> order;
        for (size_t t = 1; t < raws.size(); ++t) order.push_back((int) t);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return raws[a].voxels > raws[b].voxels;
        });

        const double voxelMm3 = g.spacing[0] * g.spacing[1] * g.spacing[2];
        std::vector<short> remap((size_t) raws.size(), 0);
        for (size_t q = 0; q < order.size(); ++q) {
            const int oldId = order[q];
            const short newId = (short) (q + 1);
            remap[(size_t) oldId] = newId;
            const Raw &raw = raws[oldId];
            AiInstance ins;
            ins.id = newId;
            ins.voxels = raw.voxels;
            ins.volumeMm3 = (double) raw.voxels * voxelMm3;
            ins.centroid = Vec3(raw.sx / (double) raw.voxels, raw.sy / (double) raw.voxels,
                                raw.sz / (double) raw.voxels);
            ins.bboxMin = Vec3(raw.mMin[0], raw.mMin[1], raw.mMin[2]);
            ins.bboxMax = Vec3(raw.mMax[0], raw.mMax[1], raw.mMax[2]);
            ins.meanHu = raw.huSum / (double) raw.voxels;
            const double var = raw.huSqSum / (double) raw.voxels - ins.meanHu * ins.meanHu;
            ins.sdHu = var > 0.0 ? std::sqrt(var) : 0.0;
            ins.minHu = raw.huMin;
            ins.maxHu = raw.huMax;
            // 长轴：协方差用"平行轴定理"从第一遍累加的二阶矩还原
            //   Cov = E[x xᵀ] - mean·meanᵀ
            // 早期实现是再扫一遍整个 modelDim 体积找 oldId，实例数 N 时复杂度 O(N·V)；
            // 现在统计与连通域同遍完成，复杂度降到 O(V)。数值上与重扫等价（差分式
            // 与两段式累加都是 double，差异仅在求和顺序，量级 1e-12 mm²）。
            const double inv = 1.0 / (double) raw.voxels;
            double cov[6];
            cov[0] = (raw.sxx * inv - ins.centroid.x * ins.centroid.x);
            cov[1] = (raw.sxy * inv - ins.centroid.x * ins.centroid.y);
            cov[2] = (raw.sxz * inv - ins.centroid.x * ins.centroid.z);
            cov[3] = (raw.syy * inv - ins.centroid.y * ins.centroid.y);
            cov[4] = (raw.syz * inv - ins.centroid.y * ins.centroid.z);
            cov[5] = (raw.szz * inv - ins.centroid.z * ins.centroid.z);
            pcaPrincipalAxis(cov, ins.axis);
            r.instances.push_back(ins);
        }
        // 应用重编号（一次性原地替换，避免二次扫描整块体积）
        for (size_t t = 0; t < n; ++t) {
            const short v = r.inst[t];
            if (v > 0) r.inst[t] = remap[(size_t) v];
        }
        r.toothVoxels = 0;
        for (size_t t = 0; t < n; ++t) if (r.label[t]) ++r.toothVoxels;
        LOGD("connectedComponents: %zu instances, tooth voxels=%lld",
             r.instances.size(), r.toothVoxels);
    }

    void pcaPrincipalAxis(const double cov[6], Vec3 &axis) {
        // 对称 3x3 存成上三角 6 元：[a00, a01, a02, a11, a12, a22]
        const double a00 = cov[0], a01 = cov[1], a02 = cov[2];
        const double a11 = cov[3], a12 = cov[4], a22 = cov[5];
        double m[3][3] = {{a00, a01, a02}, {a01, a11, a12}, {a02, a12, a22}};
        double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // 特征向量累乘（列向量）
        for (int sweep = 0; sweep < 24; ++sweep) {
            // 选非对角最大者做 Jacobi 旋转
            int p = 0, q = 1;
            double off = std::fabs(m[0][1]);
            if (std::fabs(m[0][2]) > off) {
                off = std::fabs(m[0][2]);
                p = 0;
                q = 2;
            }
            if (std::fabs(m[1][2]) > off) {
                off = std::fabs(m[1][2]);
                p = 1;
                q = 2;
            }
            if (off < 1e-18) break;
            const double app = m[p][p], aqq = m[q][q], apq = m[p][q];
            double theta = (aqq - app) / (2.0 * apq);
            double t = (theta >= 0.0 ? 1.0 : -1.0)
                       / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
            const double cs = 1.0 / std::sqrt(t * t + 1.0);
            const double sn = cs * t;
            for (int k = 0; k < 3; ++k) {
                const double mkp = m[k][p], mkq = m[k][q];
                m[k][p] = cs * mkp - sn * mkq;
                m[k][q] = sn * mkp + cs * mkq;
            }
            for (int k = 0; k < 3; ++k) {
                const double mpk = m[p][k], mqk = m[q][k];
                m[p][k] = cs * mpk - sn * mqk;
                m[q][k] = sn * mpk + cs * mqk;
            }
            for (int k = 0; k < 3; ++k) {
                const double vkp = v[k][p], vkq = v[k][q];
                v[k][p] = cs * vkp - sn * vkq;
                v[k][q] = sn * vkp + cs * vkq;
            }
        }
        // 最大特征值在 m 的对角线上；其对应特征向量是 v 的同列
        int best = 0;
        if (m[1][1] > m[best][best]) best = 1;
        if (m[2][2] > m[best][best]) best = 2;
        Vec3 ax(v[0][best], v[1][best], v[2][best]);
        ax = ax.normalized();
        if (ax.length() < 0.5) ax = Vec3(0, 0, 1);
        // 符号约定：长轴统一指向体素 z 增大的一侧，AI-03 再按牙冠朝向翻向根方
        if (ax.z < 0.0) ax = ax * -1.0;
        axis = ax;
    }

    // =========================================================================
    // 牙弓归属
    // =========================================================================

    void archAssign(AiResult &r) {
        if (r.instances.empty()) return;
        const AiGrid &g = r.grid;
        // 用质心 z 排序后的最大间隙切两弓：上/下牙列在轴位方向天然分离，
        // 这比 HU/形态学假设都更稳，且对"哪一侧是上颌"不做病人系假设 ——
        // arch=0 恒指 z 索引较大的一侧，报告与 AI-03 都按同一约定使用。
        std::vector<int> order;
        for (size_t t =0 ; t < r.instances.size(); ++t) order.push_back((int) t);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return r.instances[a].centroid.z < r.instances[b].centroid.z;
        });
        double bestGap = -1.0;
        size_t split = 0;
        for (size_t t = 1; t < order.size(); ++t) {
            const double gap = r.instances[order[t]].centroid.z
                               - r.instances[order[t - 1]].centroid.z;
            if (gap > bestGap) {
                bestGap = gap;
                split = t;
            }
        }
        // 只有一组（split==0 或 ==size）时全部归同一弓
        const double occlusalZ = (split > 0 && split < order.size())
                                 ? 0.5 * (r.instances[order[split - 1]].centroid.z
                                          + r.instances[order[split]].centroid.z)
                                 : (g.appDim[2] > 0 ? g.appSpacing[2] * g.appDim[2] * 0.5 : 0.0);
        for (size_t t = 0; t < r.instances.size(); ++t) {
            AiInstance &ins = r.instances[t];
            ins.arch = ins.centroid.z >= occlusalZ ? 0 : 1;
            // 牙冠端 = bbox 在 z 上更靠近咬合平面的那一端（种植体入点就在这端）
            const double dLo = std::fabs(ins.bboxMin.z - occlusalZ);
            const double dHi = std::fabs(ins.bboxMax.z - occlusalZ);
            ins.crownZ = dHi < dLo ? ins.bboxMax.z : ins.bboxMin.z;
            ins.rootZ = dHi < dLo ? ins.bboxMin.z : ins.bboxMax.z;
        }
        // 牙弓内按绕弓心的极角排序并编号（AI-03 需要"沿弓相邻"关系）
        for (int arch = 0; arch < 2; ++arch) {
            double sx = 0.0, sy = 0.0;
            int cnt = 0;
            for (size_t t = 0; t < r.instances.size(); ++t) {
                if (r.instances[t].arch != arch) continue;
                sx += r.instances[t].centroid.x;
                sy += r.instances[t].centroid.y;
                ++cnt;
            }
            if (cnt == 0) continue;
            const Vec3 c(sx / cnt, sy / cnt, 0.0);
            std::vector<std::pair<double, int> > keys;
            for (size_t t = 0; t < r.instances.size(); ++t) {
                if (r.instances[t].arch != arch) continue;
                const double ang = std::atan2(r.instances[t].centroid.y - c.y,
                                              r.instances[t].centroid.x - c.x) * 180.0 / 3.14159265358979;
                r.instances[t].archAngleDeg = ang;
                keys.push_back(std::make_pair(ang, (int) t));
            }
            std::sort(keys.begin(), keys.end());
            // 弓心两侧的牙在极角上差 180 度，直接排序会把牙列从中间断开；
            // 这里按"离弓心同一侧起"旋转排序起点，使序号沿弓连续。
            size_t start = 0;
            double bestRad = -1.0;
            for (size_t t = 0; t < keys.size(); ++t) {
                const int id = keys[t].second;
                const double rad = std::hypot(r.instances[id].centroid.x - c.x,
                                              r.instances[id].centroid.y - c.y);
                if (rad > bestRad) {
                    bestRad = rad;
                    start = t;
                }
            }
            for (size_t t = 0; t < keys.size(); ++t) {
                const size_t pos = (start + t) % keys.size();
                r.instances[keys[pos].second].toothCountHint = (int) t + 1;
            }
        }
        LOGD("archAssign: occlusal z=%.2fmm gap=%.2fmm instances=%zu",
             occlusalZ, bestGap, r.instances.size());
    }

    // =========================================================================
    // 轮廓 -> 叠加图元
    // =========================================================================

    bool sliceMask(const AiResult &r, int plane, int position,
                   std::vector<short> &mask, int &w, int &h, int &axisA, int &axisB) {
        const AiGrid &g = r.grid;
        if (!r.hasMask()) return false;
        int fixed = 2;
        axisA = 0;
        axisB = 1;
        if (plane == MP_CORONAL) {
            fixed = 1;
            axisA = 0;
            axisB = 2;
        } else if (plane == MP_SAGITTAL) {
            fixed = 0;
            axisA = 1;
            axisB = 2;
        }
        int fIdx = 0;
        if (g.factor[fixed] > 0) fIdx = position / g.factor[fixed];
        if (fIdx < 0) fIdx = 0;
        if (fIdx >= g.redDim[fixed]) return false;    // 该层没有任何掩膜体素
        if (plane == MP_AXIAL) {
            w = g.redDim[0];
            h = g.redDim[1];
        } else if (plane == MP_CORONAL) {
            w = g.redDim[0];
            h = g.redDim[2];
        } else {
            w = g.redDim[1];
            h = g.redDim[2];
        }
        mask.assign((size_t) w * h, 0);
        const int md[3] = {g.modelDim[0], g.modelDim[1], g.modelDim[2]};
        for (int a = 0; a < w; ++a) {
            for (int b = 0; b < h; ++b) {
                int idx[3] = {0, 0, 0};
                idx[fixed] = fIdx;
                idx[axisA] = a;
                idx[axisB] = b;
                if (idx[0] >= g.redDim[0] || idx[1] >= g.redDim[1] || idx[2] >= g.redDim[2])
                    continue;
                const size_t t = (size_t) ((long long) idx[0] * md[1] + idx[1]) * md[2] + idx[2];
                mask[(size_t) a * h + b] = r.inst[t];
            }
        }
        (void) axisB;
        return true;
    }

    /**
     * 正交轮廓追踪。
     *
     * 掩膜是体素的并集，边界必然由轴对齐线段组成，所以这里不用 marching squares
     * 的 16  case 插值（那会把直角边插成斜边，画出来比实际体素区域"胖"半格）：
     * 直接收集"本像素与异类邻居之间的公共边"作为有向线段，再串成闭环。
     * 顶点坐标是"边界网格"上的整数 (va, vb)，像素 (a,b) 占据 [a,a+1]x[b,b+1]，
     * 因此世界坐标 = 顶点序号 * 该轴体素间距 —— 恰好落在两个体素中心的中间，
     * 也就是体素盒的真实边界。
     *
     * 输出：loops 为 [x0,y0,x1,y1,...] 的整数顶点对；同一段边被相邻的两个实例
     * 各取一次（绕行方向相反），因此邻牙边界线不会丢。
     */
    void traceContours(const std::vector<short> &mask, int w, int h,
                       std::vector<std::vector<int> > &loops, std::vector<int> &loopLabel) {
        loops.clear();
        loopLabel.clear();
        if (w <= 0 || h <= 0) return;
        struct Edge {
            int sx, sy, ex, ey;
            int label;
            bool used;
        };
        std::vector<Edge> edges;
        edges.reserve((size_t) w * h * 2);
        const int dx[4] = {0, 1, 0, -1};
        const int dy[4] = {1, 0, -1, 0};
        // 每条边的起点/终点（逆时针：区域在左手侧）
        const int s0[4] = {0, 1, 1, 0};
        const int s1[4] = {1, 1, 0, 0};
        const int e0[4] = {1, 1, 0, 0};
        const int e1[4] = {1, 0, 0, 1};
        for (int a = 0; a < w; ++a) {
            for (int b = 0; b < h; ++b) {
                const short L = mask[(size_t) a * h + b];
                if (L == 0) continue;
                for (int d = 0; d < 4; ++d) {
                    const int na = a + dx[d], nb = b + dy[d];
                    short nL = 0;
                    if (na >= 0 && nb >= 0 && na < w && nb < h) nL = mask[(size_t) na * h + nb];
                    if (nL == L) continue;
                    Edge e;
                    e.sx = a + s0[d];
                    e.sy = b + s1[d];
                    e.ex = a + e0[d];
                    e.ey = b + e1[d];
                    e.label = L;
                    e.used = false;
                    edges.push_back(e);
                }
            }
        }
        if (edges.empty()) return;
        // 起点 -> 边下标（同一点可能有多条出边，取列表）
        std::multimap<long long, size_t> byStart;
        for (size_t t = 0; t < edges.size(); ++t) {
            byStart.insert(std::make_pair((long long) edges[t].sx * (h + 1) + edges[t].sy, t));
        }
        for (size_t t = 0; t < edges.size(); ++t) {
            if (edges[t].used) continue;
            std::vector<int> loop;
            size_t cur = t;
            int guard = 0;
            while (guard++ <= (int) edges.size()) {
                Edge &e = edges[cur];
                if (e.used) break;
                e.used = true;
                if (loop.empty()) {
                    loop.push_back(e.sx);
                    loop.push_back(e.sy);
                }
                loop.push_back(e.ex);
                loop.push_back(e.ey);
                if (e.ex == edges[t].sx && e.ey == edges[t].sy) break;   // 回到起点
                const long long key = (long long) e.ex * (h + 1) + e.ey;
                std::multimap<long long, size_t>::iterator it = byStart.lower_bound(key);
                size_t nxt = (size_t) -1;
                for (; it != byStart.end() && it->first == key; ++it) {
                    if (!edges[it->second].used && edges[it->second].label == e.label) {
                        nxt = it->second;
                        break;
                    }
                }
                if (nxt == (size_t) -1) break;      // 断链（T 型接缝）：当前环就此收口
                cur = nxt;
            }
            if (loop.size() >= 8) {
                // 合并共线顶点（正交轮廓里 90% 是阶梯，合完顶点数下降到几十）
                std::vector<int> simp;
                const size_t m = loop.size() / 2;
                for (size_t v = 0; v < m; ++v) {
                    const size_t p = (v + m - 1) % m, q = (v + 1) % m;
                    const int ax = loop[p * 2], ay = loop[p * 2 + 1];
                    const int bx = loop[v * 2], by = loop[v * 2 + 1];
                    const int cx = loop[q * 2], cy = loop[q * 2 + 1];
                    if ((ax == bx && bx == cx) || (ay == by && by == cy)) continue;
                    simp.push_back(bx);
                    simp.push_back(by);
                }
                if (simp.size() >= 8) {
                    loops.push_back(simp);
                    loopLabel.push_back((int) edges[t].label);
                }
            }
        }
    }

    void buildOverlay(const AiResult &r, int plane, int position,
                      std::vector<OverlayPrim> &out) {
        if (!r.overlayVisible || !r.hasMask()) return;
        std::vector<short> mask;
        int w = 0, h = 0, axisA = 0, axisB = 1;
        if (!sliceMask(r, plane, position, mask, w, h, axisA, axisB)) return;
        std::vector<std::vector<int> > loops;
        std::vector<int> labels;
        traceContours(mask, w, h, loops, labels);
        if (loops.empty()) return;

        const AiGrid &g = r.grid;
        int fixed = 2;
        if (plane == MP_CORONAL) fixed = 1;
        else if (plane == MP_SAGITTAL) fixed = 0;
        const int fIdx = position / (g.factor[fixed] > 0 ? g.factor[fixed] : 1);
        const double fixedWorld = ((double) fIdx + 0.5) * g.spacing[fixed];
        // 实例 id -> 该层面积最大环的质心（文字锚点）
        std::map<int, std::pair<double, double> > anchor;
        std::map<int, double> anchorArea;

        size_t emitted = 0;
        for (size_t t = 0; t < loops.size() && emitted < (size_t) MAX_CONTOUR_PRIMS; ++t) {
            const int lid = labels[t];
            const std::vector<int> &lp = loops[t];
            OverlayPrim prim;
            prim.kind = OK_POLYGON;
            prim.color = AiConst::PALETTE[((lid - 1) % AiConst::PALETTE_COUNT
                                           + AiConst::PALETTE_COUNT) % AiConst::PALETTE_COUNT];
            prim.widthPx = 1.8;
            prim.fill = true;
            prim.ownerKind = OW_AI;
            prim.ownerId = lid;
            double area = 0.0, sx = 0.0, sy = 0.0;
            for (size_t v = 0; v + 1 < lp.size(); v += 2) {
                const int va = lp[v], vb = lp[v + 1];
                double c[3] = {0.0, 0.0, 0.0};
                c[fixed] = fixedWorld;
                c[axisA] = (double) va * g.spacing[axisA];
                c[axisB] = (double) vb * g.spacing[axisB];
                prim.world.push_back(Vec3(c[0], c[1], c[2]));
                sx += c[axisA];
                sy += c[axisB];
            }
            const size_t nv = lp.size() / 2;
            if (nv < 2) continue;
            // 鞋带公式求该环面积（边界网格单位 -> mm²），用于挑最大的环做文字锚点
            for (size_t v = 0; v + 3 < lp.size(); v += 2) {
                area += (double) lp[v] * lp[v + 3] - (double) lp[v + 2] * lp[v + 1];
            }
            area += (double) lp[lp.size() - 2] * lp[1] - (double) lp[0] * lp[lp.size() - 1];
            area = std::fabs(area) * 0.5 * g.spacing[axisA] * g.spacing[axisB];
            if (area > anchorArea[lid]) {
                anchorArea[lid] = area;
                anchor[lid] = std::make_pair(sx / (double) nv, sy / (double) nv);
            }
            // 只给最大的环写字（同一实例在同一层可能有多个环）
            prim.text.clear();
            out.push_back(prim);
            ++emitted;
        }
        // 每个实例在本层补一个质心点 + 文本，避免上千个体素都画标记
        for (std::map<int, std::pair<double, double> >::iterator it = anchor.begin();
             it != anchor.end(); ++it) {
            const int lid = it->first;
            const AiInstance *ins = nullptr;
            for (size_t t = 0; t < r.instances.size(); ++t) {
                if (r.instances[t].id == lid) {
                    ins = &r.instances[t];
                    break;
                }
            }
            if (!ins) continue;
            double c[3] = {0.0, 0.0, 0.0};
            c[fixed] = fixedWorld;
            c[axisA] = it->second.first;
            c[axisB] = it->second.second;
            char buf[128];
            snprintf(buf, sizeof(buf), "AI#%d %.2fcm3 %dvox", lid, ins->volumeMm3 / 1000.0,
                     (int) ins->voxels);
            OverlayPrim dot;
            dot.kind = OK_POINT;
            dot.color = AiConst::PALETTE[(lid - 1) % AiConst::PALETTE_COUNT];
            dot.widthPx = 2.0;
            dot.world.push_back(Vec3(c[0], c[1], c[2]));
            dot.text = buf;
            dot.labelAnchored = 1;
            dot.ownerKind = OW_AI;
            dot.ownerId = lid;
            out.push_back(dot);
        }
    }

    // =========================================================================
    // 指标 / 摘要
    // =========================================================================

    double dice(const unsigned char *a, const unsigned char *b, long long n,
                double *sensitivity, double *specificity) {
        long long tp = 0, fp = 0, fn = 0, tn = 0;
        for (long long t = 0; t < n; ++t) {
            const bool x = a[t] != 0, y = b[t] != 0;
            if (x && y) ++tp;
            else if (x && !y) ++fp;
            else if (!x && y) ++fn;
            else ++tn;
        }
        const double denom = (double) (2 * tp + fp + fn);
        if (sensitivity) *sensitivity = (tp + fn) > 0 ? (double) tp / (double) (tp + fn) : 0.0;
        if (specificity) *specificity = (tn + fp) > 0 ? (double) tn / (double) (tn + fp) : 1.0;
        return denom > 0.0 ? (2.0 * (double) tp) / denom : 0.0;
    }

    std::string summaryText(const AiResult &r) {
        if (!r.ok || !r.hasMask()) return std::string("AI 未运行");
        double total = 0.0;
        long long vox = 0;
        for (size_t t = 0; t < r.instances.size(); ++t) {
            total += r.instances[t].volumeMm3;
            vox += r.instances[t].voxels;
        }
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "AI-01: %zu 个连通域 / 掩膜 %lld 体素 / 合计 %.2f cm³ / %.0f ms (prep %.0f + ort %.0f + post %.0f)",
                 r.instances.size(), vox, total / 1000.0, r.totalMs, r.prepMs, r.inferMs,
                 r.postMs);
        return std::string(buf);
    }

}   // namespace AiCore
