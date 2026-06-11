# OSS Attribution — c_model/include/axi/

Algorithms in this directory are ported from
[alexforencich/cocotbext-axi](https://github.com/alexforencich/cocotbext-axi),
MIT license, with the adaptations listed under "Adaptation notes" below.
cocotbext-axi is Python (cocotb async); this c_model is C++17 + GoogleTest
with synchronous tick-driven semantics.

## File mapping

| c_model file (this repo)          | Upstream Python source             |
|-----------------------------------|------------------------------------|
| `axi/types.hpp`                   | `cocotbext/axi/*.py` (enums + structs) |
| `axi/memory_port.hpp`             | `cocotbext/axi/memory.py` (tick-driven re-shape of MemoryInterface) |
| `axi/memory.hpp`                  | `cocotbext/axi/axi_ram.py` (AxiRam) |
| `axi/axi_slave.hpp`               | `cocotbext/axi/axi_slave.py` (AxiSlave + AxiSlaveWrite + AxiSlaveRead) |
| `axi/axi_master.hpp`              | `cocotbext/axi/axi_master.py` (AxiMaster + AxiMasterWrite + AxiMasterRead) |
| `axi/scoreboard.hpp`              | (independent design; pattern from cocotbext-axi tests) |
| `axi/scenario_parser.hpp`         | (independent; cocotbext-axi has no scenario file format) |
| `axi/protocol_rules.hpp`          | (independent design; AXI4 IHI 0022 rule extraction inspired by cocotbext-axi runtime checks) |

## Adaptation notes

- cocotb async → C++ sync tick(): `async def _run` loops become `tick()` step functions
- Python `Queue` → `std::deque<T>`
- Python `Event` → boolean flags
- Python exceptions → `std::runtime_error` for user input; `assert(...)` for invariants
- Per-ID dicts in cocotbext-axi → `std::map<uint8_t, T>` in our c_model

## License

cocotbext-axi is MIT licensed. See <https://github.com/alexforencich/cocotbext-axi/blob/master/LICENSE>.

This c_model project inherits the MIT terms for the ported algorithms.
No project-wide license has been selected yet (see README.md, License
section); until one is added, the rest of c_model is not offered under
an open-source license.

## Phase C — Exclusive Access (AxLOCK + EXOKAY)

Independent design per AXI4 IHI 0022 §A7. cocotbext-axi (MIT) does NOT implement
exclusive monitor (only carries lock signal). Closest OSS reference is
ZipCPU/wb2axip AXIDOUBLE (Apache 2.0, Verilog, master-side buffer), used only as
semantic reference for register/state shape — no code ported.

Files: `axi_slave.hpp` (exclusive_tags_, E1-E6), `protocol_rules.hpp`
(6 stateless + 1 monitor helper, `compute_tag_range`).
