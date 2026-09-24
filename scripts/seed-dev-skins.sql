-- Dev seed: a full skins loadout for one player.
-- Usage: psql -v steamid=<your steamid64> -f scripts/seed-dev-skins.sql
--
--   docker exec -i readyup-postgres psql -U readyup readyup < scripts/seed-dev-skins.sql
--
-- Idempotent (upserts). Table DDL is copied verbatim from libs/readyup/postgres.cpp
-- (EnsureWeaponPaintsSchemaLocked) so this works on an empty database too.
-- Contract: docs/skins-db-contract.md. IDs verified against items_game.txt of CS2 1.41.8.3.
--
-- weapon_team: 2 = T, 3 = CT, 0 = both (team-specific rows win, per defindex).

BEGIN;

CREATE TABLE IF NOT EXISTS readyup_weapon_skins (  steamid64 BIGINT NOT NULL,  weapon_team SMALLINT NOT NULL,  weapon_defindex INT NOT NULL,  paint_id INT NOT NULL,  wear REAL NOT NULL DEFAULT 0.000001,  seed INT NOT NULL DEFAULT 0,  nametag TEXT NULL,  stattrak_enabled BOOLEAN NOT NULL DEFAULT FALSE,  stattrak_count INT NOT NULL DEFAULT 0,  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),  PRIMARY KEY (steamid64, weapon_team, weapon_defindex));
CREATE TABLE IF NOT EXISTS readyup_weapon_knives (  steamid64 BIGINT NOT NULL,  weapon_team SMALLINT NOT NULL,  knife_classname TEXT NOT NULL,  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),  PRIMARY KEY (steamid64, weapon_team));
CREATE TABLE IF NOT EXISTS readyup_weapon_gloves (  steamid64 BIGINT NOT NULL,  weapon_team SMALLINT NOT NULL,  glove_defindex INT NOT NULL,  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),  PRIMARY KEY (steamid64, weapon_team));
CREATE TABLE IF NOT EXISTS readyup_weapon_agents (  steamid64 BIGINT PRIMARY KEY,  agent_ct TEXT NULL,  agent_t TEXT NULL,  updated_at TIMESTAMPTZ NOT NULL DEFAULT now());

-- Weapon + knife + glove paints. Knife and glove paints live here too, keyed by the knife/glove
-- defindex (507 = Karambit, 5030 = Sport Gloves).
INSERT INTO readyup_weapon_skins
  (steamid64, weapon_team, weapon_defindex, paint_id, wear, seed, nametag, stattrak_enabled, stattrak_count)
VALUES
  -- defindex  paint                                    wear    seed
  (:steamid, 2,    7,   282, 0.15,     661, NULL,           FALSE, 0),     -- AK-47 | Redline (FT)
  (:steamid, 0,    9,   279, 0.12,     0,   NULL,           TRUE,  1337),  -- StatTrak AWP | Asiimov
  (:steamid, 3,   16,   309, 0.03,     0,   'Ready Up test', FALSE, 0),     -- M4A4 | Howl, name tag
  (:steamid, 3,   60,   984, 0.02,     0,   NULL,           FALSE, 0),     -- M4A1-S | Printstream
  (:steamid, 2,    4,    38, 0.01,     0,   NULL,           FALSE, 0),     -- Glock-18 | Fade
  (:steamid, 3,   61,   504, 0.05,     0,   NULL,           FALSE, 0),     -- USP-S | Kill Confirmed
  (:steamid, 0,    1,    37, 0.02,     0,   NULL,           FALSE, 0),     -- Desert Eagle | Blaze
  (:steamid, 0,  507,    38, 0.01,     412, NULL,           FALSE, 0),     -- ★ Karambit | Fade
  (:steamid, 0, 5030, 10037, 0.20,     0,   NULL,           FALSE, 0)      -- ★ Sport Gloves | Pandora's Box
ON CONFLICT (steamid64, weapon_team, weapon_defindex) DO UPDATE SET
  paint_id = EXCLUDED.paint_id,
  wear = EXCLUDED.wear,
  seed = EXCLUDED.seed,
  nametag = EXCLUDED.nametag,
  stattrak_enabled = EXCLUDED.stattrak_enabled,
  stattrak_count = EXCLUDED.stattrak_count,
  updated_at = now();

INSERT INTO readyup_weapon_knives (steamid64, weapon_team, knife_classname)
VALUES (:steamid, 0, 'weapon_knife_karambit')
ON CONFLICT (steamid64, weapon_team) DO UPDATE SET knife_classname = EXCLUDED.knife_classname, updated_at = now();

INSERT INTO readyup_weapon_gloves (steamid64, weapon_team, glove_defindex)
VALUES (:steamid, 0, 5030)
ON CONFLICT (steamid64, weapon_team) DO UPDATE SET glove_defindex = EXCLUDED.glove_defindex, updated_at = now();

-- Special Agent Ava | FBI (CT), The Elite Mr. Muhlik | Elite Crew (T).
INSERT INTO readyup_weapon_agents (steamid64, agent_ct, agent_t)
VALUES (:steamid,
        'agents/models/ctm_fbi/ctm_fbi_variantb.vmdl',
        'agents/models/tm_leet/tm_leet_variantf.vmdl')
ON CONFLICT (steamid64) DO UPDATE SET agent_ct = EXCLUDED.agent_ct, agent_t = EXCLUDED.agent_t, updated_at = now();

COMMIT;
