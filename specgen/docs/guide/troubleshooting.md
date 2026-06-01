# Troubleshooting

## `--check` exits 1

A committed artifact under `include/` or `rtl_pkg/` differs from a fresh elaboration. Read the unified diff printed to stdout.

Fix: re-elaborate and stage:

```bash
py -3 tools/codegen.py --target cpp --domain packet
git add include/ni_flit_constants.h
```

If the drift is unexpected, the JSON SSoT was probably updated without a matching codegen run.

## `ERROR: --domain is required with --target`

`--target` defaults to `cpp`, but `--domain` has no default. Pass one explicitly:

```bash
py -3 tools/codegen.py --target cpp --domain packet
```

## `ERROR: source JSON not found: ...`

The source JSON for the requested domain is missing. For `packet`, `signals`, `registers`, look under `generated/`. Restore the file from git if it should exist.

## `ModuleNotFoundError: No module named 'ni_spec'`

You ran `tools/codegen.py` from the wrong directory. The script adds `spec_validate/` to `sys.path` based on its own location, so it expects to be invoked from inside `spec_validate/`:

```bash
cd spec_validate
py -3 tools/codegen.py --target cpp --domain packet
```

## `[skip] verilator not in PATH -- SV lint smoke test skipped`

`--lint-sv` cannot find `verilator`. Either install it, or accept the skip — exit code is still `0`.

## `[skip] no .sv files found in rtl_pkg/`

Elaborate at least one SV package first:

```bash
py -3 tools/codegen.py --target sv --domain packet
py -3 tools/codegen.py --lint-sv
```

## ASCII encoding error during elaboration

Elaborated files are written with `encoding="ascii"`, `errors="strict"`. If a spec source contains a non-ASCII identifier — for example a Chinese-character register name — the elaborator raises. Keep all machine-relevant identifiers ASCII; non-ASCII text is fine in description strings as long as those strings are not elaborated to `.h` / `.sv`.

## A `static_assert` fires in the generated header

The arithmetic invariants in `ni_flit_constants.h` (`FLIT_WIDTH == HEADER_WIDTH + PAYLOAD_WIDTH`, and the SECDED bound) caught an internal contradiction in the JSON SSoT. Fix the width arithmetic in `generated/ni_packet.json` and re-elaborate — do not edit the header.

## Tests fail with `ImportError` for `tools.elaborate`

`pytest` must run from `spec_validate/`:

```bash
cd spec_validate
py -3 -m pytest tests/ -q
```
