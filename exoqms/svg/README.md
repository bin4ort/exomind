# exoqms-svg

**The asset-logic QMS auditor for generated SVG.** A zero-dependency C11
static analyzer (ISO honesty: an *approximate* geometry analysis, not a
renderer — see Limitations). It reads an SVG file (or a directory of
`.svg` files) and checks the **logic** of the shapes with geometry, not
taste: "an AI made tree SVG, unless it's as simple as a tapered stem and
a round crown, is almost certainly anything but an actual tree."
Generated graphics look plausible but are structurally wrong; this
module catches that. v0.1.0 ships the **tree** rule-set; shape kinds
are pluggable rule-sets behind the same `audit_run` entry point.

```
make exoqms-svg   # builds build/exoqms-svg (C11, -Wall -Wextra, 0 warnings)
make test         # runs test/test.sh (54 checks, < 60s)
```

## usage

```
exoqms-svg <target> [--shape tree|auto] [--json]
exoqms-svg --help | --version
```

`<target>` is one `.svg` file, or a directory — a directory audit walks
it recursively for `.svg` files. `--shape` selects the rule-set:
`tree` forces it, `auto` (the default) detects it from the root `<svg>`
`data-shape` attribute (e.g. `<svg data-shape="tree">`) or, failing
that, from a filename containing "tree" (case-insensitive). If `auto`
finds no hint the file is **skipped**: plain mode prints one line
`skip unknown-shape <path>: no tree hint (no data-shape="tree" on root
<svg>, filename has no "tree")`, it does not count as a finding and the
exit code stays 0. This is the documented behavior for `house.svg` in
the fixtures: with `--shape auto` it is skipped; with `--shape tree`
the tree rules run anyway and flag it (2 major + 1 minor, pinned in the
tests).

Plain output is one finding per line — `<severity> <check-id>
<reason>` — then an exact summary line:

```
minor symmetry crown leans 4830/12841 (left/right area about trunk axis x=156.0, balance ratio 0.38, want ≥ 0.6)
=== findings: 1 (0 major) ===
```

`--json` prints a JSON array of finding objects (`file`, `shape`,
`severity`, `check`, `reason`; one object per line) and no summary
line. Exit codes: `0` no findings, `1` findings, `2` usage/IO error.
Skipped files never produce findings in `--json` mode.

## the tree rule-set (one line per check, with the math)

| id | severity | flags | how |
|----|----------|-------|-----|
| `stem-taper` | major | trunk is a parallel-sided box (ratio > 0.9) or a spike (< 0.15) | trunk = the elements whose bbox center lies below the crown split; stem width = the horizontal extent of the trunk segments crossing a horizontal line (cross-section), measured at 10% (top slice) and 90% (bottom slice) of the trunk bbox height; a tapered stem is narrower at top: `ratio = width_top / width_bottom` must lie in [0.15, 0.9] |
| `stem-missing` | major | nothing below the crown region at all | trunk element count == 0; reported even on degenerate input |
| `crown-roundness` | major | crown bbox aspect outside [0.75, 1.5]; or convexity < 0.8; or a box-shaped crown | crown = elements in the upper region (see region split); aspect = `crown_w / crown_h`; convexity = `union area / convex hull area`, hull by monotone chain (Andrew) over all sampled crown points, areas by shoelace, clamped to 1.0; union area = sum of the crown element areas (overlaps counted twice — see Limitations). A single crown element filling ≥ 85% of its own bbox is a box-shaped crown (a square has convexity 1.0, so convexity alone cannot flag it — documented extension) |
| `proportions` | minor | `trunk_h / crown_h` outside [0.15, 0.6], or `crown_w / total_h` outside [0.4, 1.6] | measured on the trunk/crown/total bounding boxes |
| `symmetry` | minor | crown left/right area balance ratio < 0.6 about the trunk axis | axis = trunk bbox center x; each crown element's area is split left/right by the fraction of its bbox on each side of the axis; `ratio = min(left,right) / max(left,right)`; skipped when the stem is missing |
| `empty-shape` | major | total painted area < 0.5% of the total bbox area (a single line, a stick, an empty `<svg>`) | degenerate input: the crown/trunk shape checks are then skipped (geometry is meaningless); `stem-missing` is still reported |
| `fragmented` | minor | more than 8 disconnected stroke groups | connected components (union-find) over path/polygon/polyline/line elements, two elements linked when any two of their points are within 2% of the bbox diagonal; computed on ≤ 4000 points (beyond that the check stays silent) |
| `out-of-bounds` | minor | element entirely outside the svg `viewBox` | element bbox vs `viewBox x y w h`; no viewBox → check skipped |

**Region split heuristic (documented):** crown = the top 65% of the
total bbox height; an element belongs to the crown when its bbox center
lies at or above the split line, otherwise it is trunk material. When
the resulting crown bbox extends below the top 70% of the total height
(the 65% line cut through the canopy), trunk elements that still sit
inside the crown's horizontal span and do not extend below the crown
bottom are reclassified as crown.

## api (CLI surface)

| method | signature | purpose |
|--------|-----------|---------|
| batch | `exoqms-svg <file.svg>` | audit one file, tree rule-set via auto-detection |
| batch | `exoqms-svg <dir>` | audit every `.svg` under the directory, per-file headers + totals |
| batch | `--shape tree` | force the tree rule-set |
| batch | `--shape auto` | detect via `data-shape` attribute or filename (default) |
| batch | `--json` | JSON array output instead of plain text |
| batch | `--version` / `--help` | version / usage |

The audit engine is a library of one entry point, `audit_run()` (see
`src/svg.h`): shape kinds are pluggable rule-sets behind it, so a new
shape kind (e.g. `house`) is a new detection string + one rule function,
not a CLI change. The module is a batch auditor; it writes no state of
its own — findings are plain text or JSON, one finding per line, ready
for the QMS daemon and pipeline to consume like `exoqms-ui`'s.

## design

Five files, no dependencies beyond libc + `-lm`: `main.c` (CLI),
`util.c` (memory/strings/file/dir walk), `svgparse.c` (the SVG subset
parser: XML-lite tokenizer + path-data sampler + affine transforms),
`geom.c` (shoelace, monotone-chain convex hull) and `checks.c` (the
tree rule-set + shape detection). Elements are sampled to world-coord
points/segments once at parse time; every check runs on those points —
no second parser, no state between audits. Sampling: `C/S` → 8 points,
`Q/T` → 4, `A` → 8 (center parameterization), circles/ellipses → 16
perimeter points (their bbox corners are included in the samples),
`rect` → 4 corners, `polygon`/`polyline`/`line` → their vertices.
Paths are treated as filled: open subpaths are closed implicitly for
area and cross-section, matching SVG fill semantics.

## supported input subset (honest)

**Elements**: `svg`, `g` (nested, flattened), `path`, `circle`,
`ellipse`, `rect`, `line`, `polygon`, `polyline`. **Attributes**: `id`,
`d`, `cx/cy/r`, `rx/ry`, `x/y`, `x1/y1/x2/y2`, `width/height`,
`points`, `transform`, `viewBox`, `data-shape`. **Transforms**:
`translate`, `scale` and `rotate` (with optional center) are composed
and applied; `matrix`, `skewX`, `skewY` are **skipped with a note** on
stderr. **Path commands**: `M/m L/l H/h V/v C/c S/s Q/q T/t A/a Z/z`
with implicit repetition; relative coordinates supported; exponents
(`1e2`) supported. **Not supported**: `<use>`, `<defs>` contents (their
subtrees are ignored entirely — they are not rendered), `<text>`,
gradients/markers/clips, `style`/`fill`/`stroke` (colors are ignored;
`fill="none"` is not honored — all paths are treated as filled),
rounded-rect `rx/ry`, viewBox `preserveAspectRatio`. `garbage/no-d`
input skips the element with no finding; files are truncated at 16 MiB,
element/point counts are capped (20000 elements, 200000 points per
element) so pathological input never exhausts memory.

## layout model limitations (honest list)

- Union area = **sum of element areas**: overlapping crown elements are
  double-counted (convexity is clamped to 1.0, so this only errs toward
  "round enough"). The convexity check applies to the union only when
  the crown has ≥ 3 sampled points; with fewer, crown geometry checks
  stay silent.
- Convexity uses the sampled boundary (16-gon ≈ circle), so a disc
  measures ≈ 1.0; a 4-corner-only approximation would measure π/4 and
  falsely fail — documented choice.
- Stem width is a segment cross-section of the sampled boundary, so
  curves are measured via their samples (chords, slightly narrower than
  the true arc).
- Crown/trunk classification is by bbox center against the 65% split:
  foliage scatter far below the canopy midline can be counted as trunk
  material (see the region-split heuristic above).
- The taper check needs the trunk to be at least 1px tall and to have
  measurable width at both slices; otherwise it stays silent.
- Out-of-bounds flags only elements entirely outside the viewBox;
  partial overflows are common and not flagged.
- False negatives are preferred over false positives: when the model
  cannot decide, it stays silent.

## fixtures (permanent QA artifact)

| fixture | geometry | expected findings |
|---------|----------|-------------------|
| `tree-good.svg` | tapered stem path (12px top / 32px bottom, 95px tall) + round crown (r=85 circle + 2 symmetric foliage dots) | **0 findings** — the standard every generated tree must meet |
| `tree-stick.svg` | single vertical line | `empty-shape` major + `stem-missing` major |
| `tree-square-crown.svg` | square rect crown over a tapered stem | `crown-roundness` major only |
| `tree-box-stem.svg` | round crown over a 20px-wide parallel-sided trunk | `stem-taper` major only |
| `tree-too-lean.svg` | huge crown (r=90) on a 22px trunk stub | `proportions` minor only |
| `tree-asym.svg` | crown shifted right of the trunk axis (center x=190 vs axis x=156) | `symmetry` minor only |
| `house.svg` | rect body + triangle roof, no tree hint | `--shape auto`: skipped (exit 0); `--shape tree`: `stem-taper` + `crown-roundness` major, `proportions` minor |

## tests

`test/test.sh` (own style, `make test`): pins the finding counts and
severities of every fixture, the exact summary line and exit codes,
validates `--json` with python3 (fields and ids), exercises `--shape
auto` skipping (filename, `data-shape` attribute, unknown hint),
transforms (translate/scale keep 0 findings; translate+rotate doesn't
crash), the strange-geometry extras (out-of-bounds, fragmented,
empty-shape), feeds garbage (NUL bytes, garbage `d` data, unclosed
tags, a 10MB single line, an `r`-less circle) and asserts no crash,
checks `--version`/`--help`, unknown options, and directory mode with
per-file headers and the exact summary.

```
=== exoqms-svg tests: 54 ok, 0 fail (1s) ===
```

## integration

`exoqms-svg` is the asset-logic auditor of the QMS loop: like
`exoqms-ui` it is a batch tool the exoqms daemon shells out to; its
findings are one per line in the same `severity check-id reason`
vocabulary, so the pipeline can collect them identically. It writes no
state of its own.
