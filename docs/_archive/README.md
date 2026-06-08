# Archive

Historical specs, plans, working notes, and presentation drafts. Contents
here are **not authoritative**; they may conflict with current implementation
and current architecture documentation.

## Status

These documents are frozen at the date in their filename or in their
internal `Date:` header. They are kept for design-decision archaeology
(why a choice was made, what alternatives were considered) and for
reproducibility of completed brainstorm + plan cycles.

## When to read this

- Investigating why a specific design decision was made
- Tracing the history of a refactor or rename
- Reading completed plans whose implementation is now in `git log`

## When not to read this

- Looking for current architecture: read `../architecture.md`
- Looking for how to build, test, or contribute: read `../development.md`
- Looking for the project entry point: read `../../README.md`
## Layout

```
_archive/
  README.md                       (this file)
  noc_cmodel_rtl_plan.md          (stage roadmap; superseded by current
                                   docs/architecture.md)
  superpowers/
    specs/                        (14 completed brainstorm specs)
    plans/                        (13 completed implementation plans)
  spec_ni/                        (6 spec working notes: presentation
                                   drafts, session handoffs, review logs)
  specgen/
    2026-05-26-spec-as-code-unified-design.md  (specgen sub-project plan)
```

## Doc classes (cross-reference)

The project uses a 4-class decision model when classifying documentation:

- **Normative spec**: authoritative; changes require sign-off
  (e.g. `spec/ni/doc/*.md`).
- **Maintained guide**: tracks code drift
  (e.g. `README.md`, `docs/architecture.md`, `docs/development.md`).
- **Generated reference**: tool emits; humans do not edit
  (e.g. `c_model/FEATURE_INVENTORY.md`).
- **Historical archive**: contents here.

The classes are guidance, not per-file labels. Status is established by
directory location and per-file banners.
