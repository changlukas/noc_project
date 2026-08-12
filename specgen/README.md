# specgen - spec-as-code generator

Single-sourced NI spec: JSON/YAML definitions in `source/` and
`generated/json/` elaborate into the C++ headers and SystemVerilog packages
the model and testbench build against. Downstream code is never hand-edited;
regenerate it from the source instead.

## Where code lives

- `source/` - `constants.yaml` (parameters), `interface_handshake.json`,
  `noc_function_blocks.json` (feature inventory / cross-domain check, not
  codegen input)
- `generated/json/` - hand-curated JSON SSoT (`ni_packet.json`, `ni_signals.json`)
  plus their schemas; codegen input, not codegen output
- `generated/cpp/` - elaborated C++ headers for `ref_model/c_model/`
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
- `codegen_check` (CMake custom target, `ref_model/c_model/CMakeLists.txt`) - runs
  the same check before the c_model build
- `make pytest` - specgen + sim/tools test suites, includes the golden drift check

## Boundary

Generated files under `generated/cpp/` and `generated/sv/` are committed and
never hand-edited; a source change regenerates them. The JSON under
`generated/json/` is hand-curated input and is edited directly.

## Documentation

- [User guide](docs/guide/index.md)
