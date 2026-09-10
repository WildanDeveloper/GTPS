# WildanDev GTPS

Growtopia private server in a **single C++ binary** — ENet game server, HTTPS
login service, and web account management in one process, backed by MariaDB.

## Features

### Game server (ENet, port 17091)
- Login flow for modern clients (protocol 226, web-token based) with
  pending-auth resume bound to client IP
- Procedural world generation: grass surface, dirt, cave pockets, rock and
  lava speckles, cave background, bedrock floor, main door with spawn point
- Multi-hit punching with tile damage packets and per-tile reset timers
- Item drops as world objects: gems, blocks and seeds with rarity-based
  chances, same-tile merging up to 200, pickup with inventory-full handling
- Lava damage, death animation and respawn at the main door
- World locks (item 242) with owner tracking and staff bypass permission
- Chat with bubbles and super-broadcast, ghost mode, moderation commands
  (`/kick`, `/mute`, `/ban`, `/mod`, `/give`, ...) gated by ranked roles
- Anti-cheat basics: server-side item ownership checks, punch reach limits,
  netId/uid rewriting on relays (no avatar spoofing)

### Login service (HTTPS, port 8092, OpenSSL)
- Growtopia handshake endpoint (`server_data`)
- Web forms: **login** (username + password), **register** (username,
  password, confirm, optional email), **password recovery** via email codes
- **Google sign-in** (OAuth 2.0): auto-link by email or auto-register with a
  unique derived username
- Recovery emails delivered through the [Resend](https://resend.com) API
- All served in-process — no PHP, no Python, no external web server required

### Security
- Passwords hashed with **PBKDF2-HMAC-SHA256** (100k iterations, random
  salt); legacy SHA-256 rows migrate transparently on next login
- **Brute-force protection**: 5 failed logins → 5-minute block, escalating on
  forced attempts (up to 24h), keyed by username+IP and username+fingerprint
- **Registration limits**: max 2 accounts per IP (lifetime) + 1 per minute
- **Reset-code protection**: 6-digit codes hashed in DB, 15-minute expiry,
  invalidated after 5 wrong attempts, max 3 emails per IP per hour
- Client IP resolution trusts forwarded headers only from the local reverse
  proxy (anti-spoof)
- Prepared statements everywhere; constant-time digest comparison
- Login rate limiting on the game layer as well (5/60s per growId)

## Requirements

- Linux (developed on Debian/Ubuntu)
- g++ (C++17), make
- libmysqlclient / MariaDB Connector/C
- OpenSSL
- A MariaDB or MySQL server

## Build & run

```bash
make            # build build/WildanDev-game
./build/WildanDev-game
```

First run creates the database, tables and columns automatically. The game
server listens on UDP 17091 and the login service on HTTPS 8092 (one process).

```bash
make tests      # build unit + integration test binaries
./build/test-units
```

## Configuration

All runtime configuration lives in `resources/`:

| File                  | Purpose                                        | In repo      |
| --------------------- | ---------------------------------------------- | ------------ |
| `gameserver.conf`     | bind/ports, public host, login bind            | yes          |
| `db.conf`             | MariaDB credentials                            | `.example`   |
| `roles.conf`          | role ranks + permissions (`id\|name\|rank\|perms`) | yes      |
| `server_data.txt`     | payload served to game clients                 | yes          |
| `resend.conf`         | Resend API key + sender (recovery emails)      | `.example`   |
| `google.conf`         | Google OAuth client id/secret/redirect         | `.example`   |
| `certs/`              | TLS certificate + key for the login service    | ignored      |
| `items.dat`           | official item database served to clients       | ignored      |

### gameserver.conf keys

```ini
bind_host=0.0.0.0        # UDP listen address
bind_port=17091          # UDP listen port
max_peers=64             # ENet peer cap
public_host=...          # IP/hostname told to clients in OnSendToServer
login_host=0.0.0.0       # HTTPS login bind address
login_port=8092          # HTTPS login port
```

Environment overrides: `WILDANDEV_PORT` (game port),
`WILDANDEV_ITEMS_HASH` (advertised items.dat hash).

## Web endpoints

| Route                              | Description                              |
| ---------------------------------- | ---------------------------------------- |
| `GET /player/login/dashboard`      | Login form (also POSTed by the client)   |
| `GET /player/register`             | Registration form                        |
| `GET /player/forgot`               | Request a reset code                     |
| `GET /player/reset`                | Enter code + new password                |
| `GET /player/auth/google`          | Start Google sign-in                     |
| `POST /player/growid/login/validate` | Token endpoint consumed by game clients |
| `POST /player/growid/checktoken`   | 307 → validate/checktoken               |
| `POST /player/growid/validate/checktoken` | Token refresh (issued set only)   |
| `GET|POST /growtopia/server_data.php` | Handshake payload                     |

Put a reverse proxy (nginx) with TLS in front for production and set
`proxy_set_header X-Real-IP $remote_addr;` on the `/player/` location.

## Project layout

```
src/            C++ sources (server, login service, database, world, protocol)
tests/          unit tests + ENet integration client
resources/      configuration
thirdparty/enet vendored ENet
```

## License

Apache 2.0 — see [LICENSE](LICENSE).
