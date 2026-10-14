// Minimal, dependency-free JSON reader. Enough for the credentials file and
// the usage endpoint response; intentionally locale-independent (no strtod).
#pragma once
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace mini_json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;       // array elements, or object values
    std::vector<std::string> keys;  // object keys (parallel to items)

    const Value* Get(const char* key) const {
        if (type != Object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }
    bool IsNumber() const { return type == Number; }
    bool IsString() const { return type == String; }
    bool IsObject() const { return type == Object; }
};

class Parser {
public:
    explicit Parser(const std::string& text) : p_(text.data()), end_(text.data() + text.size()) {}

    bool Parse(Value& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return p_ == end_;
    }

private:
    static const int kMaxDepth = 64;
    const char* p_;
    const char* end_;

    bool IsDigit() const { return p_ < end_ && *p_ >= '0' && *p_ <= '9'; }

    void SkipWs() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }

    bool Literal(const char* lit) {
        size_t n = std::strlen(lit);
        if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, lit, n) != 0) return false;
        p_ += n;
        return true;
    }

    bool ParseValue(Value& v, int depth) {
        if (depth > kMaxDepth || p_ >= end_) return false;
        switch (*p_) {
        case '{': return ParseObject(v, depth);
        case '[': return ParseArray(v, depth);
        case '"': v.type = Value::String; return ParseString(v.string);
        case 't': v.type = Value::Bool; v.boolean = true; return Literal("true");
        case 'f': v.type = Value::Bool; v.boolean = false; return Literal("false");
        case 'n': v.type = Value::Null; return Literal("null");
        default: return ParseNumber(v);
        }
    }

    bool ParseNumber(Value& v) {
        bool negative = false;
        if (p_ < end_ && *p_ == '-') { negative = true; ++p_; }
        if (!IsDigit()) return false;
        double value = 0.0;
        while (IsDigit()) value = value * 10.0 + (*p_++ - '0');
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            if (!IsDigit()) return false;
            double scale = 0.1;
            while (IsDigit()) { value += (*p_++ - '0') * scale; scale *= 0.1; }
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            bool exp_negative = false;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) { exp_negative = (*p_ == '-'); ++p_; }
            if (!IsDigit()) return false;
            int exponent = 0;
            while (IsDigit()) { if (exponent < 400) exponent = exponent * 10 + (*p_ - '0'); ++p_; }
            value *= std::pow(10.0, exp_negative ? -exponent : exponent);
        }
        v.type = Value::Number;
        v.number = negative ? -value : value;
        return true;
    }

    static int HexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool ParseHex4(unsigned& cp) {
        if (end_ - p_ < 4) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            int h = HexDigit(p_[i]);
            if (h < 0) return false;
            cp = cp * 16 + static_cast<unsigned>(h);
        }
        p_ += 4;
        return true;
    }

    static void AppendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool ParseString(std::string& out) {
        ++p_;  // opening quote
        out.clear();
        while (p_ < end_) {
            char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p_ >= end_) return false;
            char e = *p_++;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!ParseHex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    const char* save = p_;
                    p_ += 2;
                    unsigned lo;
                    if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        p_ = save;
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ParseArray(Value& v, int depth) {
        ++p_;
        v.type = Value::Array;
        SkipWs();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        for (;;) {
            SkipWs();
            v.items.emplace_back();
            if (!ParseValue(v.items.back(), depth + 1)) return false;
            SkipWs();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return true; }
            return false;
        }
    }

    bool ParseObject(Value& v, int depth) {
        ++p_;
        v.type = Value::Object;
        SkipWs();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        for (;;) {
            SkipWs();
            if (p_ >= end_ || *p_ != '"') return false;
            v.keys.emplace_back();
            if (!ParseString(v.keys.back())) return false;
            SkipWs();
            if (p_ >= end_ || *p_ != ':') return false;
            ++p_;
            SkipWs();
            v.items.emplace_back();
            if (!ParseValue(v.items.back(), depth + 1)) return false;
            SkipWs();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return true; }
            return false;
        }
    }
};

}  // namespace mini_json
