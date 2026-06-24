# NoC Benchmark Generator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在已 merge 的 configurable-topology 上,把 destination 改成 address-driven(修 ring 配對 bug),新增 `gen_test_patterns.py`(spatial NoC traffic pattern + user-custom)與 benchmark runner(per-pattern latency/throughput 彙整)。

**Architecture:** 拔掉 `gen_tb_top` 寫死的 ring 配對(改 `master_i`/`slave_i` ← `scn_node{i}` identity),destination 完全由 transaction `addr=dst_coord<<32` 決定。`gen_coordinate_scenarios` 刪除,功能併入 `gen_test_patterns`(payload × destination-pattern)。correctness 回歸改用確定性 `nearest_neighbor`;benchmark 用 uniform/transpose/hotspot。

**Tech Stack:** Python(generator + runner)、SystemVerilog/Verilator(`--timing`)、CMake/GoogleTest、Git Bash/MSYS2、GNU make。

## Global Constraints
- **python**:`PYTHON3=python3`(mingw64),**不用 `py -3`**。unit tests 用 pytest;若 env 無 pytest,用 `python3 -c` harness 跑同樣斷言(別因缺 pytest 而跳過)。
- **spec**:`docs/internal/superpowers/specs/2026-06-24-noc-benchmark-generator-design.md`(fb5376e);每 task 隱含 spec 全約束。
- **branch**:feature branch off main(2916e6a 之後 HEAD,含 spec/plan commit);**不 push**;commit 格式 `type(scope): description`;不 amend、不 `--no-verify`。
- **address-driven destination 契約**:`master_i`/`slave_i` ← `scn_node{i}`;dst 由 `addr[39:32]=dst_id`(`addr_trans` coord_id=`(y<<X_WIDTH)|x`,X_WIDTH=4)決定。
- **behavior-preserving 定義**:6 個 AXI conformity scenario 在 `nearest_neighbor` 分佈下 **scoreboard clean**(覆蓋不變,非重現同流量)。
- **做減法**:刪 `gen_coordinate_scenarios.py` + `test_gen_coordinate_scenarios.py`,**無 wrapper**;call sites 原子更新。
- **位址唯一性(全域)**:每筆 write/read 對在 dst node `memory_base..+memory_size` 內分到全域唯一 local offset((src,seq) 決定);write/read 同址成對。
- **每 task gate** 綠才下一步:`make check PYTHON3=python3`(ctest 545)+ co-sim(`make sim-regress TOPOLOGY=mesh_4x4_vc1`,16-node)。
- **out of scope(→下一輪)**:injection-rate/saturation curve(動 AxiMaster)。
- **SURGICAL**:不改 c_model / DPI / scoreboard / scenario 格式。

**基準**:開工前 `make check PYTHON3=python3` 全綠 + `make sim-regress TOPOLOGY=mesh_4x4_vc1` 6/6。

---

### Task 1: address-driven 契約修正 + gen_test_patterns(nearest_neighbor)+ 刪 gen_coordinate

**Files:**
- Create: `sim/tools/gen_test_patterns.py`(初版:`--from <base>` × `nearest_neighbor` 確定性 destination)
- Modify: `sim/tools/gen_tb_top.py`(identity 配對:`master_i`/`slave_i` ← `scn_node{i}`;移除 `(i+1)%N` ring shift)
- Delete: `sim/tools/gen_coordinate_scenarios.py`、`sim/tools/test_gen_coordinate_scenarios.py`
- Modify(call sites):`sim/run_regress.py`、`sim/verilator/Makefile`(`run-tb-top`)、`sim/vcs/Makefile`(`run-tb-top`)

**Interfaces:**
- Produces:`gen_test_patterns.py --from <base.yaml> --pattern nearest_neighbor --topology <name> --out <dir>` 寫 `<dir>/node<i>/scenario.yaml`,其中 node i 的 transactions addr = `nbr_coord(i)<<32 + local`(nbr = 固定方向鄰居),`config.memory_base` = `coord(i)<<32 + base`。
- Consumes:topology yaml(x_dim/y_dim);`X_WIDTH=4`;coord(i)=`(y<<4)|x`。

- [ ] **Step 1: 寫 nearest_neighbor dst 公式 + 失敗測試**
建 `sim/tools/gen_test_patterns.py` 骨架 + `sim/tools/test_gen_test_patterns.py`:
```python
# gen_test_patterns.py (excerpt)
X_WIDTH = 4
def coord_id(x, y): return (y << X_WIDTH) | x
def nodes_of(x_dim, y_dim):
    return [(i, i % x_dim, i // x_dim) for i in range(x_dim * y_dim)]
def nearest_neighbor_dst(x, y, x_dim, y_dim):
    """Deterministic: prefer +x(East); fall back W->N->S; no-wrap; never self."""
    for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
        nx, ny = x+dx, y+dy
        if 0 <= nx < x_dim and 0 <= ny < y_dim:
            return nx, ny
    raise ValueError("isolated node")  # only if 1x1
```
```python
# test_gen_test_patterns.py
from gen_test_patterns import nearest_neighbor_dst
def test_nn_interior_prefers_east():
    assert nearest_neighbor_dst(1, 1, 4, 4) == (2, 1)
def test_nn_east_edge_falls_back_west():
    assert nearest_neighbor_dst(3, 1, 4, 4) == (2, 1)   # no East -> West
def test_nn_never_self():
    for i,(x,y) in [(i,(i%4,i//4)) for i in range(16)]:
        assert nearest_neighbor_dst(x,y,4,4) != (x,y)
```
- [ ] **Step 2: 跑測試確認 fail**
Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns.py -v`(或 `python3 -c` import,若無 pytest)
Expected: FAIL(函式/模組未完成)。
- [ ] **Step 3: 實作 gen_test_patterns `--from` × nearest_neighbor + 全域唯一位址 allocator**
讀 base scenario,對每 node i(src):
- dst = `nearest_neighbor_dst(x,y)`;每筆 write/read 對的 `addr = dst_coord<<32 + uniq_offset`,**`uniq_offset` 由全域 allocator 依 (src_node, seq) 給,確保多源收斂到同一 dst node 時不撞同絕對位址**(`nearest_neighbor` 會收斂:node1→node2、node3→node2)。write/read 同 addr 成對。`uniq_offset` 落在 dst node `memory_base..+memory_size` 內。
- `config.memory_base = coord(i)<<32 + base.memory_base`;rewrite `data_file`/`dump_file`/`strb_file` 為相對路徑(沿用舊 gen_coordinate 的檔案改寫邏輯,從 git 史 `gen_coordinate_scenarios.py` 取)。寫 `node<i>/scenario.yaml`。CLI:`--from/--pattern/--topology/--out`。
- **此 allocator(`alloc_unique_offset(dst_node, src_node, seq)`)是核心,T2/T3 的 synthetic/uniform/hotspot/transpose 全部複用**(不可只在 --from 用)。加單元測試:nearest_neighbor 下,所有 write 的絕對位址全域唯一。
- [ ] **Step 4: 改 gen_tb_top identity 配對**
`sim/tools/gen_tb_top.py` emit_tb_top:`m{i}_ctx = cmodel_master_create("master_{i}", scn_node{i})`、`s{i}_ctx = cmodel_slave_create("slave_{i}", scn_node{i})`(去掉 `(i+1)%n` shift)。`cmodel_init` 用 `scn_node{0}`(任一變體 config 相同)。對應的 nmu/nsu/master/slave 都用同 i。
- [ ] **Step 5: 刪 gen_coordinate + 改 call sites**
```bash
git rm sim/tools/gen_coordinate_scenarios.py sim/tools/test_gen_coordinate_scenarios.py
```
`run_regress.py`:把呼叫 `COORD_GEN` 改為 `gen_test_patterns.py --from <base> --pattern nearest_neighbor --topology $TOPOLOGY --out <coord>`;`node_scenarios` 回傳 `node0..node<N-1>`。`sim/verilator/Makefile` + `sim/vcs/Makefile` 的 `run-tb-top` 同樣改呼叫 gen_test_patterns。
- [ ] **Step 6: gate(behavior-preserving)**
```bash
make check PYTHON3=python3                                  # ctest 545/545
make build-verilator TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3
make sim-regress TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3      # 6/6,scoreboard clean(nearest_neighbor 分佈)
python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc1 --check   # exit 0
# all-VC behavior-preserving(契約改動影響所有 topology,spec §6 要求):
for N in 2 4 8; do make build-verilator TOPOLOGY=mesh_4x4_vc$N PYTHON3=python3 && make sim-regress TOPOLOGY=mesh_4x4_vc$N PYTHON3=python3; done
```
Expected: mesh_4x4_vc{1,2,4,8} 全 6/6 + ctest 545(現有 conformity 在 nearest_neighbor 下仍對得上)。若 scoreboard mismatch → systematic-debug address/memory_base 對齊,**不弱化 scoreboard**。
- [ ] **Step 7: Commit**
```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns.py sim/tools/gen_tb_top.py sim/run_regress.py sim/verilator/Makefile sim/vcs/Makefile
git commit -m "feat(sim): address-driven destination contract; gen_test_patterns(nearest_neighbor); drop gen_coordinate_scenarios"
```

---

### Task 2: gen_test_patterns synthetic payload + uniform_random + hotspot

**Files:**
- Modify: `sim/tools/gen_test_patterns.py`(synthetic payload + uniform/hotspot per-packet 隨機 + 全域唯一位址 + guard)
- Modify: `sim/tools/test_gen_test_patterns.py`(uniform/hotspot/uniqueness 測試)

**Interfaces:**
- Produces:`--pattern {uniform_random,hotspot}`、`--transactions-per-node N`、`--seed S`、`--hotspot <linear-node-ids>`(0..N-1 線性 node 索引,非 coord id)、`--allow-self`、synthetic AXI shape(`--size/--len` 預設)。每 node 產 `transactions-per-node` 筆 write+read 對,dst 每筆隨機,addr 全域唯一。

- [ ] **Step 1: 唯一位址 + uniform 取樣 失敗測試**
```python
def test_uniform_excludes_self_and_covers_others():
    import random
    g = pattern_dsts("uniform_random", x_dim=4, y_dim=4, src=(1,1), n=200, seed=1)
    assert (1,1) not in g
    assert len(set(g)) > 5            # 覆蓋多個 dst
def test_global_addr_uniqueness():
    scns = gen_all("uniform_random", "mesh_4x4_vc1", txn_per_node=8, seed=1)
    addrs = [t["addr"] for s in scns for t in s["transactions"] if t["op"]=="write"]
    assert len(addrs) == len(set(addrs))   # 無兩筆 write 撞同絕對位址
```
- [ ] **Step 2: 跑測試確認 fail**
Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py -k 'uniform or uniqueness' -v`
Expected: FAIL。
- [ ] **Step 3: 實作 synthetic + uniform + hotspot + 唯一位址**
- per-packet 取樣:`rng = random.Random(seed); dst = rng.choice(non_self_nodes)`(uniform);hotspot 用加權 choice 於 `--hotspot` node。
- 唯一 local offset:**複用 T1 的 `alloc_unique_offset(dst_node, src, seq)`**;每筆 burst 保留 `(len+1)*(1<<size)` bytes(WRAP/INCR 對齊足夠),且 `base + reserved ≤ memory_size`(超出 → fail-fast,提示加大 memory_size);addr = `dst_coord<<32 | off`;write/read 同 addr 成對。
- synthetic payload:無 `--from` 時用 `--size/--len` 預設產 write+read。
- [ ] **Step 4: guard 測試 + 實作**
```python
def test_guard_mesh_exceeds_dst_capacity():
    with pytest.raises(SystemExit):  # or ValueError
        gen_all("uniform_random", topo_with(x_dim=32, y_dim=32), ...)
```
實作:`x_dim*y_dim > 2**DST_ID_WIDTH` → fail-fast 清楚訊息。
- [ ] **Step 5: gate**
Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py -v`(全綠);`make check PYTHON3=python3`(ctest 545 不受影響)。
- [ ] **Step 6: Commit**
```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns.py
git commit -m "feat(sim): gen_test_patterns synthetic payload + uniform/hotspot (per-packet, global-unique addr, guard)"
```

---

### Task 3: gen_test_patterns transpose(確定性,方形 guard)

**Files:**
- Modify: `sim/tools/gen_test_patterns.py`、`sim/tools/test_gen_test_patterns.py`

**Interfaces:** Produces:`--pattern transpose`,dst=(y,x);需方形 mesh;對角 node→self(隱含 `--allow-self`)。

- [ ] **Step 1: 失敗測試**
```python
def test_transpose_swaps_xy():
    assert transpose_dst(1, 2) == (2, 1)
def test_transpose_diagonal_is_self():
    assert transpose_dst(2, 2) == (2, 2)
def test_transpose_requires_square():
    with pytest.raises(SystemExit):
        gen_all("transpose", topo_with(x_dim=3, y_dim=2), ...)
```
- [ ] **Step 2: 跑測試確認 fail**
Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py -k transpose -v` → FAIL。
- [ ] **Step 3: 實作 transpose**
`transpose_dst(x,y)=(y,x)`;`gen_all` 對 transpose:若 `x_dim!=y_dim` → fail-fast;對角 node 允許 self(不排除)。
- [ ] **Step 4: gate**
Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py -v`(全綠)。
- [ ] **Step 5: Commit**
```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns.py
git commit -m "feat(sim): gen_test_patterns transpose (deterministic, square-mesh guard)"
```

---

### Task 4: benchmark runner + perf summary + hotspot smoke

**Files:**
- Create: `sim/tools/run_benchmark.py`(跑一個 pattern、scoreboard gate、讀 perf.json 彙整)
- Modify: root `Makefile`(加 `bench` target);`sim/verilator/perf_cli_summary.py`(若需 p95)

**Interfaces:**
- Produces:`python3 sim/tools/run_benchmark.py --topology <t> --pattern <p> [--seed S] [--transactions-per-node N]` → build tb(若需)、gen_test_patterns 產 pattern、跑 Vtb_top(全 N plusargs)、要 `PASS: scoreboard clean`、讀 `perf.json` → 寫 `output/<scenario>/bench_summary.json`(latency mean/p95/max + throughput byte/txn + link/occupancy)。

- [ ] **Step 1: perf summary 失敗測試(用既有 perf.json)**
```python
def test_summary_computes_p95(tmp_path):
    perf = {"latency":{"transactions":[{"latency":l} for l in range(1,101)]}}
    s = summarize(perf)
    assert s["latency"]["p95"] == 95   # nearest-rank on 1..100
```
- [ ] **Step 2: 跑測試確認 fail**
Run: `python3 -m pytest sim/tools/test_run_benchmark.py -v` → FAIL。
- [ ] **Step 3: 實作 runner + summarize**
`summarize(perf_json)`:從 `latency.transactions` 算 mean/p95(nearest-rank)/max + per-slot throughput;標 `"injection":"greedy-finite-trace-stress"` + window。runner:呼 gen_test_patterns、launch exe、**check `PASS: scoreboard complete, scoreboard clean` marker**(tb_top 的 PASS guard 已內含 master_count==N + reads_checked≥N + scoreboard clean,只在全數通過才印此 marker — runner 靠 marker,不另解析未列印的 stats)、dump `bench_summary.json`。
- [ ] **Step 4: hotspot smoke(非-ring,必要)**
```bash
make build-verilator TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3
python3 sim/tools/run_benchmark.py --topology mesh_4x4_vc1 --pattern hotspot --hotspot 0 --transactions-per-node 4 --seed 1
```
Expected: `PASS: scoreboard clean`(non-ring 流量真的 route + 驗證)+ `bench_summary.json` 產出(latency/throughput 非空)。若 mismatch → systematic-debug,不弱化 scoreboard。
- [ ] **Step 5: gate**
Run: `python3 -m pytest sim/tools/test_run_benchmark.py -v`;`make check PYTHON3=python3`(545);Step 4 hotspot smoke PASS。
- [ ] **Step 6: Commit**
```bash
git add sim/tools/run_benchmark.py sim/tools/test_run_benchmark.py Makefile sim/verilator/perf_cli_summary.py
git commit -m "feat(sim): NoC benchmark runner + per-pattern perf summary (greedy stress); hotspot smoke"
```

---

### Task 5: 檔案對齊收尾(gen_filelist → sim/tools/)

**Files:**
- Move: `sim/gen_filelist.py` → `sim/tools/gen_filelist.py`
- Modify: 引用它的 Makefile(`sim/verilator/Makefile`、`sim/vcs/Makefile`、`sim/build_config.mk` 視引用而定)

**Interfaces:** Produces:所有 `gen_*` 工具集中 `sim/tools/`。

- [ ] **Step 1: 移檔 + 改引用**
```bash
git mv sim/gen_filelist.py sim/tools/gen_filelist.py
# 實際 callers:sim/verilator/Makefile:81、sim/vcs/Makefile:97,皆用 $(COSIM_ROOT)/gen_filelist.py
# (COSIM_ROOT=sim/)→ 改成 $(COSIM_ROOT)/tools/gen_filelist.py(精準,勿用會把路徑接歪的鬆散 sed):
sed -i 's@\$(COSIM_ROOT)/gen_filelist.py@$(COSIM_ROOT)/tools/gen_filelist.py@g' sim/verilator/Makefile sim/vcs/Makefile
grep -rn 'gen_filelist.py' sim/ --include=Makefile --include=*.mk   # 確認全部指向 tools/，無殘留舊路徑
- [ ] **Step 2: gate**
```bash
make clean-verilator PYTHON3=python3
make build-verilator TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3 && make sim-regress TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3   # 6/6
make check PYTHON3=python3   # 545
```
Expected: filelist 仍正確生成、build/co-sim 綠。
- [ ] **Step 3: Commit**
```bash
git add -A sim/tools/gen_filelist.py sim/verilator/Makefile sim/vcs/Makefile sim/build_config.mk
git commit -m "refactor(sim): move gen_filelist.py into sim/tools/ (gen_* consolidation)"
```

---

## Self-Review(plan vs spec)

- **spec §1 契約**:T1(identity 配對 + address-driven + 刪 gen_coordinate + call sites + behavior-preserving gate)。✅
- **spec §2 gen_test_patterns**:nearest_neighbor(T1)、synthetic+uniform+hotspot+唯一位址+guard(T2)、transpose(T3)、custom `--from`(T1 引入、T2/T3 沿用)。✅
- **spec §3 runner + perf**:T4(run_benchmark + summarize p95 + greedy-stress label + bench_summary.json)。✅
- **spec §4 檔案對齊**:刪 gen_coordinate+test(T1)、gen_filelist→tools/(T5)。✅
- **spec §5 測試**:pattern 公式 + guard 單元測試(T1-T3)、hotspot 非-ring smoke(T4)、回歸(每 task gate)。✅
- **spec §6 out of scope**:injection-rate 未列入任何 task。✅
- **type 一致**:`coord_id=(y<<4)|x`、`nearest_neighbor_dst`、`transpose_dst`、`pattern_dsts`、`summarize`、`bench_summary.json` 全 plan 一致。
- **已知執行時定案(非 placeholder)**:T1 的 gen_tb_top identity 配對逐行 SV、檔案改寫邏輯(從 git 史 gen_coordinate 取)以「behavior-preserving + 6/6 gate」為準;runner 與 exe 的 plusarg 細節對齊既有 run_regress。
