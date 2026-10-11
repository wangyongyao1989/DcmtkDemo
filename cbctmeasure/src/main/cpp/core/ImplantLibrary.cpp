// 植体型号库实现（升级建议 P-09）。
//
// 纯 C++11 + 本模块自带的 Json 读写器，主机侧可编译单测（src/host/test_main.cpp
// 的 group14），因此"资产写坏了"能在主机上先发现，而不是等到真机推荐结果变味。

#include "include/ImplantLibrary.h"
#include "include/MeasureMath.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#define TAG "CbctMeasureCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace {

    /**
     * 通用名义尺寸表（不是任何厂商的商品目录）。
     * 取值依据只有两条：① 与本模块既有硬编码阶梯一致（Ø3.5/4.0/4.5/5.0/5.8，
     * 见 core/AiPlanner.cpp 的 kDiaLadder），② 长度为 6~15 的整档/半档，
     * 覆盖 PRD S-03 骨高阈值（10mm）上下。
     * 真实产品目录一律通过 assets/implant_models.json 载入后覆盖本表。
     */
    const double kBuiltinDia[] = {3.4, 3.8, 4.1, 4.8, 5.8};
    const double kBuiltinLens[][5] = {
            {7.0, 8.5, 10.0, 11.5, 0.0},   // Ø3.4（窄牙槽嵴）
            {7.0, 8.5, 10.0, 11.5, 0.0},   // Ø3.8
            {8.5, 10.0, 11.5, 13.0, 0.0},  // Ø4.1（常规）
            {8.5, 10.0, 11.5, 13.0, 0.0},  // Ø4.8（平台切换常见档）
            {9.5, 11.5, 13.0, 15.0, 0.0},  // Ø5.8（磨牙区）
    };
    const size_t kBuiltinDiaCount = sizeof(kBuiltinDia) / sizeof(kBuiltinDia[0]);

    std::string num(double v) { return MeasureMath::format(v, v >= 100.0 ? 0 : 1); }

    /**
     * 整数值不带小数点：整数档就是整数档，写成 GEN-3.4-7 比 GEN-3.4-7.0 更接近
     * 目录习惯。**关键是它让内置 sku 与 assets/implant_models.json 里的 sku
     * 逐字节一致** —— 组14 用这个性质检查"随包的资产目录和内置阶梯是不是同一套"，
     * 两处口径分叉的话审阅者会看到两份不同的通用库。
     */
    std::string skuNum(double v) {
        char buf[32];
        if (std::fabs(v - std::round(v)) < 1e-9) {
            snprintf(buf, sizeof(buf), "%d", (int) std::lround(v));
        } else {
            snprintf(buf, sizeof(buf), "%.1f", v);
        }
        return buf;
    }

}   // namespace

std::string ImplantModelSpec::labelText() const {
    if (!sku.empty()) return sku;
    return "Ø" + num(diaMm) + "×" + num(lengthMm);
}

Json ImplantModelSpec::toJson() const {
    Json j = Json::makeObject();
    j.set("sku", Json::makeString(sku));
    j.set("vendor", Json::makeString(vendor));
    j.set("series", Json::makeString(series));
    j.set("connection", Json::makeString(connection));
    j.set("diaMm", Json::makeNumber(diaMm));
    j.set("lengthMm", Json::makeNumber(lengthMm));
    j.set("platformMm", Json::makeNumber(platformMm));
    j.set("abutmentMm", Json::makeNumber(abutmentMm));
    j.set("note", Json::makeString(note));
    // generic 是派生值（vendor 为空即通用库），读侧不采信、写侧带上便于人工审阅
    j.set("generic", Json::makeBool(generic()));
    return j;
}

ImplantModelSpec ImplantModelSpec::fromJson(const Json &j) {
    ImplantModelSpec s;
    s.sku = j.at("sku").asString();
    s.vendor = j.at("vendor").asString();
    s.series = j.at("series").asString();
    s.connection = j.at("connection").asString();
    s.diaMm = j.at("diaMm").asNumber(0.0);
    s.lengthMm = j.at("lengthMm").asNumber(0.0);
    s.platformMm = j.at("platformMm").asNumber(0.0);
    s.abutmentMm = j.at("abutmentMm").asNumber(0.0);
    s.note = j.at("note").asString();
    return s;
}

ImplantLibrary::ImplantLibrary() { loadBuiltin(); }

ImplantLibrary &ImplantLibrary::shared() {
    // C++11 起函数内 static 的初始化是线程安全的（magic statics），
    // 但这里不依赖该保证：调用方一律是 JNI 主线程。
    static ImplantLibrary instance;
    return instance;
}

void ImplantLibrary::clear() {
    specs_.clear();
    builtin_ = true;
}

void ImplantLibrary::loadBuiltin() {
    specs_.clear();
    for (size_t d = 0; d < kBuiltinDiaCount; ++d) {
        const double dia = kBuiltinDia[d];
        for (size_t q = 0; q < sizeof(kBuiltinLens[0]) / sizeof(double); ++q) {
            const double len = kBuiltinLens[d][q];
            if (len <= 0.0) continue;
            ImplantModelSpec s;
            char sku[32];
            snprintf(sku, sizeof(sku), "GEN-%s-%s", skuNum(dia).c_str(), skuNum(len).c_str());
            s.sku = sku;
            s.vendor = "";                       // 空 = 通用（见 generic() 的判定）
            s.series = "通用阶梯";
            s.connection = "";
            s.diaMm = dia;
            s.lengthMm = len;
            s.platformMm = 0.0;                  // 平台切换尺寸：内置库不提供
            s.abutmentMm = 0.0;
            s.note = "内置通用名义尺寸（非厂商目录）；平台/基台参数需由 implant_models.json 提供";
            specs_.push_back(s);
        }
    }
    builtin_ = true;
    LOGD("loadBuiltin: %zu generic models", specs_.size());
}

int ImplantLibrary::loadFromJson(const std::string &text, std::string *err) {
    bool ok = false;
    const Json root = Json::parse(text, &ok);
    if (!ok) {
        if (err) *err = "json 解析失败";
        LOGD("ImplantLibrary::loadFromJson: parse failed (len=%zu)", text.size());
        return -1;
    }
    const Json *arr = nullptr;
    if (root.isArray()) {
        arr = &root;
    } else if (root.isObject() && root.at("models").isArray()) {
        arr = &root.at("models");
    }
    if (arr == nullptr || arr->size() == 0) {
        if (err) *err = "没有 models 数组或数组为空";
        return -1;
    }
    std::vector<ImplantModelSpec> parsed;
    for (size_t i = 0; i < arr->size(); ++i) {
        const Json &j = (*arr)[i];
        if (!j.isObject()) continue;
        ImplantModelSpec s = ImplantModelSpec::fromJson(j);
        // 几何量非法的条目直接丢掉：Ø0 的型号会让 S-04 的"骨宽 >= 直径"恒真
        if (s.diaMm <= 0.0 || s.lengthMm <= 0.0) {
            LOGD("loadFromJson: skip #%zu (dia=%.2f len=%.2f)", i, s.diaMm, s.lengthMm);
            continue;
        }
        if (s.sku.empty()) {
            char sku[40];
            snprintf(sku, sizeof(sku), "%s-%s", s.vendor.empty() ? "GEN" : s.vendor.c_str(),
                     s.labelText().c_str());
            s.sku = sku;
        }
        parsed.push_back(s);
    }
    if (parsed.empty()) {
        if (err) *err = "所有条目的直径/长度字段都非法";
        return -1;
    }
    specs_.swap(parsed);
    builtin_ = false;
    if (err) err->clear();
    LOGD("loadFromJson: %zu models loaded (builtin=false, first sku=%s)",
         specs_.size(), specs_[0].sku.c_str());
    return (int) specs_.size();
}

bool ImplantLibrary::isGeneric() const {
    // 空库按"通用"处理：宁可得不出"这是厂商目录"的结论，也不给读者一个假采购依据
    if (specs_.empty()) return true;
    for (size_t i = 0; i < specs_.size(); ++i) {
        if (!specs_[i].vendor.empty()) return false;
    }
    return true;
}

std::string ImplantLibrary::dumpJson() const {
    Json root = Json::makeObject();
    root.set("kind", Json::makeString("cbctmeasure.implant-models"));
    // generic 取数据口径（见头文件）：随包的通用资产虽然来自 json，
    // 但它仍然不是厂商商品数据，报告文案要按"通用"来写。
    root.set("generic", Json::makeBool(isGeneric()));
    root.set("builtin", Json::makeBool(isBuiltin()));
    Json arr = Json::makeArray();
    for (size_t i = 0; i < specs_.size(); ++i) arr.push(specs_[i].toJson());
    root.set("models", arr);
    return root.dump();
}

const ImplantModelSpec *ImplantLibrary::bySku(const std::string &sku) const {
    if (sku.empty()) return nullptr;
    for (size_t i = 0; i < specs_.size(); ++i) {
        if (specs_[i].sku == sku) return &specs_[i];
    }
    return nullptr;
}

const ImplantModelSpec *ImplantLibrary::fitMatch(double maxDiaMm, double maxLenMm) const {
    const ImplantModelSpec *best = nullptr;
    for (size_t i = 0; i < specs_.size(); ++i) {
        const ImplantModelSpec &s = specs_[i];
        if (s.diaMm > maxDiaMm + 1e-9 || s.lengthMm > maxLenMm + 1e-9) continue;
        if (best == nullptr || s.lengthMm > best->lengthMm + 1e-9 ||
            (std::fabs(s.lengthMm - best->lengthMm) < 1e-9 && s.diaMm > best->diaMm + 1e-9)) {
            best = &specs_[i];
        }
    }
    return best;
}

const ImplantModelSpec *ImplantLibrary::nearestMatch(double diaMm, double lengthMm) const {
    const ImplantModelSpec *best = nullptr;
    double bestCost = 0.0;
    for (size_t i = 0; i < specs_.size(); ++i) {
        const ImplantModelSpec &s = specs_[i];
        // 归一化代价：直径 0.5mm 一档、长度 1.0mm 一档，两者相加。
        // 直径权重更高是刻意的 —— 放不进骨宽是硬失败，长度不足还能缩短。
        const double cost = std::fabs(s.diaMm - diaMm) / 0.5 + std::fabs(s.lengthMm - lengthMm) / 1.0;
        if (best == nullptr || cost < bestCost - 1e-12) {
            best = &specs_[i];
            bestCost = cost;
        }
    }
    return best;
}
