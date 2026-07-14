# specgen - spec-as-code generator

Single-sourced NI spec: JSON/YAML definitions in `source/` and
`generated/json/` elaborate into the C++ headers and SystemVerilog packages
the model and testbench build against. Downstream code is never hand-edited;
regenerate it from the source instead.

## Where code lives

- `source/` - `constants.yaml` (parameters), `interface_handshake.json`,
  `noc_function_blocks.json` (feature inventory / cross-domain check, not
  codegen input)
- `generated/json/` - `ni_signals.json` (hand-curated) plus packet/param JSON
  elaborated from `source/`
- `generated/cpp/` - elaborated C++ headers for `src/c_model/`
- `generated/sv/` - elaborated SystemVerilog packages for the testbench
- `tools/codegen.py` - the elaborator; `tools/elaborate/` - per-domain emitters

## Regenerate

```bash
py -3 tools/codegen.py --target cpp --domain packet     # domains: packet, signals, params, noc_types
py -3 tools/codegen.py --target sv --domain signals
py -3 tools/codegen.py --target sv --domain noc_types --num-vc 4   # noc_types needs --num-vc (1, 2, 4, or 8)
```

## Drift gates

- `py -3 tools/codegen.py --check` - re-elaborates every domain and diffs
  against the committed output
- `codegen_check` (CMake custom target, `src/c_model/CMakeLists.txt`) - runs
  the same check before the c_model build
- `make specgen_pytest` - specgen test suite, includes the golden drift check

## Boundary

Generated files under `generated/cpp/` and `generated/sv/` are committed and
never hand-edited; a source change regenerates them. `generated/json/
ni_signals.json` is the one hand-curated input in `generated/` and is edited
directly.

## Documentation

- [User guide](docs/guide/index.md)
