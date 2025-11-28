#include "bencode.h"

#include <cctype>
#include <charconv>
#include <sstream>

namespace bencode {

namespace {

int64_t parse_int64(std::string_view sv) {
    int64_t value{};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc() || ptr != sv.data() + sv.size()) {
        throw std::runtime_error("Invalid integer: " + std::string(sv));
    }
    return value;
}

}  // namespace

Parser::Parser(std::string input, std::optional<std::string> track_key)
    : input_(std::move(input)), track_key_(std::move(track_key)) {}

Value Parser::parse() {
    size_t pos = 0;
    Value v = parse_value(pos);
    if (pos != input_.size()) {
        fail("Trailing data after valid bencode", pos);
    }
    return v;
}

char Parser::peek(size_t pos) const {
    if (pos >= input_.size()) {
        fail("Unexpected end of input", pos);
    }
    return input_[pos];
}

void Parser::expect(char c, size_t pos) const {
    if (peek(pos) != c) {
        fail(std::string("Expected '") + c + "'", pos);
    }
}

[[noreturn]] void Parser::fail(std::string_view message, size_t pos) const {
    std::ostringstream oss;
    oss << "Bencode parse error at offset " << pos << ": " << message;
    throw std::runtime_error(oss.str());
}

Value Parser::parse_value(size_t& pos) {
    char c = peek(pos);
    if (c == 'i') return parse_int(pos);
    if (c == 'l') return parse_list(pos);
    if (c == 'd') return parse_dict(pos);
    if (std::isdigit(static_cast<unsigned char>(c))) return parse_string(pos);
    fail("Unexpected token", pos);
}

Value Parser::parse_int(size_t& pos) {
    expect('i', pos);
    size_t start = ++pos;
    if (peek(pos) == '-') ++pos;
    if (!std::isdigit(static_cast<unsigned char>(peek(pos)))) {
        fail("Integer must have digits", pos);
    }
    while (std::isdigit(static_cast<unsigned char>(peek(pos)))) ++pos;
    expect('e', pos);
    size_t end = pos++;
    int64_t val = parse_int64(std::string_view(input_).substr(start, end - start));
    return Value{val};
}

Value Parser::parse_string(size_t& pos) {
    size_t len_start = pos;
    while (std::isdigit(static_cast<unsigned char>(peek(pos)))) {
        ++pos;
    }
    if (pos == len_start) {
        fail("String length expected", pos);
    }
    if (peek(pos) != ':') {
        fail("Missing ':' after string length", pos);
    }
    int64_t len = parse_int64(std::string_view(input_).substr(len_start, pos - len_start));
    if (len < 0) {
        fail("Negative string length", pos);
    }
    ++pos;
    size_t str_start = pos;
    size_t str_end = pos + static_cast<size_t>(len);
    if (str_end > input_.size()) {
        fail("String extends past end of input", pos);
    }
    pos = str_end;
    return Value{input_.substr(str_start, static_cast<size_t>(len))};
}

Value Parser::parse_list(size_t& pos) {
    expect('l', pos);
    ++pos;
    List out;
    while (peek(pos) != 'e') {
        out.push_back(parse_value(pos));
    }
    ++pos;  // consume 'e'
    return Value{out};
}

Value Parser::parse_dict(size_t& pos) {
    expect('d', pos);
    ++pos;
    Dict out;
    while (peek(pos) != 'e') {
        Value key_v = parse_string(pos);
        const std::string& key = std::get<std::string>(key_v.data);
        size_t value_start = pos;
        Value value = parse_value(pos);
        if (track_key_ && *track_key_ == key && !tracked_span_) {
            size_t value_end = pos;
            tracked_span_ = std::make_pair(value_start, value_end - value_start);
        }
        out.emplace(key, std::move(value));
    }
    ++pos;  // consume 'e'
    return Value{out};
}

int64_t as_int(const Value& v) {
    if (auto p = std::get_if<int64_t>(&v.data)) return *p;
    throw std::runtime_error("Value is not an integer");
}

const std::string& as_string(const Value& v) {
    if (auto p = std::get_if<std::string>(&v.data)) return *p;
    throw std::runtime_error("Value is not a string");
}

const List& as_list(const Value& v) {
    if (auto p = std::get_if<List>(&v.data)) return *p;
    throw std::runtime_error("Value is not a list");
}

const Dict& as_dict(const Value& v) {
    if (auto p = std::get_if<Dict>(&v.data)) return *p;
    throw std::runtime_error("Value is not a dict");
}

const Value& require_field(const Dict& dict, std::string_view key) {
    if (auto it = dict.find(key); it != dict.end()) {
        return it->second;
    }
    throw std::runtime_error("Missing required field: " + std::string(key));
}

const Value* find_field(const Dict& dict, std::string_view key) {
    if (auto it = dict.find(key); it != dict.end()) return &it->second;
    return nullptr;
}

}  // namespace bencode
