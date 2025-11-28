// Simple bencode representation and parser.
#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bencode {
    struct Value;

    using List = std::vector<Value>;
    using Dict = std::map<std::string, Value, std::less<>>;
    using Variant = std::variant<int64_t, std::string, List, Dict>;

    struct Value {
        Variant data;
    };

    class Parser {
    public:
        explicit Parser(std::string input, std::optional<std::string> track_key = std::nullopt);

        Value parse();

        std::optional<std::pair<size_t, size_t>> tracked_span() const { return tracked_span_; }

    private:
        Value parse_value(size_t& pos);
        Value parse_int(size_t& pos);
        Value parse_string(size_t& pos);
        Value parse_list(size_t& pos);
        Value parse_dict(size_t& pos);

        char peek(size_t pos) const;
        void expect(char c, size_t pos) const;
        [[noreturn]] void fail(std::string_view message, size_t pos) const;

        std::string input_;
        std::optional<std::string> track_key_;
        std::optional<std::pair<size_t, size_t>> tracked_span_;
    };

    int64_t as_int(const Value& v);
    const std::string& as_string(const Value& v);
    const List& as_list(const Value& v);
    const Dict& as_dict(const Value& v);
    const Value& require_field(const Dict& dict, std::string_view key);
    const Value* find_field(const Dict& dict, std::string_view key);
} // namespace bencode
