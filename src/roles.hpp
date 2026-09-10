#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WildanDev
{

struct Role
{
    int id{0};
    std::string name;
    int rank{0};
    std::unordered_set<std::string> permissions;

    bool hasPermission(const std::string& permission) const
    {
        return permissions.count("*") > 0 || permissions.count(permission) > 0;
    }
};

// Loads roles.conf and resolves permission checks. Format per line:
//   id|name|rank|perm1,perm2,...
// A "default=<id>" line selects the fallback role for new players.
class RoleManager
{
public:
    bool load(const std::string& path);

    const Role* getRole(int id) const;
    const Role* getDefaultRole() const;
    const std::unordered_map<int, Role>& all() const { return m_roles; }

private:
    std::unordered_map<int, Role> m_roles;
    int m_defaultId{0};
};

} // namespace WildanDev
