# exoqms-ui

**The UI quality auditor for the exomind stack.** A zero-dependency C11
static analyzer (ISO honesty: it is an *approximate* static analysis, not
a browser — see Limitations). It reads an HTML file (or a directory of
`.html` files plus their linked `.css` files) and detects seven classes of
UI defects that humans and AI keep producing, so the Quality Management
System can enforce visual quality without a human looking at a screen.

Part of the [exomind stack](../../README.md) — the main README is the
full stack reference.

```
make exoqms-ui     # builds build/exoqms-ui (C11, -Wall -Wextra, 0 warnings)
make test          # runs test/test.sh (28 checks, < 60s)
```

## usage

```
exoqms-ui <target> [--json] [--no-emoji] [--emoji-allowlist <chars>]
exoqms-ui --help | --version
```

`<target>` is an HTML file, or a directory — a directory audit walks it
recursively for `.html` files and pulls in each page's `<link
rel="stylesheet">` files (relative paths resolved, remote URLs skipped
with a note) and inline `<style>` blocks.

Plain output is one finding per line, then an exact summary line:

```
major emoji-icon html > body > header.topbar > button.cart-btn emoji 🛒 in visible UI text where an icon belongs (use an SVG <use> or <img> icon instead)
=== findings: 12 (10 major) ===
```

`--json` prints a JSON array of findings (one object per line) and no
summary line. Exit codes: `0` no findings, `1` findings, `2` usage/IO
error.

### operation form (one console grammar for the whole stack)

```
exoqms-ui /audit?file=<target>&json=1&no_emoji=1&emoji_allowlist=<chars>
```

`/audit` runs one audit with the same options as the target-file CLI
form (`file` may be an HTML file or a directory — the `exoqms-ui
<target>` target), using the `/op?k=v…` syntax the daemon modules serve:

| op | params | maps to |
|----|--------|---------|
| `/audit` | `file`, `emoji_allowlist`; flags `json=1`, `no_emoji=1` | `<target> [--json] [--no-emoji] [--emoji-allowlist <chars>]` |

Values are literal (no URL decoding); `&` separates parameters. Flags
accept off values `0`/`false`/`no`/`off`. Exit codes are the CLI form's
(`0`/`1`/`2`). A path argument that starts with `/` but is not a known
operation (e.g. an absolute path) is treated as a file.

## the seven checks

| id | severity | flags | how |
|----|----------|-------|-----|
| `emoji-icon` | major | emoji characters in the visible text of interactive elements (button/a/label/summary/option) or icon-ish contexts (class/id containing `icon`, `ico`, `btn`) | UTF-8 scan of own text nodes against the emoji codepoint ranges (U+2600–27BF, U+2B00–2BFF, U+1F000–1FAFF, FE0F/20E3); `--no-emoji` disables, `--emoji-allowlist` admits characters |
| `overlap` | major | intersecting bounding boxes of interactive elements that are not nested (button/a/input/select/textarea/label/summary/option) | simplified layout boxes; intersection must exceed 2px in both axes; reports both selectors and the intersection size; geometry checks never fire on elements whose size cannot be determined |
| `misalign` | minor | siblings that should align but don't: same tag + shared class + same width and height, but different left edges (stacked layout) or different top edges (flex-row) | compares computed x (block flow) or y (flex row) of the sibling pair; tolerance 2px |
| `corner-mismatch` | minor | two adjacent siblings sharing an edge where one corner is rounded and the matching corner on the other element is square (rounded corner does not connect to a straight line) | border-radius per corner (shorthand 1-4 values, `%` resolved against min(w,h)); vertical and horizontal adjacency within 1px and 4px of shared edge |
| `background` | major | interactive elements with no background (no affordance) on the page background; background equal to the page background; hardcoded background colors that are neither the theme palette nor the page background | effective background via ancestor walk (background does NOT inherit; alpha composited over the page bg); page bg = body/html bg or white; bordered elements (any side ≥ 1px) are exempt as ghost buttons; palette = resolved `--color*` custom properties; elements using `var()` are exempt from the palette check |
| `sdk-default` | major | interactive elements (button/a/input/select/textarea) with zero CSS rules targeting them anywhere in the stylesheet | selector matching during cascade: element, `.class`, `#id`, `*`, descendant, `>`; pseudo-classes stripped; inline `style=` does not count as a rule |
| `contrast` | major | text vs effective background below WCAG AA: 4.5:1, or 3:1 for large text (font-size ≥ 24px) | sRGB relative luminance (WCAG formula) from hex/rgb/rgba/hsl/named colors; skipped when opacity < 1, background images are present, or any color is unresolvable |

## supported input subset (honest)

**HTML**: tags, attributes (class/id/style/src/href/...), text nodes,
nesting, self-closing tags (`/>` and void elements), comments, doctype,
common entity decoding. Not a full HTML5 spec parser: no implied
end-tags, unclosed tags stay open until the document ends, error
recovery is "pop until matching tag".

**CSS**: selectors `element`, `.class`, `#id`, `*`, descendant and `>`
combinators, comma lists; pseudo-classes are stripped (`:hover` rules
still count for `sdk-default`); custom properties on `:root` resolved
via `var()` (with fallbacks); declarations for color, background(-color/
-image), border-radius, width/height, min/max, margin/padding (all
shorthands), position/offsets, display, float, flex basics
(flex-direction/justify-content/align-items/align-self/flex-wrap/
flex-grow/shrink/basis), box-sizing, font-size, line-height, opacity,
visibility, border widths/colors. `@media/@import/@keyframes/@font-face`
blocks and `[attr]`/`+`/`~` selectors are skipped and counted (a note is
printed to stderr).

## layout model limitations (honest list)

This is a static approximation of the CSS box model on a fixed
1024×768 viewport. It is built to catch the seven defect classes, not
to render pixels:

- Static block flow only; floats are approximated as block placement;
  `float` wrapping of subsequent text is not modeled.
- Flex is simplified: row/column direction, top-aligned items,
  `justify-content`/`align-items` ignored (except direction); auto
  widths of flex items are estimated from text.
- Inline layout is a one-line flow with wrapping; `inline-block` boxes
  are placed on the line like inline elements.
- No text wrapping/measurement: intrinsic widths are estimated as
  0.5em per ASCII glyph (0.8em for non-ASCII); line-height defaults to
  `normal` (1.4).
- No transforms, no overflow clipping, no z-index, no `position:
  sticky`, no tables (block-approximated), no `calc()`, no `vw/vh`,
  no background images (elements with images are skipped for
  color-dependent checks).
- No margin collapsing between siblings.
- When a dimension cannot be determined at all, the box is marked
  unknown and geometry checks emit **nothing** for that element. When a
  dimension is only estimated (text-sized, percentage, content-derived
  height), the finding reason says "(approx geometry)".
- Default UA styles approximated: body margin 8px, font-size 16px;
  inline vs block display defaults follow common tags (a/span/label
  inline, button/input/img inline-block, everything else block).

False negatives are preferred over false positives: if the model cannot
decide, it stays silent.

## fixtures (permanent QA artifact)

- `fixtures/bad.html` + `bad.css` — deliberately defective, contains all
  seven defect classes: emoji icons (🛒 ⚙), overlapping buttons (relative
  `top: -30px`), misaligned siblings (`margin-left: 30px`), rounded card
  above square card, white-on-white CTA, transparent raw button, an
  off-palette hardcoded `#ff00ff`, an unstyled `<button>`, low-contrast
  text. Expected: **12 findings (10 major)**, every check fires.
- `fixtures/good.html` + `good.css` — clean page: SVG `<use>` icons,
  aligned flex rows and card grid, consistent corner radii, every
  interactive element styled from the `:root` palette, WCAG AA contrast
  everywhere. Expected: **0 findings**.

## tests

`test/test.sh` (own style, `make test`): audits both fixtures with
pinned finding counts and the exact summary line, validates `--json`
with python3, exercises `--no-emoji` and `--emoji-allowlist`, feeds
garbage (NUL bytes, unclosed tags, a 10MB single line) and asserts no
crash, checks exit codes 0/1/2, `--version`/`--help`, and directory
mode with per-file headers.

```
=== exoqms-ui tests: 28 ok, 0 fail (0s) ===
```

## integration

`exoqms-ui` is the batch auditor of the QMS loop: the daemon (B1) shells
out to it, and the pipeline (B3) collects its findings. It writes no
state of its own; findings are plain text or JSON, one per line.
