#include "server.hpp"
#include "items.hpp"
#include "logger.hpp"

#include <algorithm>

namespace WildanDev
{

void GameServer::registerBuiltinCommands()
{
    registerCommand("help", {"command.basic", "/help",
                             [this](Session& session, const std::vector<std::string>& args) {
                                 (void)args;
                                 sendConsoleMessage(session.peer, "Commands: /help /who /warp <world> "
                                                                  "/give <name> <id> <n> /sb <msg> /ghost /kick "
                                                                  "/mute /ban /mod /unmod");
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
                               std::string full = "[SB] <" + session.growId + "> " + message;
                               for (auto& [peer, candidate] : m_sessions)
                               {
                                   if (candidate.authenticated)
                                       sendConsoleMessage(peer, full);
                               }
                               logInfo(full);
                           }});
}

} // namespace WildanDev
