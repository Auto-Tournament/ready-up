# Skins DB contract (for the web UI)

Ready Up reads player loadouts from Postgres. The future skin picker (Auto Tournament CS2 pack,
web side) only has to write these four tables. Ready Up never writes them, except for the
StatTrak counter.

- **Database**: the one in `readyup_db.json` next to the shim, e.g.
  `host=127.0.0.1 port=5449 dbname=readyup user=readyup password=… sslmode=disable`.
- **Schema owner**: Ready Up runs `CREATE TABLE IF NOT EXISTS` for the DDL below at startup
  (`libs/readyup/postgres.cpp`, `EnsureWeaponPaintsSchemaLocked`). The web side may create the
  same tables, but must not change column types or primary keys without a Ready Up change.
- **Example data**: `scripts/seed-dev-skins.sql`.

## Keys shared by every table

| Column | Type | Meaning |
|---|---|---|
| `steamid64` | `BIGINT` | SteamID64 of the player, e.g. `76561198000000000` |
| `weapon_team` | `SMALLINT` | `2` = T, `3` = CT, `0` = both. For each defindex (or knife/glove slot), a team-specific row wins over the team-0 row |

## `readyup_weapon_skins`: paints (weapons, knives, gloves)

```sql
CREATE TABLE IF NOT EXISTS readyup_weapon_skins (
  steamid64 BIGINT NOT NULL,
  weapon_team SMALLINT NOT NULL,
  weapon_defindex INT NOT NULL,
  paint_id INT NOT NULL,
  wear REAL NOT NULL DEFAULT 0.000001,
  seed INT NOT NULL DEFAULT 0,
  nametag TEXT NULL,
  stattrak_enabled BOOLEAN NOT NULL DEFAULT FALSE,
  stattrak_count INT NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (steamid64, weapon_team, weapon_defindex));
```

| Column | Notes |
|---|---|
| `weapon_defindex` | Item definition index from `items_game.txt` (`items` → key). Weapons: AK-47 `7`, AWP `9`, M4A4 `16`, M4A1-S `60`, USP-S `61`, Glock-18 `4`, Desert Eagle `1`, … Knives use the **knife** defindex (Karambit `507`). Gloves use the **glove** defindex (Sport Gloves `5030`). |
| `paint_id` | Paint kit id (`items_game.txt` → `paint_kits` key), e.g. Redline `282`, AWP Asiimov `279`, Howl `309`, Fade `38`, Pandora's Box `10037`. `<= 0` = no paint. |
| `wear` | Float 0..1 (FN < 0.07 ≤ MW < 0.15 ≤ FT < 0.38 ≤ WW < 0.45 ≤ BS). |
| `seed` | Pattern seed 0..1000. |
| `nametag` | Optional name tag, max 160 bytes (longer is truncated). |
| `stattrak_enabled` / `stattrak_count` | Ready Up adds 1 to `stattrak_count` per kill with that weapon (needs game events). The web UI should treat `stattrak_count` as server-owned after creation. |
| `updated_at` | Informational. |

## `readyup_weapon_knives`: knife model

```sql
CREATE TABLE IF NOT EXISTS readyup_weapon_knives (
  steamid64 BIGINT NOT NULL,
  weapon_team SMALLINT NOT NULL,
  knife_classname TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (steamid64, weapon_team));
```

`knife_classname` is one of: `weapon_bayonet` (500), `weapon_knife_css` (503),
`weapon_knife_flip` (505), `weapon_knife_gut` (506), `weapon_knife_karambit` (507),
`weapon_knife_m9_bayonet` (508), `weapon_knife_tactical` (509), `weapon_knife_falchion` (512),
`weapon_knife_survival_bowie` (514), `weapon_knife_butterfly` (515), `weapon_knife_push` (516),
`weapon_knife_cord` (517), `weapon_knife_canis` (518), `weapon_knife_ursus` (519),
`weapon_knife_gypsy_jackknife` (520), `weapon_knife_outdoor` (521), `weapon_knife_stiletto` (522),
`weapon_knife_widowmaker` (523), `weapon_knife_skeleton` (525), `weapon_knife_kukri` (526).

Unknown values are ignored and the player keeps the default knife. The knife's **paint** is a
`readyup_weapon_skins` row with `weapon_defindex` = the number in brackets.

If a Ready Up admin has no knife row, they get `weapon_knife_butterfly` by default.

## `readyup_weapon_gloves`: glove model

```sql
CREATE TABLE IF NOT EXISTS readyup_weapon_gloves (
  steamid64 BIGINT NOT NULL,
  weapon_team SMALLINT NOT NULL,
  glove_defindex INT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (steamid64, weapon_team));
```

`glove_defindex`: Broken Fang `4725`, Bloodhound `5027`, Sport `5030`, Driver `5031`,
Hand Wraps `5032`, Moto `5033`, Specialist `5034`, Hydra `5035`.

Gloves **require** a matching paint row in `readyup_weapon_skins`
(`weapon_defindex = glove_defindex`, paint e.g. `10037`). Without one, Ready Up skips the gloves,
because unpainted gloves render wrong.

## `readyup_weapon_agents`: player models

```sql
CREATE TABLE IF NOT EXISTS readyup_weapon_agents (
  steamid64 BIGINT PRIMARY KEY,
  agent_ct TEXT NULL,
  agent_t TEXT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now());
```

A value can be either of these:

- the full model path from `items_game.txt` → item `model_player`, e.g.
  `agents/models/ctm_fbi/ctm_fbi_variantb.vmdl` (Special Agent Ava | FBI) or
  `agents/models/tm_leet/tm_leet_variantf.vmdl` (The Elite Mr. Muhlik | Elite Crew)
- the short WeaponPaints form `ctm_fbi/ctm_fbi_variantb`, which Ready Up expands to
  `agents/models/<value>.vmdl`

`NULL` or empty means the game's default model. CS2 1.41 ships agent models under
`agents/models/`. The old `characters/models/...` paths are stubs, so don't use them.

## When changes take effect

- Ready Up caches each player's loadout for **45 s**. It refreshes in the background when the
  player connects, about once a second while they play, and on game events. Nothing blocks the
  game thread.
- A change applies to the **next** weapon the player gets (buy, round start, pickup of their own
  drop) and to gloves and agent on the **next spawn**. Weapons already in hand keep their current
  look.
- If the web UI needs instant refresh later, add a notify path, e.g. `LISTEN/NOTIFY` or a Ready Up
  RCON command that calls `weapon_paints::Invalidate(steamid64)`.

## Reference data

Build the picker's catalog from the game files, not from hardcoded lists:
`scripts/items/items_game.txt` has items, `paint_kits`, and `paint_kits_rarity` / collections
(for which paint fits which weapon). `resource/csgo_english.txt` has display names such as
`PaintKit_<name>_Tag` and `CSGO_CustomPlayer_<name>`. Both files are in `game/csgo/pak01_dir.vpk`.
