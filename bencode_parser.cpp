#include "bencode_parser.h"
#include <cctype>
#include <algorithm>

BencodeParser::BencodeValue::BencodeValue(const BencodeString& s)
    : type_(STRING), string_val_(s) {}

BencodeParser::BencodeValue::BencodeValue(BencodeInteger i)
    : type_(INTEGER), int_val_(i) {}

BencodeParser::BencodeValue::BencodeValue(const BencodeList& l)
    : type_(LIST), list_val_(l) {}

BencodeParser::BencodeValue::BencodeValue(const BencodeDict& d)
    : type_(DICT), dict_val_(d) {}

bool BencodeParser::BencodeValue::is_string() const { return type_ == STRING; }
bool BencodeParser::BencodeValue::is_integer() const { return type_ == INTEGER; }
bool BencodeParser::BencodeValue::is_list() const { return type_ == LIST; }
bool BencodeParser::BencodeValue::is_dict() const { return type_ == DICT; }
const BencodeParser::BencodeString& BencodeParser::BencodeValue::as_string() const { return string_val_; }
BencodeParser::BencodeInteger BencodeParser::BencodeValue::as_integer() const { return int_val_; }
const BencodeParser::BencodeList& BencodeParser::BencodeValue::as_list() const { return list_val_; }
const BencodeParser::BencodeDict& BencodeParser::BencodeValue::as_dict() const { return dict_val_; }

std::shared_ptr<BencodeParser::BencodeValue> BencodeParser::parse(const std::string& data) {
    size_t pos = 0;
    return parse(data, pos);
}

std::shared_ptr<BencodeParser::BencodeValue> BencodeParser::parse(const std::string& data, size_t& pos) {
    return parse_value(data, pos);
}

std::shared_ptr<BencodeParser::BencodeValue> BencodeParser::parse_value(const std::string& data, size_t& pos) {
    if (pos >= data.length()) {
        std::cerr << "BencodeParser: Position beyond data length" << std::endl;
        return nullptr;
    }

    char c = data[pos];

    if (c == 'i') {
        return parse_integer(data, pos);
    } else if (c == 'l') {
        return parse_list(data, pos);
    } else if (c == 'd') {
        return parse_dictionary(data, pos);
    } else if (std::isdigit(c)) {
        return parse_string(data, pos);
    } else {
        std::cerr << "BencodeParser: Unknown type '" << c << "' at position " << pos << std::endl;
        return nullptr;
    }
}

std::shared_ptr<BencodeParser::BencodeValue> BencodeParser::parse_integer(const std::string& data, size_t& pos) {
    if (pos >= data.length() || data[pos] != 'i') {
        return nullptr;
    }

    ++pos;
    size_t end_pos = data.find('e', pos);

    if (end_pos == std::string::npos) {
        std::cerr << "BencodeParser: Integer missing closing 'e'" << std::endl;
        return nullptr;
    }

    std::string num_str = data.substr(pos, end_pos - pos);
    pos = end_pos + 1; // Skip 'e'

    try {
        int64_t value = std::stoll(num_str);
        return std::make_shared<BencodeValue>(value);
    } catch (const std::exception& e) {
        std::cerr << "BencodeParser: Invalid integer '" << num_str << "'" << std::endl;
        return nullptr;
    }
}

std::shared_ptr<BencodeParser::BencodeValue> BencodeParser::parse_string(const std::string& data, size_t& pos) {
    size_t colon_pos = data.find(':', pos);
    if (colon_pos == std::string::npos) {
        return nullptr;
    }

    std::string length_str = data.substr(pos, colon_pos - pos);
    size_t length;

    try {
        length = std::stoull(length_str);
    } catch (const std::exception&) {
        return nullptr;
    }
    pos = colon_pos + 1;
    if (pos + length > data.length()) {
        return nullptr;
    }

    std::string result = data.substr(pos, length);
    pos += length;
    return std::make_shared<BencodeValue>(result);
}

std::shared_ptr<BencodeParser::BencodeValue> BencodeParser::parse_list(const std::string& data, size_t& pos) {
    if (pos >= data.length() || data[pos] != 'l') {
        return nullptr;
    }

    ++pos;
    BencodeList list;

    while (pos < data.length() && data[pos] != 'e') {
        auto value = parse_value(data, pos);
        if (!value) {
            return nullptr;
        }
        list.push_back(value);
    }
    if (pos >= data.length() || data[pos] != 'e') {
        return nullptr;
    }

    ++pos;
    return std::make_shared<BencodeValue>(list);
}

std::shared_ptr<BencodeParser::BencodeValue> BencodeParser::parse_dictionary(const std::string& data, size_t& pos) {
    if (pos >= data.length() || data[pos] != 'd') {
        return nullptr;
    }
    ++pos;
    BencodeDict dict;

    while (pos < data.length() && data[pos] != 'e') {
        auto key_value = parse_string(data, pos);
        if (!key_value || key_value->type_ != BencodeValue::STRING) {
            return nullptr;
        }
        auto value = parse_value(data, pos);
        if (!value) {
            return nullptr;
        }
        dict[key_value->as_string()] = value;
    }

    if (pos >= data.length() || data[pos] != 'e') {
        return nullptr;
    }
    ++pos;
    return std::make_shared<BencodeValue>(dict);
}

std::string BencodeEncoder::encode(const BencodeValue& val) {
    if (val.is_string()) {
        return encode_string(val.as_string());
    } else if (val.is_integer()) {
        return encode_integer(val.as_integer());
    } else if (val.is_list()) {
        return encode_list(val.as_list());
    } else if (val.is_dict()) {
        return encode_dict(val.as_dict());
    }
    return "";
}

std::vector<uint8_t> BencodeEncoder::encode_bytes(const BencodeValue& val) {
    std::string encoded = encode(val);
    return std::vector<uint8_t>(encoded.begin(), encoded.end());
}


std::string BencodeEncoder::encode_string(const std::string& str) {
    return std::to_string(str.length()) + ":" + str;
}

std::string BencodeEncoder::encode_integer(int64_t num) {
    return "i" + std::to_string(num) + "e";
}

std::string BencodeEncoder::encode_list(const BencodeList& list) {
    std::string result = "l";
    for (const auto& item : list) {
        result += encode(*item);
    }
    result += "e";
    return result;
}

std::string BencodeEncoder::encode_dict(const BencodeDict& dict) {
    std::string result = "d";

    std::vector<std::string> keys;
    for (const auto& [key, value] : dict) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());

    for (const std::string& key : keys) {
        result += encode_string(key);
        result += encode(*dict.at(key));
    }
    result += "e";
    return result;
}