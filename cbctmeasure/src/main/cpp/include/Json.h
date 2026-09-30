#ifndef DCMTKDEMO_JSON_H
#define DCMTKDEMO_JSON_H

#include <map>
#include <string>
#include <vector>

/**
 * 极小 JSON 读写器（业务逻辑层，纯 C++）。
 *
 * 为什么不用第三方库：PRD 4.1 要求"不引入新的第三方依赖（Phase 1）"，
 * 而持久化只需要"对象/数组/字符串/数字/布尔/null"这一层能力，
 * 自己实现 ~300 行即可覆盖，且能主机侧编译单测。
 *
 * 能力边界（明确不支持，避免误用）：
 *   - 不做 schema 校验；解析失败返回 null 并由调用方决定降级策略
 *   - 不支持 \uXXXX 之外的扩展；\u 只处理 BMP（本模块仅写 ASCII/UTF-8 原文）
 *   - 数字一律按 double 存储；整型语义由取值方四舍五入
 *
 * 键序：对象内部保留插入顺序（vector<pair>），保证导出的 JSON 逐字节稳定，
 * 便于 A/B 对比与 diff 审阅。
 */
class Json {
public:
    enum Type { JT_NULL, JT_BOOL, JT_NUM, JT_STR, JT_ARR, JT_OBJ };

    Json() : type_(JT_NULL) {}

    static Json makeNull();
    static Json makeBool(bool b);
    static Json makeNumber(double d);
    static Json makeString(const std::string &s);
    static Json makeArray();
    static Json makeObject();

    Type type() const { return type_; }
    bool isNull() const { return type_ == JT_NULL; }
    bool isObject() const { return type_ == JT_OBJ; }
    bool isArray() const { return type_ == JT_ARR; }

    // ---- 取值（类型不符时返回默认值，绝不抛异常）----
    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    int asInt(int fallback = 0) const;
    std::string asString(const std::string &fallback = std::string()) const;

    // ---- 对象读写 ----
    /** 读字段；不存在返回 null Json（可安全继续链式取值） */
    const Json &at(const std::string &key) const;
    bool has(const std::string &key) const;
    void set(const std::string &key, const Json &value);

    // ---- 数组读写 ----
    size_t size() const;
    const Json &operator[](size_t i) const;
    void push(const Json &value);

    // ---- 序列化 ----
    /** pretty=true 时两空格缩进（人工审阅用），false 为紧凑单行（落盘用） */
    std::string dump(bool pretty = false) const;

    /**
     * 解析。失败时返回 null Json 并把 ok 置 false；
     * 成功但尾部有多余内容视为失败（拒绝半截文件当成功）。
     */
    static Json parse(const std::string &text, bool *ok = nullptr);

    /** 字符串转义（供报告/SR 文本字段复用） */
    static std::string escape(const std::string &s);

private:
    void dumpInto(std::string &out, bool pretty, int indent) const;
    void dumpScalar(std::string &out) const;

    Type type_;
    bool bool_;
    double num_;
    std::string str_;
    std::vector<Json> arr_;
    // 插入有序的键值对容器（不用 std::map，避免键序被字典化改写）
    std::vector<std::pair<std::string, Json> > obj_;

    static const Json kNull;
};

#endif // DCMTKDEMO_JSON_H
