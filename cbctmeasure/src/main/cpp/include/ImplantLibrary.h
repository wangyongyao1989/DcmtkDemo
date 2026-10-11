#ifndef DCMTKDEMO_IMPLANTLIBRARY_H
#define DCMTKDEMO_IMPLANTLIBRARY_H

#include <string>
#include <vector>

#include "include/Json.h"

/**
 * 植体型号库（升级建议 P-09）。
 *
 * 为什么需要它：AI-03 原来把"建议规格"写成硬编码的直径阶梯 + 连续长度
 * （core/AiPlanner.cpp 里的 kDiaLadder），临床上不可采购 —— 医生拿到的是
 * "Ø4.27×10.8mm"这种不存在的型号。型号库把这一层换成**离散的可选型目录**，
 * 并让推荐结果落到真实存在的 (Ø, 长度) 组合上。
 *
 * 数据的诚实边界（重要）：
 *   - 内置库（kBuiltin）只有**体直径与桩体长度**两个几何量，且刻意用
 *     "GEN-Øx.y-Lz"这种通用编号而不是任何厂商的商品名；
 *     platformMm / abutmentMm / connection 一律留空或 0，
 *     因为平台切换与连接基台尺寸属于厂商目录数据，凭记忆填写等于编造。
 *   - 真实型号必须来自 json 资产（assets/implant_models.json 或外置文件），
 *     由 loadFromJson() 覆盖内置库；"这批数据能不能当商品型号用"由 isGeneric()
 *     从数据本身派生（所有条目 vendor 为空即通用），一路带进 plan.json 与报告，
 *     让审阅者一眼看出这次评估用的是通用阶梯还是真实产品目录。
 *
 * 与判级的关系：本库**不引入任何新的临床阈值**。S-03/S-04/S-05/S-06 的阈值
 * 仍然只有 ImplantPlanner::kMin* 那一套；型号库只影响"建议用哪颗桩体"。
 */
struct ImplantModelSpec {
    std::string sku;            // 型号编号（库内唯一，落进 Implant.sku）
    std::string vendor;         // 厂商（内置通用库为空）
    std::string series;         // 系列
    std::string connection;     // 连接方式文字（不参与计算）
    double diaMm = 0.0;         // 体直径（S-04/S-06 用的就是它）
    double lengthMm = 0.0;      // 桩体全长
    double platformMm = 0.0;    // 平台直径（0 = 资产未提供）
    double abutmentMm = 0.0;    // 基台高度（0 = 资产未提供）
    std::string note;

    bool generic() const { return vendor.empty(); }

    /** "GEN-Ø4.3×11.5" 形式，UI/报告共用 */
    std::string labelText() const;

    Json toJson() const;
    static ImplantModelSpec fromJson(const Json &j);
};

class ImplantLibrary {
public:
    /**
     * 进程内共享实例（与 JNI 会话表同一套"主线程串行访问"约定，不加锁）。
     * 首次取用会自动载入内置通用库，因此任何时刻 count() 都 > 0，
     * AI-03 不会因为"忘了加载资产"而退化成连续长度。
     */
    static ImplantLibrary &shared();

    /** 清空（主机单测用；清空后 specs() 为空，匹配一律返回 false） */
    void clear();

    /** 恢复内置通用库（覆盖当前内容） */
    void loadBuiltin();

    /**
     * 从 json 资产载入（覆盖当前内容）。
     * 文本可以是整个对象 `{"models":[...]}` 或直接是数组 `[...]`。
     * 解析失败或一条都没有时**保持原内容不变**并返回 -1，
     * 避免"资产写坏 -> 型号库突然变空 -> 推荐口径悄悄改变"。
     * @return 载入条数，或 -1（失败，err 给出原因）
     */
    int loadFromJson(const std::string &text, std::string *err = nullptr);

    /** 序列化（当前库的全部型号；供 JNI 回传与归档取证） */
    std::string dumpJson() const;

    const std::vector<ImplantModelSpec> &specs() const { return specs_; }
    size_t count() const { return specs_.size(); }
    /** 当前内容是 C++ 内置表（true）还是 json 资产（false）—— 只表示"来源" */
    bool isBuiltin() const { return builtin_; }

    /**
     * 数据口径：所有条目 vendor 都为空 -> 通用名义尺寸目录（不是厂商商品数据）。
     *
     * 报告/SR 的"型号库：……"一行必须读这个而不是 isBuiltin()：随包的
     * assets/implant_models.json 本身就是一组通用名义尺寸（vendor 全空），
     * 若按"是不是内置表"来表述，它会被印成"厂商目录"，等于凭空给读者一个
     * 不存在的采购依据。builtin_ 只回答"这份数据从哪来"，generic 回答"能不能当商品型号用"。
     */
    bool isGeneric() const;

    const ImplantModelSpec *bySku(const std::string &sku) const;

    /**
     * 骨量约束下的选型：在 `diaMm <= maxDiaMm 且 lengthMm <= maxLenMm` 的型号里
     * 取**最长**的一档（同长度取较粗的），因为临床上"能放多长"决定初期稳定性。
     * 没有任何型号能塞进这个骨量包络时返回 nullptr（调用方按"需植骨/改方案"处理）。
     */
    const ImplantModelSpec *fitMatch(double maxDiaMm, double maxLenMm) const;

    /** 无约束的最近型号（|Δ直径|/0.5 + |Δ长度|/1.0 最小）；库空时返回 nullptr */
    const ImplantModelSpec *nearestMatch(double diaMm, double lengthMm) const;

private:
    ImplantLibrary();

    std::vector<ImplantModelSpec> specs_;
    bool builtin_ = true;
};

#endif // DCMTKDEMO_IMPLANTLIBRARY_H
