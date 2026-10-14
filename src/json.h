// json.h — the little JSON the control channel needs: parse what the
// coordinator sends (flat objects plus one nested "config"), build what we send.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Object, Array };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<std::pair<std::string, Value>> obj;
    std::vector<Value> arr;

    const Value* get(const std::string& key) const;
    std::string get_string(const std::string& key, const std::string& def = "") const;
    double get_number(const std::string& key, double def = 0) const;
    bool get_bool(const std::string& key, bool def = false) const;
    bool has(const std::string& key) const { return get(key) != nullptr; }
};

bool parse(const std::string& text, Value& out);

// A JSON string literal, escaped.
std::string quote(const std::string& s);

// Builds one flat object: {"a":"x","b":1,"c":true}
class Writer {
public:
    Writer& str(const char* key, const std::string& value);
    Writer& num(const char* key, long long value);
    Writer& boolean(const char* key, bool value);
    std::string done() const { return out_ + "}"; }

private:
    void key(const char* k);
    std::string out_ = "{";
    bool first_ = true;
};

}  // namespace json
