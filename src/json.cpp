#include "json.h"

#include <cstdlib>
#include <cstring>

namespace json {

const Value* Value::get(const std::string& key) const {
    if (type != Object) return nullptr;
    for (const auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string Value::get_string(const std::string& key, const std::string& def) const {
    const Value* v = get(key);
    return (v && v->type == String) ? v->str : def;
}

double Value::get_number(const std::string& key, double def) const {
    const Value* v = get(key);
    if (!v) return def;
    if (v->type == Number) return v->num;
    if (v->type == Bool) return v->b ? 1 : 0;
    return def;
}

bool Value::get_bool(const std::string& key, bool def) const {
    const Value* v = get(key);
    if (!v) return def;
    if (v->type == Bool) return v->b;
    if (v->type == Number) return v->num != 0;
    return def;
}

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    explicit Parser(const std::string& text) : s(text) {}

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }

    bool lit(const char* word) {
        size_t n = strlen(word);
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }

    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    bool hex4(unsigned& out) {
        if (i + 4 > s.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
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
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool value(Value& out) {
        if (++depth > 32) return false;
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        bool ok = false;
        if (c == '{') {
            out.type = Value::Object;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; ok = true; }
            else {
                while (true) {
                    ws();
                    std::string key;
                    if (!string(key)) break;
                    ws();
                    if (i >= s.size() || s[i] != ':') break;
                    ++i;
                    Value v;
                    if (!value(v)) break;
                    out.obj.emplace_back(std::move(key), std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; ok = true; }
                    break;
                }
            }
        } else if (c == '[') {
            out.type = Value::Array;
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; ok = true; }
            else {
                while (true) {
                    Value v;
                    if (!value(v)) break;
                    out.arr.push_back(std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; ok = true; }
                    break;
                }
            }
        } else if (c == '"') {
            out.type = Value::String;
            ok = string(out.str);
        } else if (c == 't') { out.type = Value::Bool; out.b = true; ok = lit("true"); }
        else if (c == 'f') { out.type = Value::Bool; out.b = false; ok = lit("false"); }
        else if (c == 'n') { out.type = Value::Null; ok = lit("null"); }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            out.type = Value::Number;
            const char* start = s.c_str() + i;
            char* end = nullptr;
            out.num = strtod(start, &end);
            ok = end && end > start;
            if (ok) i += (size_t)(end - start);
        }
        --depth;
        return ok;
    }
};

}  // namespace

bool parse(const std::string& text, Value& out) {
    Parser p(text);
    if (!p.value(out)) return false;
    p.ws();
    return p.i == text.size();
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out + "\"";
}

void Writer::key(const char* k) {
    if (!first_) out_ += ",";
    first_ = false;
    out_ += quote(k);
    out_ += ":";
}

Writer& Writer::str(const char* k, const std::string& value) { key(k); out_ += quote(value); return *this; }
Writer& Writer::num(const char* k, long long value) { key(k); out_ += std::to_string(value); return *this; }
Writer& Writer::boolean(const char* k, bool value) { key(k); out_ += value ? "true" : "false"; return *this; }

}  // namespace json
