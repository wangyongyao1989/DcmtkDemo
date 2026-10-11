// AI-02 下颌管掩膜 -> 神经管中心线的实现（规则说明见 include/NerveAutoTracker.h）。
//
// 本文件是纯 C++：不依赖 ONNX Runtime、不依赖 JNI、不依赖 AiResult 之外的状态，
// 因此在 macOS 上可以用 sh cbctmeasure/src/host/build_and_run.sh 直接单测，
// 同一套规则也能被主机脚本 scripts/canal.py 的 centerlines() 复算，
// 两端逐点比对就是第 8 轮报告里"设备中心线 == 主机中心线"的证据。

#include "include/NerveAutoTracker.h"

#include <android/log.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

#define TAG "CbctMeasureAi"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace NerveAutoTracker {

    namespace {

        /** 26 邻域偏移，与 AiCore::connectedComponents 同一套遍历顺序 */
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
        static const Neighbor26 kN26;

        /** 线性索引 -> (i,j,k)；stride 与 label 的 C 序布局一致 */
        inline void decode(int lin, long long stride0, int md2, int out[3]) {
            const int i = (int) ((long long) lin / stride0);
            const int rem = (int) ((long long) lin - (long long) i * stride0);
            const int j = rem / md2;
            out[0] = i;
            out[1] = j;
            out[2] = rem - j * md2;
        }

        struct Group {
            long long cnt;
            double sum[3];

            Group() : cnt(0) { sum[0] = sum[1] = sum[2] = 0.0; }
        };

        /** e) 的中位数：偶数个取中间两个的算术平均（与 numpy.median 一致） */
        double medianInPlace(std::vector<double> &v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            const size_t m = v.size() / 2;
            return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
        }

        /** 折线点数上限保护，见头文件 MAX_TRACK_POINTS 的说明 */
        void decimate(std::vector<Vec3> &pts) {
            const size_t n = pts.size();
            if (n <= (size_t) MAX_TRACK_POINTS) return;
            std::vector<Vec3> keep;
            keep.reserve((size_t) MAX_TRACK_POINTS);
            const double step = (double) (n - 1) / (double) (MAX_TRACK_POINTS - 1);
            for (int q = 0; q < MAX_TRACK_POINTS; ++q) {
                keep.push_back(pts[(size_t) std::llround(step * q)]);
            }
            keep.back() = pts.back();      // 末点必须是真末点，否则折线少一段
            pts.swap(keep);
        }

        std::string fnum(double v, int digits) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%.*f", digits, v);
            return std::string(buf);
        }

        /** 按网格轴下标写 Vec3 的分量（结构体不是数组，不依赖跨成员的指针算术） */
        inline void setCoord(Vec3 &v, int axis, double value) {
            if (axis == 0) v.x = value;
            else if (axis == 1) v.y = value;
            else v.z = value;
        }

    }   // namespace

    std::vector<NerveTrack> track(const AiResult &r, int minVoxels, int maxCandidates) {
        std::vector<NerveTrack> out;
        if (!r.hasMask() || r.label.empty()) return out;
        const AiGrid &g = r.grid;
        if (minVoxels <= 0) minVoxels = AiConst::MIN_CANAL_VOXELS;
        if (maxCandidates <= 0) maxCandidates = AiConst::CANAL_MAX_CANDIDATES;
        const size_t n = (size_t) g.modelCount();
        if (r.label.size() != n) {
            LOGW("canal track: label size %zu != modelCount %lld（跳过）",
                 r.label.size(), g.modelCount());
            return out;
        }

        // ---- a) 26 邻域连通域（visited 独立开，不改 r.inst）----
        // 不直接复用 connectedComponents 的 inst：那张表用的是牙齿的散点阈值（8）
        // 和"全部实例"，而下颌管要自己的阈值（12）与"前 6 个候选"。
        // 两套后处理各算各的，牙齿侧以后改一个常量才不会悄悄改变神经的安全判级输入。
        std::vector<unsigned char> visited(n, 0);
        std::vector<std::vector<int> > comps;
        std::vector<int> stack;
        stack.reserve(2048);
        const int md[3] = {g.modelDim[0], g.modelDim[1], g.modelDim[2]};
        const long long stride0 = (long long) md[1] * md[2];
        int ix[3];
        for (size_t t = 0; t < n; ++t) {
            if (visited[t] || r.label[t] == 0) continue;
            std::vector<int> vox;
            stack.clear();
            stack.push_back((int) t);
            visited[t] = 1;
            while (!stack.empty()) {
                const int lin = stack.back();
                stack.pop_back();
                vox.push_back(lin);
                decode(lin, stride0, md[2], ix);
                for (int q = 0; q < 26; ++q) {
                    const int a = ix[0] + kN26.d[q][0];
                    const int b = ix[1] + kN26.d[q][1];
                    const int c = ix[2] + kN26.d[q][2];
                    if (a < 0 || b < 0 || c < 0 || a >= md[0] || b >= md[1] || c >= md[2]) {
                        continue;
                    }
                    const size_t s = (size_t) (((long long) a * md[1] + b) * md[2] + c);
                    if (visited[s] || r.label[s] == 0) continue;
                    visited[s] = 1;
                    stack.push_back((int) s);
                }
            }
            if ((int) vox.size() < minVoxels) continue;      // 散点剔除
            comps.push_back(vox);
        }

        // ---- b) 体素数降序；并列按最小线性索引升序（两端同序才能逐点比对）----
        std::sort(comps.begin(), comps.end(), [](const std::vector<int> &a,
                                                 const std::vector<int> &b) {
            if (a.size() != b.size()) return a.size() > b.size();
            long long ma = LLONG_MAX, mb = LLONG_MAX;
            for (size_t q = 0; q < a.size(); ++q) if ((long long) a[q] < ma) ma = a[q];
            for (size_t q = 0; q < b.size(); ++q) if ((long long) b[q] < mb) mb = b[q];
            return ma < mb;
        });
        if (maxCandidates > 0 && (int) comps.size() > maxCandidates) {
            comps.resize((size_t) maxCandidates);
        }

        // ---- c) 选描记轴 -> d) 逐组质心 -> e) 中位半径 ----
        for (size_t ci = 0; ci < comps.size(); ++ci) {
            const std::vector<int> &vox = comps[ci];
            NerveTrack tr;
            tr.voxels = (long long) vox.size();
            int lo[3] = {md[0], md[1], md[2]};
            int hi[3] = {-1, -1, -1};
            double sum[3] = {0.0, 0.0, 0.0};
            long long minLin = LLONG_MAX;
            for (size_t q = 0; q < vox.size(); ++q) {
                if ((long long) vox[q] < minLin) minLin = vox[q];
                decode(vox[q], stride0, md[2], ix);
                for (int a = 0; a < 3; ++a) {
                    if (ix[a] < lo[a]) lo[a] = ix[a];
                    if (ix[a] > hi[a]) hi[a] = ix[a];
                    sum[a] += (double) ix[a];
                }
            }
            if (hi[0] < 0) continue;
            tr.minLinearIndex = minLin;
            int axis = 0;
            double bestExtent = -1.0;
            for (int a = 0; a < 3; ++a) {
                tr.extentVox[a] = (double) (hi[a] - lo[a] + 1);
                // 严格大于 => 并列时保留先出现的轴（x<y<z），与 numpy argmax 同语义
                if (tr.extentVox[a] > bestExtent) {
                    bestExtent = tr.extentVox[a];
                    axis = a;
                }
            }
            tr.traceAxis = axis;
            for (int a = 0; a < 3; ++a) {
                setCoord(tr.centroid, a,
                         (sum[a] / (double) tr.voxels + 0.5) * g.spacing[a]);
            }

            const int per = md[axis];
            std::vector<Group> groups((size_t) per);
            for (size_t q = 0; q < vox.size(); ++q) {
                decode(vox[q], stride0, md[2], ix);
                Group &gr = groups[(size_t) ix[axis]];
                gr.cnt += 1;
                for (int a = 0; a < 3; ++a) gr.sum[a] += (double) ix[a];
            }
            // 垂直面 = 除描记轴外的另外两轴；体素面积按各自的抽稀间距相乘
            int perp[2] = {0, 1};
            for (int a = 0, w = 0; a < 3; ++a) if (a != axis) perp[w++] = a;
            const double areaUnit = g.spacing[perp[0]] * g.spacing[perp[1]];
            std::vector<double> radii;
            radii.reserve((size_t) per);
            for (int c = 0; c < per; ++c) {          // 升序扫描 == np.unique 的排序语义
                const Group &gr = groups[(size_t) c];
                if (gr.cnt <= 0) continue;
                double mean[3];
                for (int a = 0; a < 3; ++a) mean[a] = gr.sum[a] / (double) gr.cnt;
                // 抽稀未覆盖的补 0 区不产出点（与 AiCore 统计同一处理）。
                // 牙科序列 redDim==modelDim，这个分支恒不触发。
                if (mean[0] >= (double) g.redDim[0] || mean[1] >= (double) g.redDim[1]
                    || mean[2] >= (double) g.redDim[2]) continue;
                Vec3 p;
                for (int a = 0; a < 3; ++a) {
                    setCoord(p, a, (mean[a] + 0.5) * g.spacing[a]);
                }
                tr.points.push_back(p);
                radii.push_back(std::sqrt((double) gr.cnt * areaUnit
                                          / 3.14159265358979323846));
            }
            tr.groupsUsed = (int) tr.points.size();
            if (tr.points.size() < 2) continue;       // 单点不成线，S-05 用不上
            decimate(tr.points);
            tr.radiusMm = medianInPlace(radii);
            out.push_back(tr);
        }

        LOGD("canal track: %zu 条（散点阈值 %d 体素，候选上限 %d）",
             out.size(), minVoxels, maxCandidates);
        return out;
    }

    std::vector<NervePath> toPaths(const std::vector<NerveTrack> &tracks) {
        std::vector<NervePath> out;
        for (size_t i = 0; i < tracks.size(); ++i) {
            const NerveTrack &t = tracks[i];
            NervePath p;
            // 名称前缀就是"自动生成"的身份证：会话按它清理旧行，
            // 医生手工描记的神经管（名字不同）不会被 AI 重跑删掉。
            p.name = std::string(AiConst::CANAL_AUTO_NOTE) + " " + std::to_string((int) i + 1);
            p.color = (int) AiConst::CANAL_COLOR;
            p.visible = true;
            p.points = t.points;
            // 半径夹进 [0.4, 6.0]mm：它直接进入 ImplantPlanner::nerveDistance 的减项。
            // 中位数在极少数情况下会给出 0（某截面只剩 1 个被部分容积吃掉的体素），
            // 那会让"桩尖贴到管壁"不再违规；反之异常大的值会把安全距离压成负数误报。
            // 解剖上下颌管半径约 1.0~2.5mm，这个区间足够宽，只在病态值上生效。
            p.radiusMm = std::max(0.4, std::min(6.0, t.radiusMm));
            out.push_back(p);
        }
        return out;
    }

    Json tracksJson(const std::vector<NerveTrack> &tracks) {
        static const char *const AXIS_NAMES[3] = {"x", "y", "z"};
        Json arr = Json::makeArray();
        for (size_t i = 0; i < tracks.size(); ++i) {
            const NerveTrack &t = tracks[i];
            const int ax = std::min(2, std::max(0, t.traceAxis));
            Json o = Json::makeObject();
            o.set("voxels", Json::makeNumber((double) t.voxels));
            o.set("traceAxis", Json::makeNumber(t.traceAxis));
            o.set("traceAxisName", Json::makeString(AXIS_NAMES[ax]));
            Json ext = Json::makeArray();
            for (int a = 0; a < 3; ++a) ext.push(Json::makeNumber(t.extentVox[a]));
            o.set("extentVox", ext);
            o.set("radiusMm", Json::makeNumber(t.radiusMm));
            o.set("groups", Json::makeNumber(t.groupsUsed));
            o.set("minLinearIndex", Json::makeNumber((double) t.minLinearIndex));
            Json cen = Json::makeArray();
            cen.push(Json::makeNumber(t.centroid.x));
            cen.push(Json::makeNumber(t.centroid.y));
            cen.push(Json::makeNumber(t.centroid.z));
            o.set("centroidMm", cen);
            Json pts = Json::makeArray();
            for (size_t q = 0; q < t.points.size(); ++q) {
                Json one = Json::makeArray();
                one.push(Json::makeNumber(t.points[q].x));
                one.push(Json::makeNumber(t.points[q].y));
                one.push(Json::makeNumber(t.points[q].z));
                pts.push(one);
            }
            o.set("pointsMm", pts);
            arr.push(o);
        }
        return arr;
    }

    std::string summaryText(const std::vector<NerveTrack> &tracks, double thresholdUsed,
                            bool measuredRadius) {
        const std::string thr = thresholdUsed > 0.0 ? fnum(thresholdUsed, 2) : std::string("?");
        if (tracks.empty()) {
            return std::string("AI-02 下颌管：本次没追出可用的管道（阈值 ") + thr
                   + "，单条至少 " + std::to_string(AiConst::MIN_CANAL_VOXELS)
                   + " 体素）；神经距离继续以手工描记为准";
        }
        std::string list;
        const char *const AX = "xyz";
        for (size_t i = 0; i < tracks.size() && i < 4; ++i) {
            if (!list.empty()) list += "、";
            list += std::to_string(tracks[i].voxels) + "体素/半径"
                    + fnum(tracks[i].radiusMm, 2) + "mm/轴"
                    + AX[std::min(2, std::max(0, tracks[i].traceAxis))];
        }
        std::string head = "AI-02 自动下颌管：" + std::to_string(tracks.size()) + " 条（" + list
                + (tracks.size() > 4 ? "…" : "") + "）；阈值 " + thr
                + "；后处理 >=" + std::to_string(AiConst::MIN_CANAL_VOXELS) + " 体素、前 "
                + std::to_string(AiConst::CANAL_MAX_CANDIDATES) + " 候选；";
        head += measuredRadius
                ? "半径取掩膜实测截面中位数（S-05 的减项用它，不是固定 1.5mm）"
                : "半径回退固定 1.5mm（实测截面不可用）";
        return head;
    }

}   // namespace NerveAutoTracker
