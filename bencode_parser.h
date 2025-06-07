#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <memory>

class BencodeParser {
public:
    struct BencodeValue;

    using BencodeString = std::string;
    using BencodeInteger = int64_t;
    using BencodeList = std::vector<std::shared_ptr<BencodeValue>>;
    using BencodeDict = std::map<std::string, std::shared_ptr<BencodeValue>>;

    struct BencodeValue {
        enum Type { STRING, INTEGER, LIST, DICT };
        Type type_;
        BencodeString string_val_;
        BencodeInteger int_val_;
        BencodeList list_val_;
        BencodeDict dict_val_;
        explicit BencodeValue(const BencodeString& s);
        explicit BencodeValue(BencodeInteger i);
        explicit BencodeValue(const BencodeList& l);
        explicit BencodeValue(const BencodeDict& d);
        bool is_string() const;
        bool is_integer() const;
        bool is_list() const;
        bool is_dict() const;
        const BencodeString& as_string() const;
        BencodeInteger as_integer() const;
        const BencodeList& as_list() const;
        const BencodeDict& as_dict() const;
    };
    static std::shared_ptr<BencodeValue> parse(const std::string& data);
    static std::shared_ptr<BencodeValue> parse(const std::string& data, size_t& pos);

private:
    static std::shared_ptr<BencodeValue> parse_value(const std::string& data, size_t& pos);
    static std::shared_ptr<BencodeValue> parse_integer(const std::string& data, size_t& pos);
    static std::shared_ptr<BencodeValue> parse_string(const std::string& data, size_t& pos);
    static std::shared_ptr<BencodeValue> parse_list(const std::string& data, size_t& pos);
    static std::shared_ptr<BencodeValue> parse_dictionary(const std::string& data, size_t& pos);
};
using BencodeValue = BencodeParser::BencodeValue;
using BencodeDict = BencodeParser::BencodeDict;
using BencodeList = BencodeParser::BencodeList;


class BencodeEncoder {
public:
    static std::string encode(const BencodeValue& value);
    static std::vector<uint8_t> encode_bytes(const BencodeValue& value);

private:
    static std::string encode_string(const std::string& str);
    static std::string encode_integer(int64_t val);
    static std::string encode_list(const BencodeList& list);
    static std::string encode_dict(const BencodeDict& dict);
};