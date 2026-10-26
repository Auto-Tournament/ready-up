# Auto Tournament HUD addon (`at_hud`)

The Panorama layouts behind Ready Up's in-game HUD. The server spawns a `custom_hud_layout`
entity that points at one of these layouts, and players render it from this Workshop addon.
Plan and background: Vikunja #428.

This is the P0 spike: two hand-written layouts. `at_hello` is the smallest possible proof; `at_kit`
is the real HUD card, so it ships in the first publish. The component compiler that generates these
files comes in P2.

## What's here

| File | Ends up in the addon as |
|---|---|
| `layout/at_hello.xml` | `panorama/layout/custom_game/at_hello.vxml_c` |
| `styles/at_hello.css` | `panorama/styles/custom_game/at_hello.vcss_c` |
| `layout/at_kit.xml` | `panorama/layout/custom_game/at_kit.vxml_c` |
| `styles/at_kit.css` | `panorama/styles/custom_game/at_kit.vcss_c` |
| `build.ps1` | copies, compiles and checks the files above |

## Using it from the server

Set `m_strLayout` on the `custom_hud_layout` entity to the **source** path, extension included:

```
panorama/layout/custom_game/at_hello.xml
```

Not `at_hello`, and not the compiled `.vxml_c` name. Both render nothing and log nothing.

| Panel id | What the server does with it |
|---|---|
| `at_hello` | the card. Add class `hidden` to hide it. Visible by default. |
| `at_hello_text` | the label. Set dialog variable `text`. |

## `at_kit`: the HUD card

```
panorama/layout/custom_game/at_kit.xml
```

One card on the left, just above the chat. It's the only thing we draw, and only when nobody is
fighting: warmup, round end and freeze time, pauses, map changes. Never during a live round.
Design and a playable walk through a match:
[HUD kit canvas](https://claude.ai/artifact/Nnt5WzmHAQbt8bXpoYKv97) ("Play" board).

The card has a **header** (dot, kind, a few words), at most one **body** and up to two
**command chips**. All motion is CSS transitions fired by the classes below.

### Classes on `at_card`

| Class | Effect |
|---|---|
| `on` | card slides in from the left; without it, it slides out. Keep the old texts while it leaves. |
| `c-live` / `c-warn` / `c-info` / `c-ban` | state colour (dot, kind, line). None = accent orange. |
| `b-players` / `b-dots` / `b-dmg` / `b-bar` | which body shows. At most one. |
| `cmds` | shows the chip row |
| `nudge` | pulses an orange ring: this player still has to act (not ready) |
| `go` | the orange wipe crosses the card once (everyone ready). Remove it later to re-arm. |

The game itself adds `HUD_BUYMENU_VISIBLE` to the HUD root while the buy menu is open; the card
hides then without any server call.

### Header

| Id | Dialog variable | Notes |
|---|---|---|
| `at_kind_a`, `at_kind_b` | `text` | kind, e.g. `WARMUP`, `KNIFE WON`, `TECH PAUSE` |
| `at_text_a`, `at_text_b` | `text` | a few words, e.g. `6 / 10 ready` |

Two layers so text changes roll instead of jumping: write the new text into the layer that has
class `old`, then swap the classes `cur` and `old` on both layers.

### Bodies

| Body | Ids | Per-slot classes |
|---|---|---|
| `b-players` | `at_team_a`, `at_team_b` (`text`); rows `at_pa0`–`at_pa4`, `at_pb0`–`at_pb4`; names `at_pa0_name`… (`text`) | row: `ok` ready, `me` this player |
| `b-dots` | `at_da0`–`at_da4`, `at_db0`–`at_db4` | `ok` ready |
| `b-dmg` | rows `at_d0`–`at_d4`; `at_dN_name`, `at_dN_given` (`100 /3`), `at_dN_taken`, `at_dN_hp` (`text`) | row: `hit`, `big` (100+ given), `took`, `dead` |
| `b-bar` | segments `at_s0`–`at_s9`; `at_pct` (`text`) | segment: `fill` |

### Command chips

`at_c0`, `at_c1`. Each has two layers (`at_c0_a`, `at_c0_b`, with `cur`/`old` like the header),
and each layer has `at_c0_a_cmd` (`.r`) and `at_c0_a_lbl` (`ready`).

| Chip class | Look |
|---|---|
| none | secondary (dark) |
| `main` | the step to take (accent) |
| `side-ct` / `side-t` | the side the command gives you (`.stay CT`, `.switch T`) |
| `gone` | unused slot (fades out) |

Use the short commands (`.r`, `.ur`), at most two at a time.

## Rules for layouts

- Only `Panel`, `Label`, `Image` and `Button`. Anything else and the client refuses the whole layout
  (`[custom_hud] Layout contains disallowed panel type`).
- No `id` on the root panel. The compiler rejects it. Put ids on children.
- `hittest="false"` on anything that shouldn't take the mouse.
- No inline `style=""`, and quote `@keyframes` names (`@keyframes 'fade'`). Either one makes the
  layout fail to load with no error.
- `@keyframes` on `transform` don't run. Use a `transition` triggered by a class.
- Images are CSS backgrounds (`background-image: url("s2r://…vtex")`, `background-size: contain`),
  never `<Image>` with `scaling` or a `.vsvg`. Our own `.vtex` files must be BGRA8888; DXT5 renders pink.
- Width and height can't animate. Opacity, position (`transform`) and colours can, through
  `transition`. `@keyframes` only for opacity.
- Panorama caches layouts for the whole session. Restart the game after a change, and republish
  the addon before players see it.

More in [cs2-ui-kit's GOTCHAS.md](https://github.com/nvmxre/cs2-ui-kit/blob/master/docs/GOTCHAS.md) (MIT).

## Build (Windows)

You need Counter-Strike 2 with **Counter-Strike 2 Workshop Tools** (Steam › CS2 › Properties › DLC).
You'll know they're installed when a `content` folder sits next to `game` in the CS2 folder.

```bash
powershell -ExecutionPolicy Bypass -File hud/build.ps1
```

The script finds CS2 through Steam's library folders (pass `-Cs2 "<folder>"` to override), copies the
sources into `content\csgo_addons\at_hud\panorama\…`, runs `game\bin\win64\resourcecompiler.exe -i`
on each file, checks the compiled files landed in `game\csgo_addons\at_hud\panorama\…`, and deletes
the tool caches (`_bakeresourcecache`, `tools_thumbnail_cache.sqlite3`, …) so they don't get uploaded.

To test on your own client without the Workshop, copy the two compiled files into
`game\csgo\panorama\layout\custom_game\` and `game\csgo\panorama\styles\custom_game\`, then restart CS2.

## Publish (Windows)

The first upload creates the Workshop item. Later uploads update the same item.

1. Run `hud/build.ps1`.
2. Start **Counter-Strike 2 Workshop Tools** from Steam (CS2 › Launch › Workshop Tools, or
   `cs2.exe -tools`). CS2 can't be running at the same time.
3. Pick `at_hud` in the addon list and launch the tools.
4. Open the **Workshop Manager** and choose **New** (first time) or select the existing item (updates).
5. Fill in:
   - Title: `Auto Tournament HUD`
   - Description: `HUD layouts for Auto Tournament / Ready Up servers. Your game downloads this when you join.`
   - Visibility: **Unlisted** (servers can download unlisted and public items, not private/friends-only)
   - Preview image: any 512×512 JPG/PNG
6. Publish and accept the Workshop agreement if Steam asks. The item id is the number in the
   item's URL (`steamcommunity.com/sharedfiles/filedetails/?id=<id>`).

After publishing, a new item or update is available to its author at once, but game servers and
other anonymous downloads only get it after Steam finishes processing it, which can take hours.
Check with:

```bash
steamcmd +login anonymous +workshop_download_item 730 <id> +quit
```

"Access Denied" means Steam hasn't finished yet. It's not a server bug.

Later (P2): publish from a script with `steamcmd +workshop_build_item <vdf>` instead of the GUI.

## Workshop item

| | |
|---|---|
| Name | Auto Tournament HUD |
| Id | [3827905393](https://steamcommunity.com/sharedfiles/filedetails/?id=3827905393) |
| Visibility | Public |
