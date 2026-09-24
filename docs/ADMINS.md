# Admins (shared DB)

Ready Up stores a **global admin list** in Postgres so multiple servers can share the same admins.

## Local Postgres (docker)

```bash
docker compose -f compose.postgres.yml up -d
```

This exposes Postgres on **127.0.0.1:5449**.

## DB config file

Config lives next to the Ready Up shim:

`game/csgo/readyup/bin/linuxsteamrt64/readyup_db.json`

Example:

```json
{
  "conninfo": "host=127.0.0.1 port=5449 dbname=readyup user=readyup password=readyup sslmode=disable"
}
```

## Commands

- **Public**:
  - `.ru admins` (list)
- **Admin-only**:
  - `.ru admins add <steamid64|name_fragment>`
  - `.ru admins remove <steamid64|name_fragment>`

Notes:
- If no admins exist yet, the **first admin must be added from server console** (or seeded in DB).
- SteamID input is **SteamID64** (decimal) or a connected-player name fragment.

