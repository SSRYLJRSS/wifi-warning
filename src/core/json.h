#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ww {

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Data = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Data data;

    JsonValue();
    JsonValue(std::nullptr_t);
    JsonValue(bool value);
    JsonValue(double value);
    JsonValue(int value);
    JsonValue(std::string value);
    JsonValue(const char* value);
    JsonValue(Array value);
    JsonValue(Object value);

    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    std::string asString(const std::string& fallback = "") const;
    const Array& asArray() const;
    const Object& asObject() const;

    const JsonValue* get(const std::string& key) const;
};

std::optional<JsonValue> parseJson(const std::string& text, std::string* error = nullptr);
std::string stringifyJson(const JsonValue& value, int indent = 2);
std::string jsonEscape(const std::string& value);

}
