# Skins engine surface

Everything the skins feature (weapon paints, knives, gloves, agents) touches inside Valve's
`libserver.so`, and how each item was checked against **CS2 1.41.8.3 (buildid 25492732)**,
`libserver.so` sha1 `9da9450a493fcc8e811ff19688ebff207dc6004f`.

Code: `core/src/readyup/skins_engine.{h,cpp}` (signatures, entity system),
`core/src/readyup/schema.cpp` (field offsets), `core/src/readyup/weapon_paints_apply.cpp` (weapons, knives,
per-tick driver), `core/src/readyup/weapon_paints_cosmetics.cpp` (gloves, agents).

## How it runs

All engine calls happen in `weapon_paints::GameFrameTick()`, called from the GameFrame vtable hook
**after** the original `GameFrame`. The server sends this frame's snapshot after that. So a weapon
created this frame (a buy, the round-start give, a pickup) gets its skin before any client sees it.

- Every tick: controllers 1..64 → `m_hPlayerPawn` → pawn → `m_pWeaponServices->m_hMyWeapons`.
  Each weapon handle is decorated once. The owner's loadout comes from the async Postgres cache.
- Spawn (pawn `m_lifeState` goes to alive, or a new pawn handle): agent model and gloves, applied
  on that frame and again on the next frame.
- Anything decorated after its first snapshot (loadout still loading, plugin loaded mid-round,
  gloves on a pawn that persists across rounds) is re-sent with `MarkEntityFullyChanged`.
- Nothing here depends on game events or detours. Events only prefetch loadouts, and the
  `player_death` StatTrak counter still needs them.

Every function resolves through the repo-owned engine surface: `gamedata/engine-surface.json`,
embedded at build time, read with `EngineFunction(name)`. An entry only resolves if its signature
matches **exactly once** and **every identity anchor passes**. If an item doesn't resolve, the
feature that needs it is skipped and the rest keeps working. The CounterStrikeSharp CDN gamedata
is not used.

After a CS2 update, check the whole surface offline:

```
build/readyup_sigcheck <path to Valve libserver.so> gamedata/engine-surface.json
```

The first active tick always prints one status line per skins item:

```
[ReadyUp] skins: CAttributeList::SetOrAddAttributeValueByName   OK       built-in signature
```

### How items were verified

- **Signatures**: each pattern was scanned over the executable `PT_LOAD` segments of the real
  binary and has exactly one match. The function behind it was then anchored to its purpose:
  string xrefs, a known call site, or a vtable slot resolved via RTTI and `R_X86_64_RELATIVE`
  relocs.
- **Schema**: an offline harness loads `libtier0`, `libschemasystem` and `libserver` from the build,
  calls `InstallSchemaBindings("SchemaSystem_001", …)`, then runs Ready Up's **real**
  `schema.cpp` and `skins_engine.cpp` against it. Signature resolution is checked the same way,
  against the loaded image.

## Signatures and addresses

RVAs are offsets into `libserver.so` 1.41.8.3. **engine-surface key** is the `functions` key in
`gamedata/engine-surface.json`. The anchor column lists the identity anchors that
`readyup_sigcheck` enforces, followed by how the function was identified by hand.

| engine-surface key | Anchors (checked by sigcheck) |
|---|---|
| `CAttributeList_SetOrAddAttributeValueByName` | `caller_string` "set item texture prefab" (via_thunk) |
| `CBaseEntity_ChangeSubclass` | `caller_string` "ChangeSubclass" (the input handler) |
| `CBaseModelEntity_SetModel` | `caller_string` "models/chicken/chicken.vmdl" |
| `CBaseModelEntity_GetModel` | `caller_string` "models/hostage/hostage.vmdl" |
| `CModel_FindBodygroupByName` | `caller_string` "bodygroup" (Pulse SetBodygroupByName binding) |
| `CBaseModelEntity_SetBodygroup` | `string_ref` "CBaseModelEntity::SetBodygroup(%d,%d) failed: iGroup outside the range…" |
| `CBaseEntity_NetworkStateChanged` + vtable index `CEntityInstance::NetworkStateChanged` = 29 | `callee_string` "CNetworkTransmitComponent::StateChanged(%s) @%s:%d". This is a new anchor type: the function calls a private helper that references the string. |
| `UTIL_Remove` (entity system anchor) | `caller_string` "particles/inferno_fx/extinguish_fire.vpcf" |

Hand verification:

| Item | RVA | Status | Anchor / notes | Resolution (history) |
|---|---|---|---|---|
| `CAttributeList::SetOrAddAttributeValueByName(this, name, float)` | `0x1caa060` | **was missing**: no gamedata key; WeaponPaints file not shipped | Only callee of thunk `0x1caa260` (`add rdi,0x70; jmp`) = `CEconItemView::…` (`+0x70` = `m_AttributeList`). That thunk is called with the literal strings `"set item texture prefab"`, `"…seed"`, `"…wear"`. Body calls `GetItemSchema()` then `GetAttributeDefinitionByName(name)` and loops `m_Attributes` (stride 0x48). | Built-in `55 48 89 E5 41 57 41 56 41 55 49 89 FD 41 54 53 48 89 F3 48 83 EC 78 F3 0F 11 85 6C FF FF FF` (1 match) |
| `CBaseEntity::ChangeSubclass(this, const char*)` (knives) | `0xd61280` | **new** (replaces broken AcceptInput) | Called by the `subclass_change` console command right before it logs `"subclass_change: Changed entity %d from subclass %s -> subclass %s"`. Knives use the defindex string (`"507"`), same as the `ChangeSubclass` input WeaponPaints fires. | Built-in `55 48 89 E5 41 57 49 89 F7 41 56 41 55 41 54 53 48 89 FB 48 81 EC A8 00 00 00` |
| `CEntityInstance_AcceptInput` (gamedata) | n/a | **broken**: CDN/CSS pattern has 0 matches. The old code also passed a `const char*` where the engine takes `variant_t*` | Not needed any more | Removed. Knife uses ChangeSubclass; bodygroup uses the three calls below |
| `CBaseModelEntity::SetModel(this, const char*)` (agents) | `0x163d280` | OK: gamedata sig matches once | Called from pawn spawn code (`0xaea0dd`) with a non-empty string in `rsi`. Body: resource lookup via `[rip+X]->vfunc(0x68)`, then set model by handle. | Built-in added: gamedata pattern incl. trailing `E9 ? ? ? ? CC CC CC CC 55`. Without that tail it matches 2x |
| `CBaseModelEntity::GetModel(this)` | `0xd43180` | **new** | 1st call in `CBaseModelEntity::InputSetBodyGroup` (xref `"%s: unexpected parameter format for InputSetBodyGroup…"`) | Built-in (48-byte pattern) |
| `CModel::FindBodygroupByName(model, name)` | `0x1e86040` | **new** | 2nd call in InputSetBodyGroup; result `< 0` = not found | Built-in `55 48 89 E5 41 57 41 56 41 55 49 89 FD 41 54 53 31 DB` |
| `CBaseModelEntity::SetBodygroup(this, int, int)` | `0x1676780` | **new** | 3rd call in InputSetBodyGroup. Own function contains `"CBaseModelEntity::SetBodygroup(%d,%d) failed…"` | Built-in `85 F6 0F 88 ? ? ? ? 55 48 89 E5 41 57 41 56` |
| `CEntityInstance::NetworkStateChanged(data)`, vtbl **[29]** | `0xa3fa30` | **new** | Gamedata `NetworkStateChanged` (chain helper, `0x2293d60`, 1 match) dispatches through `[vtbl+0xE8]` = slot 29. Slot 29 is `0xa3fa30` for CBaseEntity, CBaseModelEntity, CBasePlayerPawn, CCSPlayerPawn(Base), CEconEntity, CBasePlayerWeapon, CCSWeaponBase(Gun) and CKnife (RTTI). When `data.count` (dword +0) is 0, it ORs `FL_FULL_EDICT_CHANGED\|FL_EDICT_CHANGED` into the transmit flags. | Built-in pattern. At runtime we call `vtbl[29]` only if it equals the resolved address |
| `g_pGameEntitySystem` | global `0x2bac8d0` | **new** | `UTIL_Remove` (gamedata sig, 1 match) = `lea rax,[rip+X]; mov rdi,[rax]; jmp CEntitySystem::…`. `UTIL_CreateEntityByName` and the inline handle lookups use the same global. | Resolved from the UTIL_Remove `lea` at +8 (built-in + gamedata) |
| `GiveNamedItem` / `CCSPlayer_ItemServices` vtbl[24] | `0x1588d60` / thunk `0x15897d0` | OK, **not used** | Contains `"GiveNamedItem: interpreting '%s' as '%s'"`. vtbl[24] thunk zeroes args, then jumps here. **Only accepts the 48 stock classnames** (alias table at `0x27894c0`), so `weapon_knife_karambit` returns NULL. That's why knives use ChangeSubclass. | none |
| `CBasePlayerPawn_RemovePlayerItem` | `0x15de9d0` | OK, not used | Detaches, then tail-calls `UTIL_Remove` | none |
| `CGameEventManager_Init` | `0x171dd20` | CDN sig matches once | Owned by the hooks work (see below) | none |
| GameFrame hook = `ISource2Server` vtbl[19] | `0x1996e10` | OK | `CSource2Server` vtbl (RTTI) slot 19 references the `"GameFrame"` VPROF string and takes 3 bools | none |

## Entity system layout (hardcoded, verified)

| Item | Value | Evidence |
|---|---|---|
| identity chunks | `CEntitySystem + 0x10`, `chunk[idx >> 9]` | Inline lookups at e.g. `0xa3d21b`: `sar edx,9; mov rsi,[r8+rdx*8+0x10]` |
| identity stride | `0x70` | `and edx,0x1ff; imul rdx,rdx,0x70`. Schema: `sizeof(CEntityIdentity) == 112` |
| handle in identity | `+0x10` (u32), index = low 15 bits | `movzx edi,word [rsi+0x10]; and edi,0x7fff`. Full compare of the 32-bit handle (serial) for `CHandle` lookups |
| entity from identity | `+0x00` (`m_pInstance`) | Standard layout |
| sanity check | entity 0 designer name == `"worldent"` | Checked at runtime. Skins turn off if it fails |

## Schema fields

Looked up by name at runtime (`SchemaFindOffset`). Offsets below are for 1.41.8.3, for reference only.

| Class::field | Offset | Declared in | Used for |
|---|---|---|---|
| `CBasePlayerController::m_steamID` | 0x9e8 | CBasePlayerController | owner of pawn |
| `CCSPlayerController::m_hPlayerPawn` | 0xbc4 | CCSPlayerController | pawn handle |
| `CBaseEntity::m_iTeamNum` | 0x624 | CBaseEntity | team (2/3) |
| `CBaseEntity::m_lifeState` | 0x5b8 | CBaseEntity | spawn detection |
| `CBasePlayerPawn::m_pWeaponServices` | 0xdf0 | CBasePlayerPawn | inventory |
| `CPlayer_WeaponServices::m_hMyWeapons` | 0x48 | CPlayer_WeaponServices | `{int size; CHandle* elems @+8}` |
| `CEconEntity::m_AttributeManager` | 0xd38 | CEconEntity | → `CAttributeContainer` |
| `CAttributeContainer::m_Item` | 0x50 | CAttributeContainer | → `CEconItemView` |
| `CEconEntity::m_nFallbackPaintKit / Seed / m_flFallbackWear / m_nFallbackStatTrak` | 0x1178 / 0x117c / 0x1180 / 0x1184 | CEconEntity | fallback paint |
| `CEconEntity::m_OriginalOwnerXuidLow / High` | 0x1170 / 0x1174 | CEconEntity | picked-up weapons keep the original owner's skin |
| `CEconItemView::m_iItemDefinitionIndex` (u16) | 0x38 | CEconItemView | defindex |
| `CEconItemView::m_iEntityQuality` | 0x3c | | 3 = ★ for knives |
| `CEconItemView::m_iItemID / m_iItemIDHigh / m_iItemIDLow / m_iAccountID` | 0x48 / 0x50 / 0x54 / 0x58 | | non-zero fake item id so clients read attributes |
| `CEconItemView::m_bInitialized` | 0x68 | | gloves |
| `CEconItemView::m_AttributeList / m_NetworkedDynamicAttributes` | 0x70 / 0xe8 | | paint attributes |
| `CEconItemView::m_szCustomName` | 0x160 | | name tag |
| `CCSPlayerPawn::m_EconGloves / m_nEconGlovesChanged` | 0x1320 / 0x1708 | CCSPlayerPawn | gloves |
| `CEntityIdentity::m_designerName` / `CEntityInstance::m_pEntity` | 0x20 / 0x10 | entity2 | classnames |

### `schema.cpp` was broken on this build (fixed)

The old code walked `CSchemaSystem`'s scope array at a hardcoded `+0x188`. On 1.41.8.3 that reads
garbage (the "scope count" is a pointer value), so every lookup failed or read out of bounds. It
also didn't walk base classes, so fields like `CBasePlayerWeapon::m_nFallbackPaintKit` (declared
on `CEconEntity`) could never resolve.

The new code:

- calls `ISchemaSystem::FindTypeScopeForModule("libserver.so")` (vtbl 13) and
  `CSchemaSystemTypeScope::FindDeclaredClass(name)` (vtbl 2)
- walks base classes
- detects the `SchemaClassInfoData_t` layout at runtime. This build inserted a pointer at +0x18,
  so size/fieldCount/baseCount/fields/bases are now at 0x20/0x24/0x29/0x30/0x38. The legacy
  layout is still supported.

This also fixes the other `SchemaFindOffset` users in `game_events.cpp`. For example,
`CCSPlayerController::m_iTeamNum` now resolves. `CCSPlayerController::m_iKills` does not exist in
this build, so its caller keeps falling back to event counting.

## Game events

| Event | Before | Now |
|---|---|---|
| `player_spawn` | applied gloves/agents (and forced a default agent on everyone) | prefetch only. Cosmetics come from the tick |
| `item_equip` / `item_pickup` | tried `GetEntity("item")`, but `item` is a classname string, so nothing ever applied | prefetch only |
| `player_death` | StatTrak +1 | unchanged, except it now updates the row that actually matched (team 0 rows included) |

## What still depends on the hooks work (`fix/hooks-and-signatures`)

- **StatTrak counting** needs `player_death`, so it needs a working game event manager. Skins,
  knives, gloves and agents do not.
- A future GiveNamedItem detour could replace polling. Polling already hits the
  pre-snapshot window, so it isn't required.
- If that branch moves the GameFrame hook to a detour, keep calling
  `weapon_paints::GameFrameTick()` **after** the original.

## Remaining risks

- `MarkEntityFullyChanged` builds `NetworkStateChangedData` with a zero offset count and path index
  `-1` at +0x38. The zero-count branch was read from the disassembly, but hasn't run live yet.
  If late re-networking misbehaves, set `READYUP_DISABLE_SKINS=1` to turn the feature off.
- Agent models go through `SetModel` with no explicit precache. All `agents/models/*.vmdl` ship
  in the VPK, but whether the server precaches every agent is unverified.
- Glove visuals depend on the client re-reading `m_EconGloves` after `m_nEconGlovesChanged`
  changes plus the full re-send. This matches what WeaponPaints does, but hasn't been tested here.

## Legacy paint kits

884 of the 1481 paint kits in 1.41.8.3 (for example Redline 282, AWP Asiimov 279, Howl 309) have
`"use_legacy_model" "1"` in `items_game.txt`. They were made for the CS:GO weapon UVs, so in CS2
they only look right on the weapon's legacy model, which is bodygroup `body` = 1. Fade, Printstream
and other CS2-native kits are not legacy.

- **Table**: the ids are in `core/src/readyup/legacy_paint_kits.inc`. After a game update, regenerate
  it with `scripts/gen_legacy_paint_kits.py <items_game.txt> <version>`.
- **Apply**: when a weapon gets a legacy paint, Ready Up sets `body` = 1 through the
  GetModel → FindBodygroupByName → SetBodygroup chain.
- **Retry**: if the weapon's model isn't loaded yet, Ready Up retries on later ticks and then
  re-sends the entity to clients.

## Glove bodygroup timing

The pawn's `default_gloves` bodygroup does exist on agent models. But on the spawn frame the model
behind the pawn's model handle is often not loaded yet, especially right after the agent
`SetModel`. `GetModel` then returns null and there is nothing to set the bodygroup on.

Cosmetics are therefore applied on ticks 0, 1, 8 and 32 after the spawn. The debug log gives the
exact reason for each outcome: `entity has no loaded model`, `model has no such bodygroup`, or
`default_gloves bodygroup set`.
