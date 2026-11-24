#include "Toml.h"

#include <cctype>
#include <sstream>

namespace msid::toml {

namespace {

std::string trim (const std::string& s)
{
    size_t a = s.find_first_not_of (" \t\r\n");
    if (a == std::string::npos)
        return {};
    size_t b = s.find_last_not_of (" \t\r\n");
    return s.substr (a, b - a + 1);
}

std::string stripComment (const std::string& line)
{
    bool inStr = false;
    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];
        if (c == '"' && (i == 0 || line[i - 1] != '\\'))
            inStr = ! inStr;
        else if (c == '#' && ! inStr)
            return line.substr (0, i);
    }
    return line;
}

std::vector<std::string> splitArray (const std::string& inner, const std::string& where)
{
    std::vector<std::string> items;
    std::string cur;
    bool inStr = false;
    for (size_t i = 0; i < inner.size(); ++i)
    {
        char c = inner[i];
        if (c == '"' && (i == 0 || inner[i - 1] != '\\'))
            inStr = ! inStr;
        if (c == ',' && ! inStr)
        {
            items.push_back (trim (cur));
            cur.clear();
        }
        else if ((c == '[' || c == ']') && ! inStr)
            throw Error (where + ": nested arrays are not supported");
        else
            cur += c;
    }
    if (inStr)
        throw Error (where + ": unterminated string");
    auto last = trim (cur);
    if (! last.empty())
        items.push_back (last);
    for (auto& it : items)
        if (it.empty())
            throw Error (where + ": empty array element");
    return items;
}

Value parseValue (const std::string& raw, const std::string& where)
{
    auto v = trim (raw);
    Value out;
    out.where = where;
    if (v.empty())
        throw Error (where + ": missing value");
    if (v.front() == '"')
    {
        if (v.size() < 2 || v.back() != '"')
            throw Error (where + ": unterminated string");
        std::string s;
        for (size_t i = 1; i + 1 < v.size(); ++i)
        {
            if (v[i] == '\\' && i + 2 < v.size())
            {
                char n = v[++i];
                s += n == 'n' ? '\n' : n == 't' ? '\t' : n;
            }
            else
                s += v[i];
        }
        out.v = s;
        return out;
    }
    if (v.front() == '[')
    {
        if (v.back() != ']')
            throw Error (where + ": arrays must be on one line: [1, 2]");
        Array arr;
        for (auto& item : splitArray (v.substr (1, v.size() - 2), where))
            arr.push_back (parseValue (item, where));
        out.v = arr;
        return out;
    }
    if (v == "true" || v == "false")
    {
        out.v = v == "true";
        return out;
    }
    std::string num;
    for (char c : v)
        if (c != '_')
            num += c;
    try
    {
        size_t used = 0;
        bool isFloat = num.find_first_of (".eE") != std::string::npos && num.rfind ("0x", 0) != 0;
        if (isFloat)
        {
            double d = std::stod (num, &used);
            if (used == num.size())
            {
                out.v = d;
                return out;
            }
        }
        else
        {
            long long n = std::stoll (num, &used, 0);
            if (used == num.size())
            {
                out.v = n;
                return out;
            }
        }
    }
    catch (...) {}
    throw Error (where + ": cannot read value '" + v + "'");
}

bool validKey (const std::string& k)
{
    if (k.empty())
        return false;
    for (char c : k)
        if (! (std::isalnum ((unsigned char) c) || c == '_' || c == '-'))
            return false;
    return true;
}

} // namespace

Document parse (const std::string& text, const std::string& source)
{
    Document doc;
    Table* cur = &doc.root;
    std::istringstream in (text);
    std::string line;
    int lineNo = 0;
    while (std::getline (in, line))
    {
        ++lineNo;
        auto where = source + ":" + std::to_string (lineNo);
        auto l = trim (stripComment (line));
        if (l.empty())
            continue;
        if (l.rfind ("[[", 0) == 0)
        {
            if (l.size() < 5 || l.substr (l.size() - 2) != "]]")
                throw Error (where + ": expected [[name]]");
            auto name = trim (l.substr (2, l.size() - 4));
            if (! validKey (name))
                throw Error (where + ": bad table name '" + name + "'");
            if (doc.tables.count (name))
                throw Error (where + ": [" + name + "] is a table, not an array of tables");
            doc.arrays[name].emplace_back();
            cur = &doc.arrays[name].back();
            continue;
        }
        if (l.front() == '[')
        {
            if (l.back() != ']')
                throw Error (where + ": expected [name]");
            auto name = trim (l.substr (1, l.size() - 2));
            if (! validKey (name))
                throw Error (where + ": bad table name '" + name + "'");
            if (doc.tables.count (name) || doc.arrays.count (name))
                throw Error (where + ": [" + name + "] defined twice");
            cur = &doc.tables[name];
            doc.tableWhere[name] = where;
            continue;
        }
        auto eq = l.find ('=');
        if (eq == std::string::npos)
            throw Error (where + ": expected key = value");
        auto key = trim (l.substr (0, eq));
        if (! validKey (key))
            throw Error (where + ": bad key '" + key + "'");
        if (cur->count (key))
            throw Error (where + ": '" + key + "' set twice");
        (*cur)[key] = parseValue (l.substr (eq + 1), where);
    }
    return doc;
}

} // namespace msid::toml
