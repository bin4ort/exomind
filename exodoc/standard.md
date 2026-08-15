# exodoc standard v0.1.0 — the documentation standard for the AI-native stack

Normative document. In the spirit of ISO 9001:2015 clause 7.5
("documented information": creation, identification, availability and
conformance of documentation), this standard defines what a component's
documentation must contain so that it is machine-checkable. Every check
in this standard is implemented by `exodoc audit`; a component is
documentation-compliant when every applicable check passes.

## 1. Scope

Every component of the exomind stack (exomind, exosched, exoflow, and
any future component) SHALL ship a `README.md` in its component
directory. The stack manifest `docs/stack.tsv` enumerates components
(see section 7). A daemon component additionally SHALL expose a
self-describing `GET /` spec (identification and availability of its
documented API, 7.5.3).

## 2. Identification (7.5.2 — creating and updating)

- **c1 purpose** — the document SHALL open with a top-level heading
  `# <component>` (the heading SHALL name the component as given in the
  manifest, case-insensitive) followed by a non-empty first paragraph
  describing the component's purpose. A fenced code block or a second
  heading before the paragraph does not count.
- **c8 version** — the document SHALL contain a version token matching
  the pattern `v?X.Y.Z` (three dot-separated numeric parts; leading `v`
  optional). IP-address-shaped tokens (`127.0.0.1`), dates
  (`YYYY.MM.DD`) and other numeric collocations are not version tokens.
  The FIRST version token in the document is the documented version.

## 3. Section requirements (7.5.2 — documented information SHALL be
available and suitable)

Required sections, detected as markdown headings `##` or `###` whose
normalized (lowercased) text matches a synonym below. The section SHALL
be non-empty (at least one non-whitespace character between it and the
next heading of the same or higher level).

| check | requirement | accepted headings (normalized prefix/word match) |
|-------|-------------|--------------------------------------------------|
| c2 build | build instructions present | `build`, `building`, `quickstart` |
| c3 run | run instructions and flags present | `run`, `running`, `usage`, `quickstart` |
| c4 api | endpoint table or method list | `api`, `endpoints`, `endpoint` |
| c5 state | durability / state / architecture description | `internals`, `architecture`, `design`, `durability`, `storage`, `state`, `data model` |
| c6 tests | test command and expected behavior | `tests`, `testing` |
| c7 honesty | known limitations or roadmap | `limitations`, `roadmap` |

When the manifest supplies a `build_cmd` for the component, the Build
section SHALL contain that command string; when it supplies a
`test_cmd`, the Tests section SHALL contain it.

## 4. Version conformance (c8, when `--live`)

For a live daemon (manifest port set) the documented version token
SHALL match the authoritative version of the daemon. The authoritative
version is, in order of preference:

1. the output of the local binary `<dir>/build/<name> <version_flag>`
   (`<version_flag>` defaults to `--version`, overridable as the 6th
   manifest column), when the binary exists and is executable;
2. the version token in the daemon's `GET /` spec (self-description is
   ground truth);
3. otherwise the check is `SKIP` with reason `unreachable`.

A mismatch is a `FAIL` naming both tokens. A component without a live
daemon is checked for token presence only.

## 5. API conformance (c9, when `--live`)

For a live daemon, the endpoint set is extracted from the `GET /` spec
(any line matching `METHOD /path...`, also inside markdown tables) and
from the document's API section only. Endpoints are normalized
(lowercase, query strings stripped after the first `?`) and
deduplicated. The two sets SHALL be identical:

- every endpoint in the live spec SHALL appear in the document's API
  section — otherwise `FAIL` naming each missing endpoint;
- every documented endpoint SHALL be present in the live spec —
  otherwise `FAIL` naming each absent endpoint.

## 6. Scoring and reporting

Each check is `PASS`, `FAIL` or `SKIP`. `SKIP` does not count towards
the score. A component's score is
`pass / (pass + fail)` rounded to the nearest percent. The run summary
is printed as `=== audit: <total pass> pass, <total fail> fail (score <N>%) ===`.
With `--json`, the full machine-readable report is emitted for the
future Quality Management component (iteration 5).

## 7. Stack manifest (`docs/stack.tsv`)

One component per line, tab-separated:

    name<TAB>dir<TAB>port<TAB>build_cmd<TAB>test_cmd<TAB>version_flag

`port` empty means the component is not a live daemon (e.g. exodoc
itself). `build_cmd`/`test_cmd` are optional (section 3); `version_flag`
defaults to `--version` (section 4). Lines starting with `#` are
comments. Malformed lines are skipped with a warning, never fatal.

## 8. Robustness

`exodoc audit` SHALL never crash on malformed input: documents and
manifests are truncated at 16 MiB, control bytes (including NUL) are
stripped, oversized lines are scanned incrementally, and unreachable
daemons are reported as `SKIP` with reason `unreachable`.
