#include "server.hpp"
#include "items.hpp"
#include "logger.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>

namespace WildanDev
{

void GameServer::registerBuiltinCommands()
{
    registerCommand("help", {"command.basic", "/help",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 (void)args;
                                 sendConsoleMessage(session.peer, "Commands: /help /who /me /news /time "
                                                                  "/warp /store /skin /weather /sb <msg> "
                                                                  "/ghost /kick /mute /ban + emotes (/love, "
                                                                  "/troll...)");
                             }});

    registerCommand("who", {"command.basic", "/who",
                            [this](Session& session, const std::vector<std::string>& args) {
                                (void)args;
                                std::size_t online = 0;
                                for (const auto& [peer, candidate] : m_sessions)
                                {
                                    (void)peer;
                                    if (candidate.authenticated)
                                        ++online;
                                }
                                sendConsoleMessage(session.peer,
                                                   "Players online: " + std::to_string(online) + ".");
                            }});

    registerCommand("ghost", {"command.ghost", "/ghost",
                              [this](Session& session, const std::vector<std::string>& args) {
                                  (void)args;
                                  session.ghost = !session.ghost;
                                  sendConsoleMessage(session.peer, session.ghost ? "Ghost mode on."
                                                                                : "Ghost mode off.");
                              }});

    registerCommand("warp", {"command.basic", "/warp <world>",
                              [this](Session& session, const std::vector<std::string>& args) {
                                  if (args.empty())
                                  {
                                      sendConsoleMessage(session.peer, "Usage: /warp <world>");
                                      return;
                                  }
                                  joinWorld(session, args[0]);
                              }});

    registerCommand("mod", {"command.mod", "/mod <name>",
                            [this](Session& session, const std::vector<std::string>& args) {
                                if (args.empty())
                                {
                                    sendConsoleMessage(session.peer, "Usage: /mod <name>");
                                    return;
                                }
                                Session* target = findSession(args[0]);
                                if (target == nullptr)
                                {
                                    sendConsoleMessage(session.peer, "Player not found.");
                                    return;
                                }
                                std::string denied;
                                if (!canModerate(session, *target, &denied))
                                {
                                    sendConsoleMessage(session.peer, denied);
                                    return;
                                }
                                const Role* role = findRoleByName("Moderator");
                                if (role == nullptr)
                                {
                                    sendConsoleMessage(session.peer, "Moderator role is not configured.");
                                    return;
                                }
                                target->roleId = role->id;
                                if (!m_database.setRole(target->playerId, role->id))
                                {
                                    sendConsoleMessage(session.peer, "Role change failed to persist.");
                                    return;
                                }
                                sendConsoleMessage(session.peer, target->growId + " is now a Moderator.");
                                sendConsoleMessage(target->peer, "You are now a Moderator.");
                                logInfo(session.growId + " granted Moderator to " + target->growId);
                            }});

    registerCommand("unmod", {"command.mod", "/unmod <name>",
                              [this](Session& session, const std::vector<std::string>& args) {
                                  if (args.empty())
                                  {
                                      sendConsoleMessage(session.peer, "Usage: /unmod <name>");
                                      return;
                                  }
                                Session* target = findSession(args[0]);
                                if (target == nullptr)
                                {
                                    sendConsoleMessage(session.peer, "Player not found.");
                                    return;
                                }
                                std::string denied;
                                if (!canModerate(session, *target, &denied))
                                {
                                    sendConsoleMessage(session.peer, denied);
                                    return;
                                }
                                const Role* role = m_roles.getDefaultRole();
                                if (role == nullptr)
                                {
                                    sendConsoleMessage(session.peer, "Default role is not configured.");
                                    return;
                                }
                                  target->roleId = role->id;
                                  if (!m_database.setRole(target->playerId, role->id))
                                  {
                                      sendConsoleMessage(session.peer, "Role change failed to persist.");
                                      return;
                                  }
                                  sendConsoleMessage(session.peer, target->growId + " is back to " + role->name +
                                                                          ".");
                                  sendConsoleMessage(target->peer, "Your staff role was revoked.");
                                  logInfo(session.growId + " revoked staff from " + target->growId);
                              }});

    registerCommand("give", {"command.give", "/give <name> <item_id> <count>",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 if (args.size() < 3)
                                 {
                                     sendConsoleMessage(session.peer, "Usage: /give <name> <item_id> <count>");
                                     return;
                                 }
                                 Session* target = findSession(args[0]);
                                 if (target == nullptr)
                                 {
                                     sendConsoleMessage(session.peer, "Player not found.");
                                     return;
                                 }
                                 int itemId = 0;
                                 int count = 0;
                                 try
                                 {
                                     itemId = std::stoi(args[1]);
                                     count = std::stoi(args[2]);
                                 }
                                 catch (...)
                                 {
                                     sendConsoleMessage(session.peer, "Item id and count must be numbers.");
                                     return;
                                 }
                                 const ItemDef* item = findItemById(itemId);
                                 if (item == nullptr || count <= 0 || count > 200)
                                 {
                                     sendConsoleMessage(session.peer, "Unknown item or bad count (1-200).");
                                     return;
                                 }

                                 bool found = false;
                                 int newCount = count;
                                 for (auto& slot : target->inventory)
                                 {
                                     if (slot.first == itemId)
                                     {
                                         slot.second = std::min(200, slot.second + count);
                                         newCount = slot.second;
                                         found = true;
                                         break;
                                     }
                                 }
                                 if (!found)
                                     target->inventory.emplace_back(itemId, count);
                                 m_database.setInventoryItem(target->playerId, itemId, newCount);
                                 sendInventoryState(*target);

                                 sendConsoleMessage(session.peer, "Gave " + std::to_string(count) + "x " +
                                                                  item->name + " to " + target->growId + ".");
                                 sendConsoleMessage(target->peer, "Received " + std::to_string(count) + "x " +
                                                                  item->name + ".");
                                 logInfo(session.growId + " gave " + item->name + " x" +
                                         std::to_string(count) + " to " + target->growId);
                             }});

    registerCommand("kick", {"player.kick", "/kick <name>",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 if (args.empty())
                                 {
                                     sendConsoleMessage(session.peer, "Usage: /kick <name>");
                                     return;
                                 }
                                Session* target = findSession(args[0]);
                                if (target == nullptr)
                                {
                                    sendConsoleMessage(session.peer, "Player not found.");
                                    return;
                                }
                                std::string denied;
                                if (!canModerate(session, *target, &denied))
                                {
                                    sendConsoleMessage(session.peer, denied);
                                    return;
                                }
                                logInfo(session.growId + " kicked " + target->growId);
                                sendConsoleMessage(target->peer, "Kicked by staff.");
                                enet_peer_disconnect(target->peer, 0);
                             }});

    registerCommand("mute", {"player.mute", "/mute <name>",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 if (args.empty())
                                 {
                                     sendConsoleMessage(session.peer, "Usage: /mute <name>");
                                     return;
                                 }
                                Session* target = findSession(args[0]);
                                if (target == nullptr)
                                {
                                    sendConsoleMessage(session.peer, "Player not found.");
                                    return;
                                }
                                std::string denied;
                                if (!canModerate(session, *target, &denied))
                                {
                                    sendConsoleMessage(session.peer, denied);
                                    return;
                                }
                                target->muted = true;
                                m_database.setMuted(target->playerId, true);
                                sendConsoleMessage(session.peer, target->growId + " is muted.");
                                sendConsoleMessage(target->peer, "You were muted by staff.");
                                logInfo(session.growId + " muted " + target->growId);
                             }});

    registerCommand("unmute", {"player.mute", "/unmute <name>",
                               [this](Session& session, const std::vector<std::string>& args) {
                                   if (args.empty())
                                   {
                                       sendConsoleMessage(session.peer, "Usage: /unmute <name>");
                                       return;
                                   }
                                   Session* target = findSession(args[0]);
                                   if (target == nullptr)
                                   {
                                       sendConsoleMessage(session.peer, "Player not found.");
                                       return;
                                   }
                                   std::string denied;
                                   if (!canModerate(session, *target, &denied))
                                   {
                                       sendConsoleMessage(session.peer, denied);
                                       return;
                                   }
                                   target->muted = false;
                                   m_database.setMuted(target->playerId, false);
                                   sendConsoleMessage(session.peer, target->growId + " is unmuted.");
                                   sendConsoleMessage(target->peer, "You were unmuted.");
                                   logInfo(session.growId + " unmuted " + target->growId);
                               }});

    registerCommand("ban", {"player.ban", "/ban <name>",
                            [this](Session& session, const std::vector<std::string>& args) {
                                if (args.empty())
                                {
                                    sendConsoleMessage(session.peer, "Usage: /ban <name>");
                                    return;
                                }
                                Session* target = findSession(args[0]);
                                if (target == nullptr)
                                {
                                    sendConsoleMessage(session.peer, "Player not found.");
                                    return;
                                }
                                std::string denied;
                                if (!canModerate(session, *target, &denied))
                                {
                                    sendConsoleMessage(session.peer, denied);
                                    return;
                                }
                                m_database.setBanned(target->playerId, true);
                                logInfo(session.growId + " banned " + target->growId);
                                sendConsoleMessage(target->peer, "Banned by staff.");
                                enet_peer_disconnect(target->peer, 0);
                            }});

    registerCommand("unban", {"player.ban", "/unban <growid>",
                              [this](Session& session, const std::vector<std::string>& args) {
                                  if (args.empty())
                                  {
                                      sendConsoleMessage(session.peer, "Usage: /unban <growid>");
                                      return;
                                  }
                                  auto playerId = m_database.findPlayerId(args[0]);
                                  if (!playerId.has_value())
                                  {
                                      sendConsoleMessage(session.peer, "Account not found.");
                                      return;
                                  }
                                  m_database.setBanned(playerId.value(), false);
                                  sendConsoleMessage(session.peer, args[0] + " is unbanned.");
                                  logInfo(session.growId + " unbanned " + args[0]);
                              }});

    registerCommand("sb", {"command.broadcast", "/sb <message>",
                           [this](Session& session, const std::vector<std::string>& args) {
                               if (session.muted)
                               {
                                   sendConsoleMessage(session.peer, "You are muted.");
                                   return;
                               }
                               if (args.empty())
                               {
                                   sendConsoleMessage(session.peer, "Usage: /sb <message>");
                                   return;
                               }
                               std::string message;
                               for (const auto& word : args)
                               {
                                   if (!message.empty())
                                       message.push_back(' ');
                                   message += word;
                               }
                               std::string world = session.worldName.empty() ? "?" : session.worldName;
                               std::string full = "CP:0_PL:0_OID:_CT:[SB]_ `5** from (" + roleColor(session) +
                                                  session.growId + "``5) in [``$" + world + "``5] ** : ``$" +
                                                  message + "``";
                               for (auto& [peer, candidate] : m_sessions)
                               {
                                   if (candidate.authenticated)
                                       sendConsoleMessage(peer, full);
                               }
                               logInfo("[SB] " + session.growId + " (" + world + "): " + message);
                           }});

    registerCommand("me", {"command.basic", "/me <text>",
                           [this](Session& session, const std::vector<std::string>& args) {
                               std::string message;
                               for (const auto& word : args)
                               {
                                   if (!message.empty())
                                       message.push_back(' ');
                                   message += word;
                               }
                               if (message.empty())
                               {
                                   sendConsoleMessage(session.peer, "Usage: /me <text>");
                                   return;
                               }
                               if (session.muted)
                               {
                                   sendConsoleMessage(session.peer, "You are muted.");
                                   return;
                               }
                               if (session.worldName.empty())
                                   return;
                               World& world = m_worlds.getOrCreate(session.worldName);
                               std::string color = roleColor(session);
                               std::string bubble = "CP:0_PL:0_OID:_player_chat= `6<" + color +
                                                    session.growId + "``6>`` " + message;
                               std::string console = "CP:0_PL:0_OID:_CT:[W]_ `6<" + color + session.growId +
                                                     "``6>`` " + message;
                               for (auto& [peer, candidate] : m_sessions)
                               {
                                   if (candidate.worldName != world.name)
                                       continue;
                                   sendVariant(peer, {VariantValue::makeString("OnTalkBubble"),
                                                      VariantValue::makeInt(session.netId),
                                                      VariantValue::makeString(bubble),
                                                      VariantValue::makeUInt(0)});
                                   sendConsoleMessage(peer, console);
                               }
                           }});

    registerCommand("news", {"command.basic", "/news",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 (void)args;
                                 if (session.authenticated)
                                     handleEnterGame(session);
                             }});

    registerCommand("time", {"command.basic", "/time",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 (void)args;
                                 std::time_t t = std::time(nullptr);
                                 char buf[32] = {};
                                 std::strftime(buf, sizeof(buf), "%H:%M", std::localtime(&t));
                                 static const char* months[] = {"January",  "February", "March",
                                                                "April",    "May",      "June",
                                                                "July",     "August",   "September",
                                                                "October",  "November", "December"};
                                 std::tm* tm = std::localtime(&t);
                                 sendConsoleMessage(session.peer, std::string("`2Growtopia Time: `w") +
                                                                      months[tm->tm_mon] + " " +
                                                                      std::to_string(tm->tm_mday) + ", " +
                                                                      buf + "`` (server time)");
                             }});

    registerCommand("weather", {"command.weather", "/weather <0-80>",
                                [this](Session& session, const std::vector<std::string>& args) {
                                    if (args.empty())
                                    {
                                        sendConsoleMessage(session.peer, "Usage: /weather <0=sunny, 2=night, "
                                                                         "11=snowy, 18=party ...>");
                                        return;
                                    }
                                    int id = 0;
                                    if (auto [ptr, ec] = std::from_chars(args[0].data(),
                                                                         args[0].data() + args[0].size(), id);
                                        ec != std::errc() || id < 0 || id > 80)
                                    {
                                        sendConsoleMessage(session.peer, "Weather id must be 0-80.");
                                        return;
                                    }
                                    if (session.worldName.empty())
                                        return;
                                    World& world = m_worlds.getOrCreate(session.worldName);
                                    for (auto& [peer, candidate] : m_sessions)
                                    {
                                        if (candidate.worldName != world.name)
                                            continue;
                                        sendVariant(peer, {VariantValue::makeString("OnSetCurrentWeather"),
                                                           VariantValue::makeInt(id)});
                                    }
                                    logInfo(session.growId + " set weather " + std::to_string(id) + " in " +
                                            world.name);
                                }});

    registerCommand("skin", {"command.basic", "/skin <rgba>",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 if (args.empty())
                                 {
                                     sendConsoleMessage(session.peer,
                                                        "Usage: /skin <rgba decimal, e.g. 4294967295=white>");
                                     return;
                                 }
                                 unsigned long value = 0;
                                 if (auto [ptr, ec] = std::from_chars(args[0].data(),
                                                                      args[0].data() + args[0].size(), value);
                                     ec != std::errc() || value > 0xFFFFFFFFul)
                                 {
                                     sendConsoleMessage(session.peer, "Invalid color value.");
                                     return;
                                 }
                                 session.skinColor = static_cast<uint32_t>(value);
                                 sendSetClothing(session, true);
                             }});

    registerCommand("who", {"command.basic", "/who",
                            [this](Session& session, const std::vector<std::string>& args) {
                                (void)args;
                                if (session.worldName.empty())
                                {
                                    sendConsoleMessage(session.peer, "Join a world first.");
                                    return;
                                }
                                World& world = m_worlds.getOrCreate(session.worldName);
                                std::string names;
                                for (auto& [peer, candidate] : m_sessions)
                                {
                                    if (candidate.worldName != world.name)
                                        continue;
                                    if (candidate.netId != session.netId)
                                        sendVariant(session.peer,
                                                    {VariantValue::makeString("OnTalkBubble"),
                                                     VariantValue::makeInt(candidate.netId),
                                                     VariantValue::makeString(candidate.growId),
                                                     VariantValue::makeUInt(1)});
                                    if (!names.empty())
                                        names += ", ";
                                    names += candidate.growId;
                                }
                                sendConsoleMessage(session.peer, "`wWho's in `$" + world.name +
                                                                     "``: " + names + "``");
                            }});

    registerCommand("store", {"command.basic", "/store",
                              [this](Session& session, const std::vector<std::string>& args) {
                                  (void)args;
                                  if (session.authenticated)
                                      sendStoreDialog(session);
                              }});

    registerCommand("nuke", {"command.nuke", "/nuke",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 (void)args;
                                 if (session.worldName.empty())
                                 {
                                     sendConsoleMessage(session.peer, "Join a world first.");
                                     return;
                                 }
                                 World& world = m_worlds.getOrCreate(session.worldName);
                                 for (auto& [peer, candidate] : m_sessions)
                                 {
                                     if (candidate.worldName != world.name)
                                         continue;
                                     sendConsoleMessage(peer, "`4This world is being nuked by " +
                                                                  session.growId + "!");
                                 }
                                 World fresh;
                                 fresh.name = world.name;
                                 fresh.ownerId = world.ownerId;
                                 fresh.isPublic = world.isPublic;
                                 m_worlds.replace(world.name, std::move(fresh));
                                 // Force everyone out; the world regenerates on rejoin.
                                 for (auto& [peer, candidate] : m_sessions)
                                 {
                                     if (candidate.worldName != world.name)
                                         continue;
                                     sendVariant(peer, {VariantValue::makeString("OnRemove"),
                                                        VariantValue::makeString("netID|" +
                                                                                 std::to_string(candidate.netId) +
                                                                                 "\n")},
                                                 candidate.netId);
                                     candidate.worldName.clear();
                                     candidate.netId = 0;
                                 }
                                 logInfo(session.growId + " nuked world " + world.name);
                             }});

    registerCommand("maintenance", {"command.nuke", "/maintenance <on|off>",
                                    [this](Session& session, const std::vector<std::string>& args) {
                                        if (args.empty() || (args[0] != "on" && args[0] != "off"))
                                        {
                                            sendConsoleMessage(session.peer,
                                                                "Usage: /maintenance <on|off>");
                                            return;
                                        }
                                        bool on = args[0] == "on";
                                        std::ofstream file("resources/maintenance",
                                                           std::ios::trunc);
                                        if (on)
                                            file << "1";
                                        file.close();
                                        if (!on)
                                            std::remove("resources/maintenance");
                                        std::string text = on ? "`4Server maintenance ENABLED.`` New "
                                                                "logins will see the maintenance page."
                                                              : "`2Server maintenance DISABLED.``";
                                        sendConsoleMessage(session.peer, text);
                                        logInfo(session.growId + " set maintenance " + args[0]);
                                    }});

    // Growmoji emotes: /wl, /love, /troll, ... show the glyph in a bubble.
    for (const auto& [name, glyph] : kEmoteGlyphs)
    {
        registerCommand(name, {"command.basic", "/" + name,
                               [this, glyph](Session& session, const std::vector<std::string>& args) {
                                   (void)args;
                                   if (session.muted || session.worldName.empty())
                                       return;
                                   World& world = m_worlds.getOrCreate(session.worldName);
                                   for (auto& [peer, candidate] : m_sessions)
                                   {
                                       if (candidate.worldName != world.name)
                                           continue;
                                       sendVariant(peer, {VariantValue::makeString("OnTalkBubble"),
                                                          VariantValue::makeInt(session.netId),
                                                          VariantValue::makeString(glyph),
                                                          VariantValue::makeUInt(0)});
                                   }
                               }});
    }
}

} // namespace WildanDev
