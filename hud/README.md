# Auto Tournament HUD addon (`at_hud`)

The Panorama layouts behind Ready Up's in-game HUD. The server spawns a `custom_hud_layout`
entity that points at one of these layouts, and players render it from this Workshop addon.
Plan and background: Vikunja #428.

This is the P0 spike: one hand-written layout. The component compiler that generates these files
comes in P2.

## What's here

| File | Ends up in the addon as |
|---|---|
| `layout/at_hello.xml` | `panorama/layout/custom_game/at_hello.vxml_c` |
| `styles/at_hello.css` | `panorama/styles/custom_game/at_hello.vcss_c` |
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
| Id | _not published yet_ |
| Visibility | Unlisted |
