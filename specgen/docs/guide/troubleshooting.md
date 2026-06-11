# Troubleshooting

## `--check` exits 1

A committed artifact under `generated/cpp/` or `generated/sv/` differs from a fresh elaboration. Read the unified diff printed to stdout.

Fix: re-elaborate and stage:

```bash
py -3 specgen/tools/codegen.py --target cpp --domain packet
git add generated/cpp/ni_flit_constants.h
```

If the drift is unexpected, the JSON SSoT was probably updated without a matching codegen run.

## `ERROR: --domain is required with --target`

`--target` defaults to `cpp`, but `--domain` has no default. Pass one explicitly:

```bash
py -3 specgen/tools/codegen.py --target cpp --domain packet
```

## `ERROR: source JSON not found: ...`

The source JSON for the requested domain is missing. For `packet`, `signals`, `registers`, look under `generated/json/`. Restore the file from git if it should exist.

## `ModuleNotFoundError: No module named 'ni_spec'`

`codegen.py` inserts the specgen root into `sys.path` from its own file location, so it runs from any working directory. If this error appears, the checkout is incomplete (`specgen/ni_spec/` missing) -- restore it from git, then:

```bash
py -3 specgen/tools/codegen.py --target cpp --domain packet
```

## `[skip] verilator not in PATH -- SV lint smoke test skipped`

`--lint-sv` cannot find `verilator`. Either install it, or accept the skip — exit code is still `0`.

## `[skip] no .sv files found in generated/sv/`

Elaborate at least one SV package first:

```bash
py -3 specgen/tools/codegen.py --target sv --domain packet
py -3 specgen/tools/codegen.py --lint-sv
```

## ASCII encoding error during elaboration

Elaborated files are written with `encoding="ascii"`, `errors="strict"`. If a spec source contains a non-ASCII identifier — for example a Chinese-character register name — the elaborator raises. Keep all machine-relevant identifiers ASCII; non-ASCII text is fine in description strings as long as those strings are not elaborated to `.h` / `.sv`.

## A `static_assert` fires in the generated header

The arithmetic invariants in `ni_flit_constants.h` (`FLIT_WIDTH == HEADER_WIDTH + PAYLOAD_WIDTH`, and the SECDED bound) caught an internal contradiction in the JSON SSoT. Fix the width arithmetic in `generated/json/ni_packet.json` and re-elaborate — do not edit the header.

## Tests fail with `ImportError` for `tools.elaborate`

`pytest` must run from `specgen/`:

```bash
cd specgen
py -3 -m pytest tests/ -q
```
