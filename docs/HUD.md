# Center HUD (CS2 center panel)

Ready Up draws per-player HTML with the `show_survival_respawn_status` game
event (`loc_token` = HTML). Tested in game on CS2 1.41.8.3 with `.ru hudtest 1-7`.

## Refresh rules

- The panel stays up for a short fixed time; the event's `duration` does not
  keep it open.
- Each send creates a new panel that fades in over the old one.
- Steady display: resend every frame (`hud_tick_ms=0`, `hud_resend_ms=0`,
  `hud_duration_s=1`). These are the defaults.
- Slower resends (1s/2s, 8s/10s) make the panel open and close.
- An `<img>` in a panel that is resent every frame flashes, because the image is
  reloaded on each send. Only use images in one-off sends.

## What renders

| Works | Does not work |
|---|---|
| `class='fontSize-s|sm|m|l|xl|xxl'`, `fontWeight-Bold`, `<b>` | `<i>`, `<u>` (rendered as plain text) |
| `<font color='#hex'>`, named colors (`red`) | `<span style=...>` |
| `<img src='https://...png'>` (public URL) | SVG images; `width`/`height` attributes (PNG draws at native size) |
| Raw UTF-8 (✔ ✖ ★ • →), decimal and hex entities | Named entities (`&check;`, `&star;`, `&rarr;`) |

Logos must be hosted as a PNG already scaled to the display size.

## Knife panel and chat

- After the knife round starts, the KNIFE ROUND panel stays up for `hud_knife_hold_s` seconds (default 30), also while the round is running.
- While the panel reaches players, flow updates (ready, countdown, knife start, knife winner, side pick) are shown only in the panel, not in chat. Replies that explain a refused command, admin actions and the LIVE line still go to chat. With `ready_hud=0`, or if a panel send fails, the chat messages come back.
