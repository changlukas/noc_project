# Commands

Single entry point: `specgen/tools/codegen.py`. Paths resolve from the script's own location, so it runs from any working directory (examples assume the repo root).

## Synopsis

```bash
py -3 specgen/tools/codegen.py --target {cpp|sv} --domain {packet|signals|params} [--out PATH]
py -3 specgen/tools/codegen.py --check
py -3 specgen/tools/codegen.py --lint-sv
```

## Elaborate one artifact

```bash
py -3 specgen/tools/codegen.py --target cpp --domain packet
```

| Flag | Default | Description |
|------|---------|-------------|
| `--target` | `cpp` | `cpp` writes a `.h`; `sv` writes a `.sv`. |
| `--domain` | (required) | One of `packet`, `signals`, `params`. |
| `--out` | `generated/cpp/` for cpp, `generated/sv/` for sv | Output directory. Created if missing. |

The six `(target, domain)` combinations and their outputs:

| Target | Domain | Output filename | Source JSON |
|--------|--------|-----------------|-------------|
| `cpp` | `packet` | `ni_flit_constants.h` | `generated/json/ni_packet.json` |
| `cpp` | `signals` | `ni_signals.h` | `generated/json/ni_signals.json` |
| `cpp` | `params` | `ni_params.h` | `source/constants.yaml` |
| `sv` | `packet` | `ni_flit_pkg.sv` | `generated/json/ni_packet.json` |
| `sv` | `signals` | `ni_signals_pkg.sv` | `generated/json/ni_signals.json` |
| `sv` | `params` | `ni_params_pkg.sv` | `source/constants.yaml` |

Exit codes: `0` success, `1` source JSON missing or elaborator error, `2` argument error.

## Drift check

```bash
py -3 specgen/tools/codegen.py --check
```

Re-elaborates all six artifacts into a scratch dir, diffs each against the committed `generated/cpp/` and `generated/sv/`, and prints a unified diff (up to 40 lines per file) for anything that drifted.

The `Generated at:` line is excluded from comparison, so timestamps alone never trigger drift.

Exit code `0` = all artifacts match committed; `1` = at least one drifted.

Typical CI use:

```bash
py -3 specgen/tools/codegen.py --check || exit 1
```

## SV lint smoke test

```bash
py -3 specgen/tools/codegen.py --lint-sv
```

Runs `verilator --lint-only --Wall` on every `generated/sv/*.sv`. Exits `0` and skips gracefully when:

- `verilator` is not on PATH, or
- `generated/sv/` contains no `.sv` files yet (run `--target sv --domain ...` first).

Otherwise the exit code matches `verilator`'s.

## Help

```bash
py -3 specgen/tools/codegen.py --help
```
