# Skins loadout contract (JSON)

Ready Up has no database (docs/FLEET.md D13). The skins plugin gets player loadouts from one of
two places:

- **Standalone**: `loadouts.json` in the plugin data dir,
  `csgo/readyup/plugins/skins/loadouts.json`. Whatever manages skins (a web picker, a script,
  `scripts/seed-dev-skins.py`, `scripts/migrate-postgres-to-json.py`) writes this file. Ready Up
  only reads it.
- **Fleet mode** (a `[fleet] url` is set): the platform pushes `skins.loadout` per player over the
  fleet link (D6). The local file is not read then. See
  [`plugins/fleet/protocol/v1/messages/skins.loadout.json`](../../fleet/protocol/v1/messages/skins.loadout.json).

The file has the same four "tables" the old Postgres schema had (`readyup_weapon_skins`,
`readyup_weapon_knives`, `readyup_weapon_gloves`, `readyup_weapon_agents`). Each table is a list
of rows under the player. The column names did not change.

## File format

```json
{
  "version": 1,
  "players": {
    "76561198000000000": {
      "weapon_skins": [
        {"weapon_team": 2, "weapon_defindex": 7, "paint_id": 282, "wear": 0.15, "seed": 661},
        {"weapon_team": 0, "weapon_defindex": 9, "paint_id": 279, "wear": 0.12, "seed": 0,
         "stattrak_enabled": true, "stattrak_count": 1337},
        {"weapon_team": 3, "weapon_defindex": 16, "paint_id": 309, "wear": 0.03, "seed": 0,
         "nametag": "Ready Up test"},
        {"weapon_team": 0, "weapon_defindex": 507, "paint_id": 38, "wear": 0.01, "seed": 412},
        {"weapon_team": 0, "weapon_defindex": 5030, "paint_id": 10037, "wear": 0.2, "seed": 0}
      ],
      "weapon_knives": [{"weapon_team": 0, "knife_classname": "weapon_knife_karambit"}],
      "weapon_gloves": [{"weapon_team": 0, "glove_defindex": 5030}],
      "weapon_agents": {"agent_ct": "agents/models/ctm_fbi/ctm_fbi_variantb.vmdl",
                        "agent_t": "agents/models/tm_leet/tm_leet_variantf.vmdl"}
    }
  }
}
```

- `version` is required. Ready Up understands `1`. A newer version, broken JSON or a missing
  `version` makes Ready Up move the file to `loadouts.json.corrupt-<unix time>`, log it and run
  with no loadouts. Fix the file and put it back.
- Keys under `players` are SteamID64 strings.
- Every table is optional. Unknown keys are ignored.
- Write the file atomically: write a temp file in the same directory, then `rename()` it over
  `loadouts.json`. The helper scripts also take `flock` on `loadouts.json.lock`.

## Keys shared by the tables

| Key | Type | Meaning |
|---|---|---|
| `weapon_team` | int | `2` = T, `3` = CT, `0` = both. For each defindex (or knife/glove slot), a team-specific row wins over the team-0 row |

Only one row per (`weapon_team`, `weapon_defindex`) in `weapon_skins` and one per `weapon_team` in
`weapon_knives` / `weapon_gloves`. With duplicates the last row wins.

## `weapon_skins`: paints (weapons, knives, gloves)

| Key | Type | Default | Notes |
|---|---|---|---|
| `weapon_defindex` | int | required | Item definition index from `items_game.txt` (`items` → key). Weapons: AK-47 `7`, AWP `9`, M4A4 `16`, M4A1-S `60`, USP-S `61`, Glock-18 `4`, Desert Eagle `1`, … Knives use the **knife** defindex (Karambit `507`). Gloves use the **glove** defindex (Sport Gloves `5030`). |
| `paint_id` | int | `0` | Paint kit id (`items_game.txt` → `paint_kits` key), e.g. Redline `282`, AWP Asiimov `279`, Howl `309`, Fade `38`, Pandora's Box `10037`. `<= 0` = no paint. |
| `wear` | number | `0.000001` | Float 0..1 (FN < 0.07 ≤ MW < 0.15 ≤ FT < 0.38 ≤ WW < 0.45 ≤ BS). |
| `seed` | int | `0` | Pattern seed 0..1000. |
| `nametag` | string / null | none | Optional name tag, max 160 bytes (longer is truncated). |
| `stattrak_enabled` | bool | `false` | StatTrak counter shown on the weapon. |
| `stattrak_count` | int | `0` | Starting count. Kills are counted in `stattrak.json` (below). |

## `weapon_knives`: knife model

| Key | Type | Notes |
|---|---|---|
| `knife_classname` | string | One of the classnames below |

`weapon_bayonet` (500), `weapon_knife_css` (503), `weapon_knife_flip` (505),
`weapon_knife_gut` (506), `weapon_knife_karambit` (507), `weapon_knife_m9_bayonet` (508),
`weapon_knife_tactical` (509), `weapon_knife_falchion` (512), `weapon_knife_survival_bowie` (514),
`weapon_knife_butterfly` (515), `weapon_knife_push` (516), `weapon_knife_cord` (517),
`weapon_knife_canis` (518), `weapon_knife_ursus` (519), `weapon_knife_gypsy_jackknife` (520),
`weapon_knife_outdoor` (521), `weapon_knife_stiletto` (522), `weapon_knife_widowmaker` (523),
`weapon_knife_skeleton` (525), `weapon_knife_kukri` (526).

Unknown values are ignored and the player keeps the default knife. The knife's **paint** is a
`weapon_skins` row with `weapon_defindex` = the number in brackets.

No knife row means the stock knife. There is no default knife, for admins or anyone else.

## `weapon_gloves`: glove model

| Key | Type | Notes |
|---|---|---|
| `glove_defindex` | int | Broken Fang `4725`, Bloodhound `5027`, Sport `5030`, Driver `5031`, Hand Wraps `5032`, Moto `5033`, Specialist `5034`, Hydra `5035` |

Gloves **require** a matching paint row in `weapon_skins` (`weapon_defindex = glove_defindex`,
paint e.g. `10037`). Without one, Ready Up skips the gloves, because unpainted gloves render wrong.

## `weapon_agents`: player models

One object (not a list): `{"agent_ct": ..., "agent_t": ...}`. A value can be either of these:

- the full model path from `items_game.txt` → item `model_player`, e.g.
  `agents/models/ctm_fbi/ctm_fbi_variantb.vmdl` (Special Agent Ava | FBI) or
  `agents/models/tm_leet/tm_leet_variantf.vmdl` (The Elite Mr. Muhlik | Elite Crew)
- the short WeaponPaints form `ctm_fbi/ctm_fbi_variantb`, which Ready Up expands to
  `agents/models/<value>.vmdl`

`null` or empty means the game's default model. CS2 1.41 ships agent models under
`agents/models/`. The old `characters/models/...` paths are stubs, so don't use them.

## StatTrak: `stattrak.json`

Ready Up never writes `loadouts.json`. Kills go to `stattrak.json` next to it:

```json
{"version": 1, "counters": {"76561198000000000/0/9": 1338}}
```

The key is `<steamid64>/<weapon_team>/<weapon_defindex>` of the row that matched. Once a counter
exists it replaces `stattrak_count` from `loadouts.json`. Delete the key to reset it to the file's
value. In fleet mode the counters live on the platform: Ready Up sends `skins.stattrak`
increments (every 10 s) and shows the count from `skins.loadout` plus local kills.

## When changes take effect

- Ready Up caches each player's loadout for **45 s**. It refreshes in the background when the
  player connects, about once a second while they play, and on game events. It re-reads
  `loadouts.json` only when the file changed. Nothing blocks the game thread.
- `skins_refresh [steamid64]` (server console) refreshes now.
- `.skins reload` (chat, also `.ru skins reload`): a player re-reads their own loadout; about a
  second later their gloves, agent and the weapons they hold are re-applied. Only while nothing
  is live (idle, practice, scrim or match warmup); refused during knife rounds, live maps and
  postgame. Once per 10 s per player.
- In fleet mode a `skins.loadout` applies at once.
- A change applies to the **next** weapon the player gets (buy, round start, pickup of their own
  drop) and to gloves and agent on the **next spawn** (a respawn, a round start or
  `mp_restartgame`; a player who stays alive keeps them until then). Weapons already in hand keep their current
  look.

## Dev seed

`scripts/seed-dev-skins.py --steamid <steamid64> [--file <path to loadouts.json>]` adds a full
test loadout (AK Redline, StatTrak AWP Asiimov, M4A4 Howl with a name tag, M4A1-S Printstream,
Glock Fade, USP-S Kill Confirmed, Deagle Blaze, Karambit Fade, Sport Gloves Pandora's Box,
agents) for one player, keeping everyone else in the file.

## Reference data

Build the picker's catalog from the game files, not from hardcoded lists:
`scripts/items/items_game.txt` has items, `paint_kits`, and `paint_kits_rarity` / collections
(for which paint fits which weapon). `resource/csgo_english.txt` has display names such as
`PaintKit_<name>_Tag` and `CSGO_CustomPlayer_<name>`. Both files are in `game/csgo/pak01_dir.vpk`.
