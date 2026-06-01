# Commands

Single entry point: `tools/codegen.py`. Invoke from `spec_validate/`.

## Synopsis

```bash
py -3 tools/codegen.py --target {cpp|sv} --domain {packet|signals|registers} [--out PATH]
py -3 tools/codegen.py --check
py -3 tools/codegen.py --lint-sv
```

## Elaborate one artifact

```bash
py -3 tools/codegen.py --target cpp --domain packet
```

| Flag | Default | Description |
|------|---------|-------------|
| `--target` | `cpp` | `cpp` writes a `.h`; `sv` writes a `.sv`. |
| `--domain` | (required) | One of `packet`, `signals`, `registers`. |
| `--out` | `include/` for cpp, `rtl_pkg/` for sv | Output directory. Created if missing. |

The six `(target, domain)` combinations and their outputs:

| Target | Domain | Output filename | Source JSON |
|--------|--------|-----------------|-------------|
| `cpp` | `packet` | `ni_flit_constants.h` | `generated/ni_packet.json` |
| `cpp` | `signals` | `ni_signals.h` | `generated/ni_signals.json` |
| `cpp` | `registers` | `ni_regs.h` | `generated/ni_registers.json` |
| `sv` | `packet` | `ni_flit_pkg.sv` | `generated/ni_packet.json` |
| `sv` | `signals` | `ni_signals_pkg.sv` | `generated/ni_signals.json` |
| `sv` | `registers` | `ni_regs_pkg.sv` | `generated/ni_registers.json` |

Exit codes: `0` success, `1` source JSON missing or elaborator error, `2` argument error.

## Drift check

```bash
py -3 tools/codegen.py --check
```

Re-elaborates all six artifacts into a scratch dir, diffs each against the committed `include/` and `rtl_pkg/`, and prints a unified diff (up to 40 lines per file) for anything that drifted.

The `Generated at:` line is excluded from comparison, so timestamps alone never trigger drift.

Exit code `0` = all artifacts match committed; `1` = at least one drifted.

Typical CI use:

```bash
py -3 tools/codegen.py --check || exit 1
```

## SV lint smoke test

```bash
py -3 tools/codegen.py --lint-sv
```

Runs `verilator --lint-only --Wall` on every `rtl_pkg/*.sv`. Exits `0` and skips gracefully when:

- `verilator` is not on PATH, or
- `rtl_pkg/` contains no `.sv` files yet (run `--target sv --domain ...` first).

Otherwise the exit code matches `verilator`'s.

## Help

```bash
py -3 tools/codegen.py --help
```
