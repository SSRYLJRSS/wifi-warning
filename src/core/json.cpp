#include "core/json.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace ww {

static const JsonValue::Array emptyArray;
static const JsonValue::Object emptyObject;

JsonValue::JsonValue() : data(nullptr) {}
JsonValue::JsonValue(std::nullptr_t) : data(nullptr) {}
JsonValue::JsonValue(bool value) : data(value) {}
JsonValue::JsonValue(double value) : data(value) {}
JsonValue::JsonValue(int value) : data(static_cast<double>(value)) {}
JsonValue::JsonValue(std::string value) : data(std::move(value)) {}
JsonValue::JsonValue(const char* value) : data(std::string(value)) {}
JsonValue::JsonValue(Array value) : data(std::move(value)) {}
JsonValue::JsonValue(Object value) : data(std::move(value)) {}

bool JsonValue::isNull() const { return std::holds_alternative<std::nullptr_t>(data); }
bool JsonValue::isBool() const { return std::holds_alternative<bool>(data); }
bool JsonValue::isNumber() const { return std::holds_alternative<double>(data); }
bool JsonValue::isString() const { return std::holds_alternative<std::string>(data); }
bool JsonValue::isArray() const { return std::holds_alternative<Array>(data); }
bool JsonValue::isObject() const { return std::holds_alternative<Object>(data); }
bool JsonValue::asBool(bool fallback) const { return isBool() ? std::get<bool>(data) : fallback; }
double JsonValue::asNumber(double fallback) const { return isNumber() ? std::get<double>(data) : fallback; }
std::string JsonValue::asString(const std::string& fallback) const { return isString() ? std::get<std::string>(data) : fallback; }
const JsonValue::Array& JsonValue::asArray() const { return isArray() ? std::get<Array>(data) : emptyArray; }
const JsonValue::Object& JsonValue::asObject() const { return isObject() ? std::get<Object>(data) : emptyObject; }

const JsonValue* JsonValue::get(const std::string& key) const {
    if (!isObject()) return nullptr;
    const auto& object = std::get<Object>(data);
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    std::optional<JsonValue> parse(std::string* error) {
        skipWhitespace();
        auto value = parseValue();
        if (!value) return fail(error);
        skipWhitespace();
        if (pos_ != text_.size()) {
            message_ = "Unexpected trailing characters";
            return fail(error);
        }
        return value;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;
    int depth_ = 0;
    std::string message_;

    std::optional<JsonValue> fail(std::string* error) {
        if (error) {
            std::ostringstream ss;
            ss << message_ << " at byte " << pos_;
            *error = ss.str();
        }
        return std::nullopt;
    }

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    bool consume(char expected) {
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::optional<JsonValue> parseValue() {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            message_ = "Unexpected end of input";
            return std::nullopt;
        }
        char c = text_[pos_];
        if (c == '"') return parseString();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return JsonValue(true);
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return JsonValue(false);
        }
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return JsonValue(nullptr);
        }
        message_ = "Unexpected token";
        return std::nullopt;
    }

    std::optional<JsonValue> parseString() {
        if (text_[pos_] != '"') {
            message_ = "Expected string";
            return std::nullopt;
        }
        ++pos_;
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return JsonValue(out);
            if (c == '\\') {
                if (pos_ >= text_.size()) break;
                char esc = text_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        if (pos_ + 4 <= text_.size()) {
                            out += "\\u";
                            out.append(text_, pos_, 4);
                            pos_ += 4;
                        }
                        break;
                    default:
                        out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        message_ = "Unterminated string";
        return std::nullopt;
    }

    std::optional<JsonValue> parseNumber() {
        size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        try {
            return JsonValue(std::stod(text_.substr(start, pos_ - start)));
        } catch (...) {
            message_ = "Invalid number";
            return std::nullopt;
        }
    }

    std::optional<JsonValue> parseArray() {
        consume('[');
        if (++depth_ > 64) { --depth_; message_ = "JSON nesting too deep"; return std::nullopt; }
        JsonValue::Array array;
        skipWhitespace();
        if (consume(']')) { --depth_; return JsonValue(array); }
        while (true) {
            auto value = parseValue();
            if (!value) { --depth_; return std::nullopt; }
            array.push_back(*value);
            if (consume(']')) { --depth_; return JsonValue(array); }
            if (!consume(',')) {
                --depth_;
                message_ = "Expected comma in array";
                return std::nullopt;
            }
        }
    }

    std::optional<JsonValue> parseObject() {
        consume('{');
        if (++depth_ > 64) { --depth_; message_ = "JSON nesting too deep"; return std::nullopt; }
        JsonValue::Object object;
        skipWhitespace();
        if (consume('}')) { --depth_; return JsonValue(object); }
        while (true) {
            skipWhitespace();
            auto key = parseString();
            if (!key) { --depth_; return std::nullopt; }
            if (!consume(':')) {
                --depth_;
                message_ = "Expected colon in object";
                return std::nullopt;
            }
            auto value = parseValue();
            if (!value) { --depth_; return std::nullopt; }
            object[key->asString()] = *value;
            if (consume('}')) { --depth_; return JsonValue(object); }
            if (!consume(',')) {
                --depth_;
                message_ = "Expected comma in object";
                return std::nullopt;
            }
        }
    }
};

std::optional<JsonValue> parseJson(const std::string& text, std::string* error) {
    return Parser(text).parse(error);
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream ss;
    for (unsigned char c : value) {
        switch (c) {
            case '"': ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default:
                if (c < 0x20) {
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                } else {
                    ss << c;
                }
        }
    }
    return ss.str();
}

static void stringifyInto(const JsonValue& value, std::ostringstream& ss, int indent, int depth) {
    auto pad = [&](int level) {
        for (int i = 0; i < level * indent; ++i) ss << ' ';
    };
    const bool pretty = indent > 0;

    if (value.isNull()) {
        ss << "null";
    } else if (value.isBool()) {
        ss << (value.asBool() ? "true" : "false");
    } else if (value.isNumber()) {
        double n = value.asNumber();
        if (n == static_cast<long long>(n)) ss << static_cast<long long>(n);
        else ss << n;
    } else if (value.isString()) {
        ss << '"' << jsonEscape(value.asString()) << '"';
    } else if (value.isArray()) {
        const auto& array = value.asArray();
        ss << '[';
        if (pretty && !array.empty()) ss << '\n';
        for (size_t i = 0; i < array.size(); ++i) {
            if (pretty) pad(depth + 1);
            stringifyInto(array[i], ss, indent, depth + 1);
            if (i + 1 < array.size()) ss << ',';
            if (pretty) ss << '\n';
        }
        if (pretty && !array.empty()) pad(depth);
        ss << ']';
    } else {
        const auto& object = value.asObject();
        ss << '{';
        if (pretty && !object.empty()) ss << '\n';
        size_t i = 0;
        for (const auto& [key, child] : object) {
            if (pretty) pad(depth + 1);
            ss << '"' << jsonEscape(key) << "\":";
            if (pretty) ss << ' ';
            stringifyInto(child, ss, indent, depth + 1);
            if (++i < object.size()) ss << ',';
            if (pretty) ss << '\n';
        }
        if (pretty && !object.empty()) pad(depth);
        ss << '}';
    }
}

std::string stringifyJson(const JsonValue& value, int indent) {
    std::ostringstream ss;
    stringifyInto(value, ss, indent, 0);
    ss << '\n';
    return ss.str();
}

}
