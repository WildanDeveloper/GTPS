#include "roles.hpp"
#include "logger.hpp"

#include <fstream>
#include <sstream>

namespace WildanDev
{

namespace
{

std::vector<std::string> split(const std::string& text, char delimiter)
{
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, delimiter))
        parts.push_back(current);
    return parts;
}

std::string trimCopy(std::string text)
{
    auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos)
        return "";
    auto end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

} // namespace

bool RoleManager::load(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        logError("Cannot open roles file: " + path);
        return false;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        line = trimCopy(line);
        if (line.empty() || line[0] == '#')
            continue;

        if (line.rfind("default=", 0) == 0)
        {
            try
            {
                m_defaultId = std::stoi(trimCopy(line.substr(8)));
            }
            catch (...)
            {
                logWarn("Invalid default role on line " + std::to_string(lineNumber));
            }
            continue;
        }

        auto fields = split(line, '|');
        if (fields.size() != 5)
        {
            logWarn("Skipping malformed role on line " + std::to_string(lineNumber));
            continue;
        }

        Role role;
        try
        {
            role.id = std::stoi(trimCopy(fields[0]));
            role.rank = std::stoi(trimCopy(fields[2]));
        }
        catch (...)
        {
            logWarn("Skipping role with invalid id/rank on line " + std::to_string(lineNumber));
            continue;
        }
        role.name = trimCopy(fields[1]);
        std::string color = trimCopy(fields[3]);
        role.color = color.empty() ? "`w" : color;
        for (const auto& perm : split(fields[4], ','))
        {
            std::string cleaned = trimCopy(perm);
            if (!cleaned.empty())
                role.permissions.insert(cleaned);
        }
        m_roles[role.id] = std::move(role);
    }

    if (m_roles.empty() || m_roles.count(m_defaultId) == 0)
    {
        logError("Role table is empty or default role is missing");
        return false;
    }

    logInfo("Loaded " + std::to_string(m_roles.size()) + " roles, default=" + std::to_string(m_defaultId));
    return true;
}

const Role* RoleManager::getRole(int id) const
{
    auto it = m_roles.find(id);
    return it == m_roles.end() ? nullptr : &it->second;
}

const Role* RoleManager::getDefaultRole() const
{
    return getRole(m_defaultId);
}

} // namespace WildanDev
