#ifndef DCMTKDEMO_MEASUREMENTMANAGER_H
#define DCMTKDEMO_MEASUREMENTMANAGER_H

#include <string>
#include <vector>

#include "include/AiPlanner.h"
#include "include/AiTypes.h"
#include "include/ImplantPlanner.h"
#include "include/MeasurePicker.h"
#include "include/MeasureTypes.h"
#include "include/RoiExtractor.h"
#include "include/VolumeRef.h"
#include "include/Json.h"

/**
 * 测量会话管理器（PRD 7.3 的 MeasurementManager）。
 *
 * 职责边界：
 *   - 持有本次会话的全部临床对象：测量项、ROI、种植体方案、神经管路径；
 *   - 所有数值计算在这里发生（体积/面积/骨密度走 RoiExtractor，
 *     种植体安全走 ImplantPlanner），Kotlin 侧只负责展示与持久化路径；
 *   - 输出"世界坐标叠加图元"，投影到屏幕由 Kotlin 通过 :cbctdeal 的
 *     projectPoints() 完成 —— 这是"本模块不链接 VTK"决策的另一半。
 *
 * 线程约定：与 :cbctdeal 一致，实例由 JNI 层的会话表持有，
 * 同一会话的调用来自 Kotlin 主线程（计算走 post 到渲染线程之外独立执行），
 * 因此本类不加锁；VolumeRef 指向的体数据在会话期间必须保持有效。
 *
 * AI（PRD 5.6）在这里的边界：本类**不认识 ONNX Runtime**。推理由 JNI 层用
 * core/AiEngine + ai/OrtEngine 完成后，把 AiResult 交给 adoptAiResult()；
 * 之后掩膜叠加、R-06 掩膜 ROI、M-04/M-08 复用、AI-03 推荐全部走一期就有的
 * 那套对象模型，因此"AI 挂了不影响测量"是结构上的性质而不是 try/catch。
 *
 * 继承 AiMaskQuery：RoiExtractor 需要知道"世界点是否属于实例 L"才能统计 R-06，
 * 而这个映射（抽稀网格、补 0 区、轴序）只有持有 AiResult 的一方说得清，
 * 所以由本类实现该接口并在构造时把自己注入 extractor_（见 cpp 构造注释）。
 */
class MeasurementManager : public AiMaskQuery {
public:
    MeasurementManager();

    /** 绑定体数据（loadSeries 之后、任何计算之前必须调用） */
    void bindVolume(const CbctVolume *vol);
    bool hasVolume() const { return vol_.valid(); }
    const VolumeRef &volume() const { return vol_; }

    // ---- M-01 ~ M-08：提交并计算 ----
    /**
     * 追加一条测量。points 至少要满足该类型的最小点数（距离 2 / 角度 3 /
     * 弧长 2 / HU 采样 1 / 体积与骨密度走 roiId）。
     * 计算失败时 ok=false 并把原因写入 record.detailJson，记录仍然保留，
     * 便于 UI 显示"待重算"而不是静默丢弃。
     */
    MeasureRecord addMeasure(int type, const std::vector<Vec3> &points, int roiId,
                             const std::string &name, unsigned int color,
                             const std::string &note = std::string());

    /** 重算单条（体数据换窗、ROI 参数变化后调用） */
    bool recalcMeasure(int id);
    bool removeMeasure(int id);
    bool renameMeasure(int id, const std::string &name);
    bool setMeasureVisible(int id, bool visible);
    bool setMeasureNote(int id, const std::string &note);
    const std::vector<MeasureRecord> &measures() const { return records_; }
    const MeasureRecord *findMeasure(int id) const;
    void clearMeasures();

    /** 点按屏幕坐标 -> 世界射线的拾取入口（PRD 5.1.3 的"点按取点"） */
    PickResult pickByRay(const Vec3 &origin, const Vec3 &dir, const PickRequest &req) const;

    /** 吸附到最近体素中心（拖动/落点消抖；无体数据时原样返回） */
    Vec3 snapToVoxelCenter(const Vec3 &p) const;

    // ---- ROI（R-01 ~ R-05）----
    int addRoi(const RoiDef &roi);
    bool updateRoi(const RoiDef &roi);
    bool removeRoi(int id);
    bool setRoiVisible(int id, bool visible);
    const std::vector<RoiDef> &rois() const { return rois_; }
    const RoiDef *findRoi(int id) const;
    /** ROI 统计（体积 + 骨密度）；ROI 不存在时 ok=false */
    RoiStats statRoi(int id) const;
    /** 供叠加层"隔离显示"复用的分割区间（无阈值 ROI 时返回 false） */
    bool dominantHuRange(double &lo, double &hi) const;

    // ---- 种植体方案（S-01 ~ S-06）----
    int addImplant(const Implant &im);
    bool updateImplant(const Implant &im);
    bool removeImplant(int id);
    const std::vector<Implant> &implants() const { return implants_; }
    const Implant *findImplant(int id) const;
    /** 重算全部种植体的安全指标（方案变化后一次性调用，保证互相间距正确） */
    void recomputePlan();

    // ---- 神经管路径（S-05 依据）----
    int addNervePath(const NervePath &path);
    bool updateNervePath(const NervePath &path);
    bool removeNervePath(int id);
    bool appendNervePoint(int id, const Vec3 &p);
    const std::vector<NervePath> &nervePaths() const { return nerves_; }

    // ---- AI-01 / AI-03（PRD 5.6）----
    /** 当前推理结果（只读取证信息、掩膜尺寸；不要在这里做数值计算） */
    const AiResult &ai() const { return ai_; }
    /**
     * 接收一次推理结果（JNI 在 AiEngine::runSegment 之后调用）。
     * 会清掉上一次的结果；overlay 的层位沿用 setAiOverlaySlice 最近一次的值。
     */
    void adoptAiResult(const AiResult &src);
    /** 丢弃推理结果（切序列 / 用户点"清除 AI"）；R-06 ROI 保留但统计变为空 */
    void clearAi();
    /** 掩膜叠加开关（关掉不影响数据，只不画） */
    void setAiOverlayVisible(bool visible);
    /** MPR 当前层面：AI 轮廓只画这一层（由 JNI 的 refreshOverlay 同步进来） */
    void setAiOverlaySlice(int plane, int position);

    /**
     * 把每个分割实例落成一等临床对象：R-06 掩膜 ROI + M-04 体积 + M-08 骨密度。
     * 走的是 addRoi/addMeasure 的正常通道，因此列表、持久化、报告、SR 全部自动包含。
     * @return 新建的测量项条数（每实例 2 条：体积 + 骨密度）
     */
    int aiAutoMeasure(bool withBoneDensity = true);

    /** AI-03 候选（不落库；采纳由 Kotlin 调 addImplant 走 S-01~S-06 正常通道） */
    std::vector<AiCandidate> aiRecommend(double minGapMm, int maxOut) const;

    // ---- AiMaskQuery（R-06 的掩膜查询；判定用 inst 而非 label）----
    bool containsMm(int label, const Vec3 &world) const override;
    bool boundsMm(int label, Vec3 &lo, Vec3 &hi) const override;

    // ---- 叠加图元（世界坐标，交给 Kotlin 投影绘制）----
    void buildOverlay(std::vector<OverlayPrim> &out) const;
    /** 种植体方案的叠加图元：圆柱轮廓 + 螺纹 + 安全色 + 告警文字 */
    void buildPlanOverlay(std::vector<OverlayPrim> &out) const;

    // ---- 持久化（PRD 8.4：measure / plan 两份 JSON，标注由 AnnotationStore 负责）----
    Json measuresJson() const;
    bool measuresFromJson(const Json &j);
    Json planJson() const;
    bool planFromJson(const Json &j);

    /**
     * 单个 ROI / 种植体的 JSON <-> 结构体映射（public static）。
     * 之所以暴露出来：持久化（measuresFromJson / planFromJson 的循环体）与
     * JNI 传输（RoiJni.addRoi / SurgeryPlanJni.addImplant 的入参）必须用同一套
     * 字段名，否则"存进去能读回来、但界面上新建的 ROI 少字段"这类问题
     * 只会在真机上暴露。字段缺失时保留结构体里的默认值，不做校验。
     */
    static void roiFromJson(const Json &j, RoiDef &out);
    static Json roiToJson(const RoiDef &roi);
    static void implantFromJson(const Json &j, Implant &out);
    static Json implantToJson(const Implant &implant);

    int nextId() { return ++nextId_; }

    /** 摘要文本（直方图式的一行统计，供 UI 顶部状态栏） */
    std::string summaryText() const;

private:
    /** 按类型计算 value/unit/detailJson；返回 false 表示输入不足 */
    bool compute(MeasureRecord &rec) const;
    /** 体积换算：mm³ -> cm³（PRD 表 M-04 单位） */
    static std::string unitFor(int type);

    VolumeRef vol_;
    RoiExtractor extractor_;
    ImplantPlanner planner_;
    MeasurePicker picker_;

    AiResult ai_;                     // 最近一次 AI-01 结果
    int aiOverlayPlane_ = MP_AXIAL;   // AI 轮廓绘制层面（与 MeasureJniHelper 的缓存同步）
    int aiOverlayPosition_ = 0;

    std::vector<MeasureRecord> records_;
    std::vector<RoiDef> rois_;
    std::vector<Implant> implants_;
    std::vector<NervePath> nerves_;
    int nextId_ = 0;
};

#endif // DCMTKDEMO_MEASUREMENTMANAGER_H
