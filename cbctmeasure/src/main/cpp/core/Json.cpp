// 极小 JSON 读写器实现。
//
// 设计约束（与 include/Json.h 顶部注释一致）：
//   - 全链路不抛异常：解析失败统一降级为 null，业务层自行决定回退；
//   - 数字以 %.12g 输出：相对误差 < 1e-12，远小于 PRD 的 0.1mm 精度门槛，
//     同时避免 %.17g 产生 "0.10000000000000001" 这类不可读文本；
//   - 键序按插入顺序保留，导出文本逐字节稳定，便于 diff 与回归比对。

#include "include/Json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

const Json Json::kNull;

Json Json::makeNull() { return Json(); }

Json Json::makeBool(bool b) {
    Json j;
    j.type_ = JT_BOOL;
    j.bool_ = b;
    return j;
}

Json Json::makeNumber(double d) {
    Json j;
    j.type_ = JT_NUM;
    j.num_ = d;
    return j;
}

Json Json::makeString(const std::string &s) {
    Json j;
    j.type_ = JT_STR;
    j.str_ = s;
    return j;
}

Json Json::makeArray() {
    Json j;
    j.type_ = JT_ARR;
    return j;
}

Json Json::makeObject() {
    Json j;
    j.type_ = JT_OBJ;
    return j;
}

bool Json::asBool(bool fallback) const {
    if (type_ == JT_BOOL) return bool_;
    if (type_ == JT_NUM) return num_ != 0.0;
    return fallback;
}

double Json::asNumber(double fallback) const {
    return type_ == JT_NUM ? num_ : fallback;
}

int Json::asInt(int fallback) const {
    if (type_ != JT_NUM) return fallback;
    return (int) (num_ >= 0 ? num_ + 0.5 : num_ - 0.5);
}

std::string Json::asString(const std::string &fallback) const {
    return type_ == JT_STR ? str_ : fallback;
}

const Json &Json::at(const std::string &key) const {
    if (type_ != JT_OBJ) return kNull;
    for (size_t i = 0; i < obj_.size(); ++i) {
        if (obj_[i].first == key) return obj_[i].second;
    }
    return kNull;
}

bool Json::has(const std::string &key) const {
    if (type_ != JT_OBJ) return false;
    for (size_t i = 0; i < obj_.size(); ++i) {
        if (obj_[i].first == key) return true;
    }
    return false;
}

void Json::set(const std::string &key, const Json &value) {
    if (type_ != JT_OBJ) {
        type_ = JT_OBJ;
        obj_.clear();
    }
    for (size_t i = 0; i < obj_.size(); ++i) {
        if (obj_[i].first == key) {
            obj_[i].second = value;
            return;
        }
    }
    obj_.push_back(std::make_pair(key, value));
}

size_t Json::size() const {
    if (type_ == JT_ARR) return arr_.size();
    if (type_ == JT_OBJ) return obj_.size();
    return 0;
}

const Json &Json::operator[](size_t i) const {
    if (type_ == JT_ARR && i < arr_.size()) return arr_[i];
    return kNull;
}

void Json::push(const Json &value) {
    if (type_ != JT_ARR) {
        type_ = JT_ARR;
        arr_.clear();
    }
    arr_.push_back(value);
}

std::string Json::escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char) s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                // 控制字符按 \u00XX 转义；>=0x20 的字节（含 UTF-8 多字节）原样输出
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char) c;
                }
        }
    }
    return out;
}

void Json::dumpScalar(std::string &out) const {
    char buf[64];
    switch (type_) {
        case JT_NULL: out += "null"; break;
        case JT_BOOL: out += bool_ ? "true" : "false"; break;
        case JT_NUM:
            if (num_ != num_ || num_ > 1e308 || num_ < -1e308) {
                // NaN/Inf 不是合法 JSON，落盘为 null 以保证文件可再解析
                out += "null";
            } else {
                snprintf(buf, sizeof(buf), "%.12g", num_);
                out += buf;
            }
            break;
        case JT_STR:
            out += '"';
            out += escape(str_);
            out += '"';
            break;
        default:
            out += "null";
            break;
    }
}

void Json::dumpInto(std::string &out, bool pretty, int indent) const {
    if (type_ == JT_ARR) {
        if (arr_.empty()) { out += "[]"; return; }
        out += pretty ? "[\n" : "[";
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (pretty) out.append((size_t) (indent + 1) * 2, ' ');
            arr_[i].dumpInto(out, pretty, indent + 1);
            out += (i + 1 < arr_.size()) ? "," : (pretty ? "\n" : "");
        }
        if (pretty) out.append((size_t) indent * 2, ' ');
        out += "]";
        return;
    }
    if (type_ == JT_OBJ) {
        if (obj_.empty()) { out += "{}"; return; }
        out += pretty ? "{\n" : "{";
        for (size_t i = 0; i < obj_.size(); ++i) {
            if (pretty) out.append((size_t) (indent + 1) * 2, ' ');
            out += '"';
            out += escape(obj_[i].first);
            out += pretty ? "\": " : "\":";
            obj_[i].second.dumpInto(out, pretty, indent + 1);
            out += (i + 1 < obj_.size()) ? "," : (pretty ? "\n" : "");
        }
        if (pretty) out.append((size_t) indent * 2, ' ');
        out += "}";
        return;
    }
    dumpScalar(out);
}

std::string Json::dump(bool pretty) const {
    std::string out;
    dumpInto(out, pretty, 0);
    return out;
}

// =============================================================================
// 解析（递归下降）
// =============================================================================
namespace {

    struct Cursor {
        const char *p;
        const char *end;
        bool ok;

        void skipWs() {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        }

        Json parseValue();

        Json parseString() {
            if (p >= end || *p != '"') { ok = false; return Json::makeNull(); }
            ++p;
            std::string s;
            while (p < end) {
                const char c = *p++;
                if (c == '"') return Json::makeString(s);
                if (c != '\\') { s += c; continue; }
                if (p >= end) break;
                const char e = *p++;
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        if (end - p < 4) { ok = false; return Json::makeNull(); }
                        char digits[5] = {p[0], p[1], p[2], p[3], 0};
                        p += 4;
                        const long cp = strtol(digits, nullptr, 16);
                        // UTF-8 编码（本模块不写非 BMP，代理对原样透传）
                        if (cp < 0x80) s += (char) cp;
                        else if (cp < 0x800) {
                            s += (char) (0xC0 | (cp >> 6));
                            s += (char) (0x80 | (cp & 0x3F));
                        } else {
                            s += (char) (0xE0 | (cp >> 12));
                            s += (char) (0x80 | ((cp >> 6) & 0x3F));
                            s += (char) (0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: ok = false; return Json::makeNull();
                }
            }
            ok = false;   // 字符串未闭合
            return Json::makeNull();
        }

        Json parseNumber() {
            const char *start = p;
            if (p < end && (*p == '-' || *p == '+')) ++p;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' ||
                               *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) ++p;
            if (p == start) { ok = false; return Json::makeNull(); }
            std::string num(start, (size_t) (p - start));
            return Json::makeNumber(strtod(num.c_str(), nullptr));
        }

        Json parseArray() {
            ++p;   // '['
            Json arr = Json::makeArray();
            skipWs();
            if (p < end && *p == ']') { ++p; return arr; }
            while (p < end) {
                arr.push(parseValue());
                if (!ok) return arr;
                skipWs();
                if (p < end && *p == ',') { ++p; skipWs(); continue; }
                if (p < end && *p == ']') { ++p; return arr; }
                ok = false;
                return arr;
            }
            ok = false;
            return arr;
        }

        Json parseObject() {
            ++p;   // '{'
            Json obj = Json::makeObject();
            skipWs();
            if (p < end && *p == '}') { ++p; return obj; }
            while (p < end) {
                skipWs();
                const Json key = parseString();
                if (!ok) return obj;
                skipWs();
                if (p >= end || *p != ':') { ok = false; return obj; }
                ++p;
                skipWs();
                obj.set(key.asString(), parseValue());
                if (!ok) return obj;
                skipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return obj; }
                ok = false;
                return obj;
            }
            ok = false;
            return obj;
        }
    };

    Json Cursor::parseValue() {
        skipWs();
        if (p >= end) { ok = false; return Json::makeNull(); }
        const char c = *p;
        if (c == '"') return parseString();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't') {
            if (end - p >= 4 && strncmp(p, "true", 4) == 0) { p += 4; return Json::makeBool(true); }
            ok = false; return Json::makeNull();
        }
        if (c == 'f') {
            if (end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; return Json::makeBool(false); }
            ok = false; return Json::makeNull();
        }
        if (c == 'n') {
            if (end - p >= 4 && strncmp(p, "null", 4) == 0) { p += 4; return Json::makeNull(); }
            ok = false; return Json::makeNull();
        }
        return parseNumber();
    }

}   // namespace

Json Json::parse(const std::string &text, bool *okOut) {
    Cursor cur = {text.c_str(), text.c_str() + text.size(), true};
    Json v = cur.parseValue();
    if (cur.ok) {
        cur.skipWs();
        if (cur.p != cur.end) cur.ok = false;   // 尾部多余内容：判定为坏文件
    }
    if (okOut) *okOut = cur.ok;
    return cur.ok ? v : Json::makeNull();
}
