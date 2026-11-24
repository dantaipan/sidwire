// Small TOML-subset reader (internal): enough for controller profiles.
//   key = "string" | integer (incl. 0x..) | float | true/false | [single-line array of those]
//   [table], [[array.of.tables]], # comments
// Anything else is an error with the line number.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace msid::toml {

struct Value;
using Array = std::vector<Value>;
struct Value
{
    std::variant<std::string, long long, double, bool, Array> v;
    std::string where;   // "source:line" for messages

    bool isString() const { return std::holds_alternative<std::string> (v); }
    bool isInt() const { return std::holds_alternative<long long> (v); }
    bool isFloat() const { return std::holds_alternative<double> (v); }
    bool isBool() const { return std::holds_alternative<bool> (v); }
    bool isArray() const { return std::holds_alternative<Array> (v); }
    bool isNumber() const { return isInt() || isFloat(); }
    double number() const { return isInt() ? (double) std::get<long long> (v) : std::get<double> (v); }
};

using Table = std::map<std::string, Value>;

struct Document
{
    Table root;
    std::map<std::string, Table> tables;                 // [name]
    std::map<std::string, std::vector<Table>> arrays;    // [[name]]
    std::map<std::string, std::string> tableWhere;        // [name] -> "source:line"
    std::vector<std::string> arrayWhere;                  // not needed beyond errors
};

struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

Document parse (const std::string& text, const std::string& source);

} // namespace msid::toml
