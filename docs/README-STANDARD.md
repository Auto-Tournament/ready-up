# README standard

How READMEs look across Sivert's repos (Gryt, Auto Tournament, ReadyUp and the rest). It's based on the [Gryt README](https://github.com/Gryt-chat/gryt). Copy the skeleton at the bottom and fill it in.

A README answers four things, in this order: what is this, can I use it yet, how do I get it running, where do I learn more. Everything else goes in the docs.

## Sections

Use these, in this order. Skip a section if there's nothing true to put in it. Don't add a heading just to have one.

| # | Section | Required | What goes in it |
|---|---------|----------|-----------------|
| 1 | Header | Yes | Centered logo, h1 name, bold one-liner, badge row. See below. |
| 2 | Preview / try it | No | A screenshot (`.github/preview.png`, width 700) or a link to a hosted version. Only if there's something to look at. |
| 3 | Status callout | While unstable | `> [!CAUTION]` with one or two sentences. Remove it at 1.0. |
| 4 | Intro | No | One or two short paragraphs, no heading. What it is, what it's part of, what it replaces. Name the alternatives plainly. |
| 5 | Features | Yes | Flat bullet list of things it does today. Mark unfinished items "(in progress)". |
| 6 | Install / Download | Yes | Numbered steps, or a `Platform | Link` table if there are several. Must work if you copy it exactly. |
| 7 | Usage / Self-hosting | If needed | The first thing to run after install. Link to docs for the rest. |
| 8 | Development | If people build it | Clone, one command, where it runs. |
| 9 | Documentation | Yes | Link to the docs site in bold, then a short list of in-repo docs. |
| 10 | Contributing | Yes | Link to the guide. One or two lines of policy if there is any, for example review rules or AI policy. |
| 11 | Sponsors | If set up | Between `<!-- sponsors:start -->` / `<!-- sponsors:end -->` markers. |
| 12 | Acknowledgments | If owed | Libraries and projects the repo depends on or learned from, with one line each on why. |
| 13 | License | Yes | License name and link, copyright line, one plain sentence on what it allows. Last section. |

A project-specific section (like ReadyUp's "Why it keeps working after CS2 updates" or "What's next") can go between Features and Install when readers need it to decide whether to use the thing. Keep it to a paragraph or a small table.

Sub-package READMEs in a monorepo (like `gryt/packages/*`) are shorter: header without badges (name, plus a `<p>` that says what it is and links back to the main repo), then Docker or Install, Quick start (development), Documentation. Nothing else unless it's specific to that package.

## Header

```html
<div align="center">
  <img src=".github/logo.svg" width="100" alt="NAME logo" />
  <h1>NAME</h1>
  <p><strong>One line saying what it is</strong></p>
  <p>
    <a href="https://github.com/OWNER/REPO/releases/latest"><img src="https://img.shields.io/github/v/release/OWNER/REPO?cacheSeconds=3600" alt="GitHub Release" /></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-LICENSE--NAME-blue.svg" alt="License: LICENSE-NAME" /></a>
    <a href="https://DOCS-URL"><img src="https://img.shields.io/badge/docs-DOCS--HOST-blue" alt="Docs" /></a>
  </p>
</div>

<br />
```

- Logo lives in the repo at `.github/logo.svg` (SVG, square). Width 80 to 120. Sub-packages can point at the main repo's logo by raw URL.
- Use HTML, not Markdown, inside the `<div>`. GitHub doesn't reliably render Markdown inside centered HTML blocks.
- The one-liner is a noun phrase, bold, no trailing period, no emoji.
- `<br />` after the `</div>` so the callout doesn't sit on the badges.

## Badges

One row, in this order. Only add ones that are true and useful.

1. **Release**: `img.shields.io/github/v/release/OWNER/REPO?cacheSeconds=3600`. Doesn't render while a repo is private. Keep it anyway so it works on the day it goes public.
2. **License**: static badge, `img.shields.io/badge/License-NAME-blue.svg`, linking to `LICENSE`. Escape `-` as `--` and spaces as `%20`.
3. **Docs**: `img.shields.io/badge/docs-HOST-blue`, linking to the docs site.
4. **Community** (optional): Discord, `img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white`.
5. **Stars** (optional, public repos): `img.shields.io/github/stars/OWNER/REPO`.

Distribution badges (Snap, AUR, Docker/GHCR) go on a second row, only when those channels exist.

No build-status, coverage, "made with" or language badges.

## Callout

```markdown
> [!CAUTION]
> **Early development.** NAME changes often. Expect breaking changes.
```

Use `CAUTION` for "not stable yet". Use `WARNING` for one specific thing that can hurt you (bans, data loss). Use `NOTE` for anything else. At most one callout above Features.

## Tone

- Write like you'd explain it to a friend on Discord. Short sentences, common words.
- Say what it does, not how great it is. No "powerful", "seamless", "blazing fast", "robust", "effortless", "next-generation", "unleash", "supercharge".
- No emoji in headings or bullets.
- Use "I" or "we" only where a person is talking (acknowledgments, policy). Use "you" for the reader.
- Name alternatives honestly ("does the same job as X and Y"). Don't trash them.
- If something isn't done, say so ("in progress", "not yet"). Don't list planned features as features.
- Feature bullets: start with the thing, not a verb like "Supports" or "Provides". No periods at the end.
- Commands, paths, file names and config keys go in backticks.
- Every command in the README has to work when pasted. If it depends on a path, say where to run it from.
- Keep the whole README under about 150 lines. Long reference material (every command, every config key) goes in `docs/` or the docs site.

## Files

- `README.md` at the repo root
- `.github/logo.svg` for the header
- `.github/preview.png` if there's a screenshot
- `LICENSE` at the root, `.github/CONTRIBUTING.md` for the contributing link

## Skeleton

```markdown
<div align="center">
  <img src=".github/logo.svg" width="100" alt="NAME logo" />
  <h1>NAME</h1>
  <p><strong>ONE-LINER</strong></p>
  <p>
    <!-- release, license, docs badges -->
  </p>
</div>

<br />

> [!CAUTION]
> **Early development.** NAME changes often. Expect breaking changes.

One or two sentences on what it is and what it's part of.

## Features

- ...

## Install

1. ...

## Documentation

Full docs at **[DOCS-HOST](https://DOCS-URL)**.

## Contributing

See the [contributing guide](.github/CONTRIBUTING.md).

## License

NAME is licensed under the [LICENSE-NAME](LICENSE). Copyright (c) YEAR Sivert Gullberg Hansen.
```
