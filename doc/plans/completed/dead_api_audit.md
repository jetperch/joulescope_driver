# Dead public API audit: declared but unimplemented surfaces

**Status**: archived 2026-10-06.  All three items are still open; they
moved to `../open_issues.md` section 1, "Public API".
**Created**: 2026-07-31

The 2026-07 design review (`design_review_2026-07.md`) found public API
surfaces that are declared and documented but have zero implementation.
Per review triage, these are documented here and excluded from the
remediation work.  Each item needs an explicit remove-or-implement
decision before the next major API revision.

## 1. Pubsub metadata-request / query flags and topic suffixes

Declared:

- `include/jsdrv.h:570-587` — `JSDRV_SFLAG_METADATA_REQ (1 << 2)`,
  `JSDRV_SFLAG_QUERY_REQ (1 << 4)`, `JSDRV_SFLAG_QUERY_RSP (1 << 5)`.
- `include/jsdrv.h:92-97` — documents `'%'` (metadata request) and `'&'`
  (query request) topic suffixes; `'?'` for query response.
- `src/topic.c:86-97` (`is_suffix_char`) and `src/pubsub.c:368-372`
  recognize the characters — plumbing started, never finished.

Not implemented:

- `publish()` (`src/pubsub.c:505-521`) dispatches only RETURN_CODE,
  METADATA_RSP, and normal PUB.
- `process_msg()` (`src/pubsub.c:634-638`) routes only `'$'` and `'#'`.
- A publish to `a/b/c%`, `a/b/c&`, or `a/b/c?` creates a junk topic node
  named `c%` / `c&` / `c?`.
- Repo-wide, the three flags appear only at their definitions: no use in
  `src/`, `test/`, `example/`, or bindings.

Recommendation: **remove** (deprecate the enum values, delete the suffix
documentation) unless a concrete consumer appears.  The retained-value +
`$` metadata mechanism already covers the known use cases.  If removed,
also simplify `is_suffix_char`.

## 2. Stream buffer `g/mode` (fill-and-hold / single capture)

Declared:

- `include_private/jsdrv_prv/buffer.h:52` —
  `JSDRV_BUFFER_MSG_MODE "g/mode"` (0: continuous, 1: fill & hold).

Not implemented:

- `src/buffer.c:481-484` — falls into the catch-all
  (`// todo mode circular or single capture`) and returns
  `JSDRV_ERROR_PARAMETER_INVALID`.
- `JSDRV_BUFFER_MSG_MODE` appears nowhere else in the repo.

Recommendation: **decide with the UI roadmap**.  Fill-and-hold (single
capture) is a genuinely useful oscilloscope-style feature; if wanted,
implement together with the `g/hold` 1→0 clear fix (Phase 1.4).
Otherwise delete the macro and its comment.

Note: the private macros `JSDRV_BUFFER_MSG_SIGNAL_TOPIC` / `_INFO` /
`_SAMPLE_REQ` (`buffer.h:53-55`) carry a `"ZZZ"` placeholder and are
unused; `src/buffer.c:446-451` compares literals instead.  Clean up when
touching this header.

## 3. `jsdrv_initialize()` arguments (`jsdrv_arg_s`)

Declared:

- `include/jsdrv.h:599-620` — `struct jsdrv_arg_s` and the documented
  `args` parameter of `jsdrv_initialize()`.

Not implemented:

- `src/jsdrv.c:987` stores the pointer; `src/jsdrv.c:352` clears it.
  Nothing ever reads it.  No argument name is recognized anywhere.

Recommendation: **keep the parameter (ABI stability), document that no
arguments are currently defined**, and reserve it for future use (e.g.
log level, backend selection, emulated-device injection per
`emulated_device.md`).  Remove the misleading detail in the doxygen that
implies arguments exist today.
