#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anvil::manager {

// A JSON value for the start-up benchmark (docs/spec/benchmark-v1.md): the
// start record the core writes, and the report the Manager builds, keeps and
// sends. Objects keep their members in order, so a document is written back
// in the order it was built.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(int value) : Json(static_cast<int64_t>(value)) {}
    Json(unsigned value) : Json(static_cast<int64_t>(value)) {}
    Json(int64_t value) : type_(Type::Number), integral_(true), integer_(value), number_(static_cast<double>(value)) {}
    Json(uint64_t value) : Json(static_cast<int64_t>(value)) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(const char* value) : Json(std::string(value)) {}

    static Json array() { return Json(Type::Array); }
    static Json object() { return Json(Type::Object); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }
    // A number written without a fraction or exponent that fits in int64.
    bool is_integer() const { return type_ == Type::Number && integral_; }

    bool as_bool() const { return bool_; }
    double as_number() const { return number_; }
    int64_t as_integer() const { return integer_; }
    const std::string& as_string() const { return string_; }
    const std::vector<Json>& items() const { return items_; }
    const std::vector<Member>& members() const { return members_; }

    // The member `key` of an object; nullptr when there is none (or this is
    // not an object).
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    // The member `key`, or a null value.
    const Json& operator[](std::string_view key) const;

    // Building: an array's next item, an object's member (replaced in place
    // when the key exists, appended otherwise).
    Json& push(Json value);
    Json& set(std::string key, Json value);

    bool operator==(const Json& other) const;

private:
    explicit Json(Type type) : type_(type) {}

    Type type_ = Type::Null;
    bool bool_ = false;
    bool integral_ = false;
    int64_t integer_ = 0;
    double number_ = 0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<Member> members_;
};

// RFC 8259 JSON, one value with nothing after it but white space. Depth is
// limited to 64. nullopt with `error` (and the byte offset in it) otherwise.
std::optional<Json> parse_json(std::string_view text, std::string& error);

// The value as UTF-8 JSON. Numbers are the shortest text that reads back as
// the same double; NaN and infinity, which JSON cannot hold, are written as
// null. With `pretty`, two spaces per level; an array or object of scalars
// that fits in 80 characters stays on one line. Ends with a newline when
// pretty.
std::string write_json(const Json& value, bool pretty = false);

}  // namespace anvil::manager
