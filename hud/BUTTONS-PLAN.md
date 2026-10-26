# Buttons (v2): build plan

Design: canvas "Auto Tournament HUD kit", page **Buttons (v2)**
(https://claude.ai/artifact/Nnt5WzmHAQbt8bXpoYKv97): the Play board (click through a match), the
Kit board (tones, states, mouse rules) and the Every screen board (the spec, one panel per event).

## Status (2026-10-27)

Built and on instance 5 (Ready Up 0.1.0-hud.10): sections 2-4 below, except the map vote. The 57
ctests pass, and `ru hudkit demo` draws on the server (the entity is made, slot state is written).
Nothing has been seen in a client yet, because the addon update with `at_ui` isn't published.

Instance 5 is now a plain scrim server: platform prefs are match + hud on, movement and practice
off. Practice mode had made it boot into surf_utopia.

### Tomorrow, in order

1. Publish the addon update from Workshop Tools (at_hud, which now has at_ui), then unsubscribe and
   resubscribe in CS2.
2. Connect (`connect 192.168.50.196:27055`) and join CT or T. The warmup card shows bottom left with
   Ready. Press B and click Ready. The card turns green with Unready.
3. `ru hudkit demo <screen> <your slot>` for every screen: pick, pause, vote, help, admin → maps,
   confirm, mapvote, chips. Check the layout, colours, animations and that the cursor comes and
   goes.
4. Type `.menu`: you get the admin menu, a real one this time (Change map runs `.map`).
5. Run the section 1 tests below (Y chat, buy menu while captured, Tab / E / R probe).
6. Play a knife round with a bot or a friend and pick a side with the buttons.

Known gaps: the map vote (mapvote plugin) and the welcome / go-live / result cards still use center
HTML. `.help` stays essentials' chat list, so the HUD menu opens with `.menu`. With
`hud_screens=0` in readyup.cfg everything goes back to center HTML.

## 1. Test first (instance 5, scrim warmup on de_mirage)

Run these before building anything:

- `ru hudkit bclick <slot>`: B frees the cursor while the buy menu is open, and a click on CT / T
  gets to the server (`click: slot N button "at_btn_ct"` in the log).
- With the buy menu open and our capture on, buying still works.
- `ru hudkit click <slot>` in freeze time and in a pause: does forced capture feel OK? Can the player
  still buy?
- Y (chat) also frees the cursor. With chat open and **no** server capture, click a button. Does a
  click arrive? The server can't tell when chat opens, so we can't turn capture on for it.
- `ru hudkit buttons <slot> 20`: check whether Tab / E / R show up in `m_nButtons`. If one does, it can
  open the cursor in place of B.
- Capture is released on click, on round start and on disconnect.

If B fails, every screen still works with typed commands (`.r .ur .ct .t .up .y .n`). The panel just
shows the command chip in place of the button.

## 2. Addon: one generic layout (`at_ui.xml`)

This replaces `at_kit`'s centre panel and `at_click`. It is a block library with fixed slots, so new
screens need no addon update:

- `#ui_card` (left, above the chat) and `#ui_mid` (centre, lower half): the same block set, so a
  screen picks its spot with a class.
- Text: `ui_kind`, `ui_title`, `ui_sub`, `ui_timer`, `ui_counter`, `ui_hint`, each `{s:text}` with a
  `has-*` class on the panel.
- Team dots `ui_dA0..4` / `ui_dB0..4` (class `on`), with team names `ui_tA` / `ui_tB`.
- Confirm chips `ui_c0..1` (`_team`, `_state`, class `ready`).
- Command chips `ui_k0..2` (`_l` label, `_c` command).
- Buttons `ui_b0..7`, each with `_l` (label) and `_s` (small count). Classes:
  - `show`
  - a tone: `t-green`, `t-ct`, `t-t`, `t-warn`, `t-danger`, `t-ghost`
  - a state: `picked`, `off`
  - a grid: `cols-1..4` and `w-s/m/l` on the row
- `ui_close` (label `Close` / `Back`).
- Colours, sizes and motion come from the Kit board: pill 48 tall, radius 24, label 18 bold, hover
  scale 1.04, picked scale 1.06 plus a white ring, same entry and exit transitions as at_kit.
- The end screen stays `at_kit`'s `#at_end` (no buttons).

## 3. Plugin (`plugins/hud`)

- `ru_hud_v1` (append only):
  - `set_capture(hud, slot, on)`
  - `on_click(cb, user)`, called on the game thread with `(slot, layout, button_id)`
- A `screen` helper in the hud plugin turns one screen into slot writes:
  `{where, kind, title, sub, timer, counter, teams, confirm, cmds, buttons[{id,label,tone,state}], close, mouse: off|b|on}`.
  It clears the slots the previous screen used, leaves first, then enters (0.35 s, as now).
- Mouse rules in one place:
  - `off`: never capture.
  - `b`: capture on `open_buymenu` and release on `close_buymenu`. Warmup only.
  - `on`: capture now; release on click, round start, disconnect or a 30 s safety.
  - Never capture while a live round runs: round_freeze_end releases every capture.
- Panels with personal content (menus, "you're ready", your own vote) are written to that slot only.
  Shared screens go to everyone, through `RU_HUD_ALL`.

## 4. Match plugin wiring (order to build)

1. Warmup card: Ready / Unready (B), plus team dots.
2. Knife side pick: CT / T for the winners (mouse on, no close), "picking a side" for the losers.
3. Tech pause: Unpause (mouse on, everyone frozen), confirm chips, then "Waiting for Team B".
4. `.help` menu (phase-aware) and the `.ru` admin menu: Change map, Replay a round, End match with a
   confirm.
5. Votes: give up (`.gg`) and replay (`.stop`), freeze time only.
6. Map vote (mapvote plugin), using the `_s` count slot.

Each button calls the same handler as its typed command, so the chat path and the button path never
drift apart. Players without the addon keep today's HTML / chat output.
