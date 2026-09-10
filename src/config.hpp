#pragma once

#include <fstream>
#include <string>
#include <unordered_map>

namespace WildanDev
{

// Minimal key=value config reader. Lines starting with '#' and blank lines
// are ignored. Whitespace around keys and values is trimmed.
class Config
{
public:
    bool load(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return false;

        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#')
                continue;
            auto eq = line.find('=', first);
            if (eq == std::string::npos)
                continue;
            std::string key = line.substr(first, eq - first);
            std::string value = line.substr(eq + 1);
            trim(key);
            trim(value);
            m_values[std::move(key)] = std::move(value);
        }
        return true;
    }

    std::string get(const std::string& key, const std::string& fallback = "") const
    {
        auto it = m_values.find(key);
        return it == m_values.end() ? fallback : it->second;
    }

    int getInt(const std::string& key, int fallback = 0) const
    {
        auto it = m_values.find(key);
        if (it == m_values.end())
            return fallback;
        try
        {
            return std::stoi(it->second);
        }
        catch (...)
        {
            return fallback;
        }
    }

private:
    static void trim(std::string& text)
    {
        auto begin = text.find_first_not_of(" \t");
        if (begin == std::string::npos)
        {
            text.clear();
            return;
        }
        auto end = text.find_last_not_of(" \t");
        text = text.substr(begin, end - begin + 1);
    }

    std::unordered_map<std::string, std::string> m_values;
};

} // namespace WildanDev
