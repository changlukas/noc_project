# Architecture

Two artifact tiers, generated top-down:

```
generated/*.json                <- validated JSON SSoT
        |  (tools/elaborate/*.py, dispatched by tools/codegen.py)
        v
generated/cpp/*.h               <- C++ headers for the C-model
generated/sv/*.sv                    <- SystemVerilog packages for co-sim
```

## Domains

Three spec domains, each with its own JSON SSoT and a pair of elaborators:

| Domain | JSON SSoT | C++ header | SV package |
|--------|-----------|------------|------------|
| `packet` | `generated/json/ni_packet.json` | `ni_flit_constants.h` | `ni_flit_pkg.sv` |
| `signals` | `generated/json/ni_signals.json` | `ni_signals.h` | `ni_signals_pkg.sv` |
| `params` | `source/constants.yaml` | `ni_params.h` | `ni_params_pkg.sv` |

`noc_function_blocks.json` is retained as a feature inventory and cross-domain consistency check, but no longer drives codegen.

## Layers

| Layer | Module | Responsibility |
|-------|--------|----------------|
| Loader | `ni_spec.loader` | Read JSON SSoT files. Pure I/O. |
| Constants API | `ni_spec.constants` | Pure-function accessors over a loaded spec. The firewall between schema and consumers. |
| Codegen dispatcher | `tools/codegen.py` | Route `(target, domain)` to the right elaborator. |
| Elaborators | `tools/elaborate/{cpp,sv}_*.py` | Format the output. Each consumes only `ni_spec.constants` — never raw JSON. |

## Why a firewall?

If the JSON shape changes — say a field is renamed — only `ni_spec.constants` needs an update. All eight elaborators and any downstream Python consumer stay untouched.

## Source of truth

The JSON SSoT files are the contract. `--check` enforces that committed `generated/cpp/` and `generated/sv/` exactly match what those JSONs would elaborate to (timestamp aside). Edit the SSoT, run codegen, commit both.

Hand-editing files in `generated/cpp/` or `generated/sv/` will be caught by `--check`. Don't.

## Provenance

Every elaborated file carries a five-line banner naming the source JSON, its SHA, the tool version, and the timestamp. See [Artifacts § Provenance banner](artifacts.md#provenance-banner).
