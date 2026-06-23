# Extract Standalone Scaffolding — Plan (Task A)

- Date: 2026-06-23
- Goal: 把 hermetic standalone harness(`NullNoc*` + `*Standalone`)從 `nmu.hpp`/`nsu.hpp` 抽到獨立 header,讓 core 檔只剩 `Nmu`/`Nsu` 本體。
- Driver: 可讀性 — production core 檔不再混 standalone/co-sim 鷹架。
- Behavior-preserving:純搬移 + 修 include,既有 ctest 全綠即驗。Codex 已判定 split 乾淨(僅用 public API、無 private 耦合、include 為 DAG)。

## Decisions
- 新檔位置:`c_model/include/nsu/nsu_standalone.hpp`、`c_model/include/nmu/nmu_standalone.hpp`(component-level harness,wrap + test 都用,故留在 nmu/nsu 不進 wrap/)。
- core 檔(`nsu.hpp`/`nmu.hpp`)**不可反向 include** standalone → 維持 DAG。
- 不改 namespace、不改任何邏輯。

## Step 1 — NSU
- **MOVE** `c_model/include/nsu/nsu.hpp` 行 192–340(`// Stage 5b: NsuStandalone` 註解 + `namespace detail { NullNocReqIn, NullNocRspOut }` + `class NsuStandalone`)→ 新 `nsu/nsu_standalone.hpp`。
- 新檔:`#pragma once` + `#include "nsu/nsu.hpp"` + standalone 內容需要的 base/std header(`router/req_in.hpp`、`router/rsp_out.hpp`、`<deque>` 等)+ `namespace ni::cmodel::nsu { ... }`。
- `nsu.hpp` 保留 1–191(含第一個 detail 區塊 135–147 `make_vc_arbiter`,屬 core)+ 收尾 `}  // namespace ni::cmodel::nsu`。
- 更新 consumers:用 `NsuStandalone`/`NullNoc*` 的檔改 include `nsu/nsu_standalone.hpp`(已知 `wrap/nsu_wrap.hpp:37`;build 會抓出其餘)。
- Gate:`make check PYTHON3=python3`(stale verilator .d 則先 `make clean-verilator`)→ commit `refactor: extract NsuStandalone scaffolding to nsu_standalone.hpp`。

## Step 2 — NMU
- **MOVE** `c_model/include/nmu/nmu.hpp` 行 420–574(`// Stage 5b: NmuStandalone` + `namespace detail { NullNocReqOut, NullNocRspIn }` + `class NmuStandalone`)→ 新 `nmu/nmu_standalone.hpp`。
- 新檔:`#pragma once` + `#include "nmu/nmu.hpp"` + 需要的 base/std header(`router/req_out.hpp`、`router/rsp_in.hpp`、`<deque>` 等)+ `namespace ni::cmodel::nmu { ... }`。
- `nmu.hpp` 保留 1–419(含第一個 detail 區塊 254–266,屬 core)+ 收尾 `}  // namespace ni::cmodel::nmu`。
- 更新 consumers:`wrap/nmu_wrap.hpp` + tests(`test_nmu*`、`test_rob_occupancy`、`test_port_pair_loopback`;build 抓漏)。
- Gate:`make check PYTHON3=python3` → commit `refactor: extract NmuStandalone scaffolding to nmu_standalone.hpp`。

## Success Criteria
- ctest 全綠(數量不變);build-verilator 綠。
- `nsu.hpp`/`nmu.hpp` 不含 `NullNoc*`/`*Standalone`;新 standalone header 各含之。
- 無 include 循環(core 不 include standalone)。
- 行為零改變(純搬移)。
