# Benchmark Stage 2 — file_master Emitter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a pulp `axi_file_master` stimulus emitter to `sim/tools/gen_test_patterns.py`: given a traffic pattern, produce per-node `write.txt` + `read.txt` in the file_master `$fscanf` format, with src-partitioned addresses and `data=f(addr)`, for all 4 patterns (neighbor / transpose / uniform_random / hotspot).

**Architecture:** Add-only. A new `--format file_master` mode reuses the existing pattern math (`neighbor_dst`/`transpose_dst`/`uniform_random_dsts`/`hotspot_dsts` + `alloc_unique_offset`) but swaps the output backend from `scenario.yaml` to the file_master text format. The existing YAML path is untouched (its consumers — `run_benchmark.py`, `run_regress.py`, ctest — keep working until Stage 5 rewrites/removes them). AXI widths follow the DUT's single source of truth, `specgen/source/constants.yaml`.

**Tech Stack:** Python 3, PyYAML (already a dependency), pytest.

## Global Constraints

- **Maximize pulp DV IP reuse:** the emitter produces the exact input pulp `axi_file_master` requires; it is the only custom piece. Do not reimplement any pulp behavior.
- **Add-only this stage:** do NOT delete the YAML path, `_emit_base_driven_node`, `--from`, `--preserve-addr`, `--id-policy`, `run_benchmark.py`, or any test. Those are removed in Stage 5 together with their consumers. Existing `test_gen_test_patterns.py` must stay green.
- **AXI widths from the DUT SSoT:** read `ID_WIDTH=8 / ADDR_WIDTH=64 / DATA_WIDTH=256` from `specgen/source/constants.yaml` (`['axi'][name]['default']`), never hardcode. The SV endpoint parameterizes the same widths from `ni_params_pkg` (generated from this file), so they stay unified.
- **Directed = INCR-only, `atop=0`, full strobe, full readback** (scoreboard's supported subset; read set == write set by construction).
- **`data=f(addr)` = address-in-data:** byte at address `A` = `A & 0xFF`.
- Keep the `sim/test_patterns/AX4-*` base YAMLs (ctest consumes them; decoupling is a separate backlog round). Emitter output uses distinct pattern-named scenario dirs, coexisting.

## File Structure

- Modify: `sim/tools/gen_test_patterns.py` — add width reader, beat encoder, `emit_file_master_node`, and `--format` wiring.
- Create: `sim/tools/test_gen_test_patterns_filemaster.py` — tests for the new emitter (kept separate so the existing test file is untouched).
- Modify: `.gitignore` — ignore generated `sim/test_patterns/*/node*/write.txt` and `read.txt`.

---

### Task 1: AXI width reader (follows the DUT SSoT)

**Files:**
- Modify: `sim/tools/gen_test_patterns.py`
- Create: `sim/tools/test_gen_test_patterns_filemaster.py`

**Interfaces:**
- Produces: `axi_widths() -> dict` returning `{"id": 8, "addr": 64, "data": 256}` read from `specgen/source/constants.yaml`.

- [ ] **Step 1: Write the failing test**

Create `sim/tools/test_gen_test_patterns_filemaster.py`:

```python
import gen_test_patterns as g


def test_axi_widths_follow_constants_ssot():
    w = g.axi_widths()
    assert w == {"id": 8, "addr": 64, "data": 256}
```

- [ ] **Step 2: Run it, verify it fails**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py::test_axi_widths_follow_constants_ssot -v`
Expected: FAIL with `AttributeError: module 'gen_test_patterns' has no attribute 'axi_widths'`.

- [ ] **Step 3: Implement the reader**

Add near the top of `gen_test_patterns.py` (after the existing imports / `ADDR_DST_SHIFT` constants):

```python
from pathlib import Path

_CONSTANTS_YAML = Path(__file__).resolve().parents[2] / "specgen" / "source" / "constants.yaml"


def axi_widths():
    """AXI ID/ADDR/DATA widths from the DUT single source of truth
    (specgen/source/constants.yaml — the same file ni_params_pkg is generated from).
    Read directly (values only); the specgen validator owns schema checking."""
    axi = yaml.safe_load(_CONSTANTS_YAML.read_text(encoding="utf-8"))["axi"]
    return {
        "id":   int(axi["ID_WIDTH"]["default"]),
        "addr": int(axi["ADDR_WIDTH"]["default"]),
        "data": int(axi["DATA_WIDTH"]["default"]),
    }
```

- [ ] **Step 4: Run it, verify it passes**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py::test_axi_widths_follow_constants_ssot -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns_filemaster.py
git commit -m "feat(emitter): AXI width reader from constants.yaml SSoT"
```

---

### Task 2: Write-beat encoder (address-in-data, full strobe)

**Files:**
- Modify: `sim/tools/gen_test_patterns.py`
- Modify: `sim/tools/test_gen_test_patterns_filemaster.py`

**Interfaces:**
- Consumes: `data_width` (bits) from `axi_widths()`.
- Produces: `encode_write_beats(addr, axi_size, axi_len, data_width) -> list[str]`, each `"0x<data> 0x<strb> 0"`. Beat b covers `[beat_addr, beat_addr + 2**axi_size)` where `beat_addr = addr + b*2**axi_size` (INCR). Byte `A` holds `A & 0xFF`; strobe has the beat's active lanes set.

- [ ] **Step 1: Write the failing test**

Add to `test_gen_test_patterns_filemaster.py`:

```python
def test_encode_write_beats_full_width_addr_in_data():
    # DW=256 -> 32 bytes/beat. One full-width beat at 0x1000.
    beats = g.encode_write_beats(0x1000, axi_size=5, axi_len=0, data_width=256)
    assert len(beats) == 1
    data_hex, strb_hex, user = beats[0].split()
    assert user == "0"
    assert strb_hex == "0x" + "f" * 8              # 32 lanes all active
    # byte j (little-endian) == (0x1000 + j) & 0xff
    data = int(data_hex, 16)
    for j in range(32):
        assert (data >> (8 * j)) & 0xFF == (0x1000 + j) & 0xFF


def test_encode_write_beats_multibeat_incr():
    beats = g.encode_write_beats(0x2000, axi_size=5, axi_len=3, data_width=256)
    assert len(beats) == 4                          # len+1 beats
    # beat 2 starts at 0x2000 + 2*32
    data = int(beats[2].split()[0], 16)
    assert data & 0xFF == (0x2000 + 2 * 32) & 0xFF
```

- [ ] **Step 2: Run it, verify it fails**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py -k encode_write_beats -v`
Expected: FAIL (`encode_write_beats` undefined).

- [ ] **Step 3: Implement the encoder**

Add to `gen_test_patterns.py`:

```python
def encode_write_beats(addr, axi_size, axi_len, data_width):
    """file_master W-beat lines: "0x<data> 0x<strb> 0", INCR, full strobe,
    address-in-data (byte A = A & 0xFF). data/strb sized to the DW bus."""
    bus_bytes = data_width // 8
    beat_bytes = 1 << axi_size
    lines = []
    for b in range(axi_len + 1):
        beat_addr = addr + b * beat_bytes
        lane0 = beat_addr % bus_bytes            # byte-lane of the beat's first byte
        data = 0
        strb = 0
        for k in range(beat_bytes):
            lane = lane0 + k
            data |= ((beat_addr + k) & 0xFF) << (8 * lane)
            strb |= 1 << lane
        lines.append(f"0x{data:0{bus_bytes * 2}x} 0x{strb:0{bus_bytes // 4}x} 0")
    return lines
```

- [ ] **Step 4: Run it, verify it passes**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py -k encode_write_beats -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns_filemaster.py
git commit -m "feat(emitter): address-in-data W-beat encoder"
```

---

### Task 3: Per-node file_master emitter

**Files:**
- Modify: `sim/tools/gen_test_patterns.py`
- Modify: `sim/tools/test_gen_test_patterns_filemaster.py`

**Interfaces:**
- Consumes: `alloc_unique_offset`, `encode_write_beats`, `ADDR_DST_SHIFT`.
- Produces: `emit_file_master_node(out_dir, src_idx, dst_cids, n_nodes, base_local, memory_size, axi_size, axi_len, data_width)` — writes `out_dir/write.txt` (12 ax fields + `len+1` W beats per txn) and `out_dir/read.txt` (11 ax fields per txn). One write+read pair per `dst_cid`, same partitioned address.

- [ ] **Step 1: Write the failing test**

Add to `test_gen_test_patterns_filemaster.py`:

```python
import os


def _parse_write(path):
    """Parse a write.txt back into (txns). Mirrors axi_file_master.parse_write field order."""
    toks = open(path).read().split("\n")
    it = iter([t for t in toks if t != ""])
    txns = []
    while True:
        try:
            axid = int(next(it))
        except StopIteration:
            break
        addr = int(next(it), 16)
        length = int(next(it)); size = int(next(it)); burst = int(next(it))
        for _ in range(6):  # lock cache prot qos region atop
            next(it)
        next(it)            # user
        beats = [next(it) for _ in range(length + 1)]
        txns.append({"id": axid, "addr": addr, "len": length, "size": size,
                     "burst": burst, "beats": beats})
    return txns


def test_emit_file_master_node_format_and_partition(tmp_path):
    d = str(tmp_path / "node0")
    # two writes from src 0 to dst tile cid=1, INCR full-width single beats.
    g.emit_file_master_node(d, src_idx=0, dst_cids=[1, 1], n_nodes=16,
                            base_local=0x1000, memory_size=0x40000,
                            axi_size=5, axi_len=0, data_width=256)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert len(w) == 2
    for t in w:
        assert t["burst"] == 1 and t["size"] == 5 and t["len"] == 0
        assert (t["addr"] >> 32) == 1               # dst tile in addr[63:32]
        assert len(t["beats"]) == 1
    # the two writes land on DISJOINT addresses (unique offset per seq)
    assert w[0]["addr"] != w[1]["addr"]
    # read.txt exists, has 2 read txns to the same addresses, no W beats
    rlines = [l for l in open(os.path.join(d, "read.txt")).read().split("\n") if l != ""]
    assert len(rlines) == 2 * 11                    # 11 ax fields, no atop, no beats
```

- [ ] **Step 2: Run it, verify it fails**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py -k emit_file_master_node -v`
Expected: FAIL (`emit_file_master_node` undefined).

- [ ] **Step 3: Implement the emitter**

Add to `gen_test_patterns.py`:

```python
def _ax_fields(axid, addr, axi_len, axi_size, include_atop):
    """The AW/AR field lines in parse_write/parse_read order. Write includes atop
    (12 fields); read omits it (11 fields, matching axi_file_master.parse_read)."""
    lines = [str(axid), f"0x{addr:x}", str(axi_len), str(axi_size),
             "1", "0", "0", "0", "0", "0"]          # burst=INCR lock cache prot qos region
    if include_atop:
        lines.append("0")                            # atop (write only)
    lines.append("0")                                # user
    return lines


def emit_file_master_node(out_dir, src_idx, dst_cids, n_nodes,
                          base_local, memory_size, axi_size, axi_len, data_width):
    """Write out_dir/{write,read}.txt for one node. One write+read pair per dst_cid,
    src-partitioned address, address-in-data payload. INCR, atop=0, full strobe."""
    os.makedirs(out_dir, exist_ok=True)
    reserved = (axi_len + 1) * (1 << axi_size)
    write_lines, read_lines = [], []
    for seq, dst_cid in enumerate(dst_cids):
        local_off = alloc_unique_offset(dst_cid, src_idx, seq, base_local,
                                        n_nodes, memory_size, reserved=reserved)
        addr = (dst_cid << ADDR_DST_SHIFT) + local_off
        write_lines += _ax_fields(0, addr, axi_len, axi_size, include_atop=True)
        write_lines += encode_write_beats(addr, axi_size, axi_len, data_width)
        read_lines += _ax_fields(0, addr, axi_len, axi_size, include_atop=False)
    with open(os.path.join(out_dir, "write.txt"), "w") as f:
        f.write("\n".join(write_lines) + "\n")
    with open(os.path.join(out_dir, "read.txt"), "w") as f:
        f.write("\n".join(read_lines) + "\n")
```

- [ ] **Step 4: Run it, verify it passes**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py -k emit_file_master_node -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns_filemaster.py
git commit -m "feat(emitter): per-node file_master write/read emitter"
```

---

### Task 4: Wire `--format file_master` into main() for all 4 patterns

**Files:**
- Modify: `sim/tools/gen_test_patterns.py`
- Modify: `sim/tools/test_gen_test_patterns_filemaster.py`

**Interfaces:**
- Consumes: `emit_file_master_node`, `axi_widths`, the existing pattern-dst helpers and `_load_topology`.
- Produces: with `--format file_master`, `main()` writes `<out>/node<i>/{write,read}.txt` for every node; neighbor/transpose/uniform_random/hotspot all run synthetic (no `--from`). Default `--format yaml` keeps the existing behavior untouched.

- [ ] **Step 1: Write the failing test**

Add to `test_gen_test_patterns_filemaster.py`:

```python
import subprocess, sys, glob

PATTERNS = [
    ["--pattern", "neighbor"],
    ["--pattern", "transpose"],
    ["--pattern", "uniform_random", "--seed", "1"],
    ["--pattern", "hotspot", "--hotspot", "5", "--seed", "1"],
]


@pytest.mark.parametrize("pat", PATTERNS, ids=lambda p: p[1])
def test_main_file_master_all_patterns(tmp_path, pat):
    out = str(tmp_path / "scn")
    g.main(["--topology", "mesh_4x4_vc1", "--format", "file_master",
            "--out", out, "--transactions-per-node", "2",
            "--size", "5", "--len", "0", "--memory-size", "0x40000"] + pat)
    nodes = sorted(glob.glob(os.path.join(out, "node*")))
    assert len(nodes) == 16
    for n in nodes:
        assert os.path.isfile(os.path.join(n, "write.txt"))
        assert os.path.isfile(os.path.join(n, "read.txt"))
```

(Add `import pytest` at the top of the test file.)

- [ ] **Step 2: Run it, verify it fails**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py -k main_file_master -v`
Expected: FAIL (`--format` unrecognized).

- [ ] **Step 3: Implement the wiring**

In `gen_test_patterns.py` `main()`, add the argument (near `--out`):

```python
    ap.add_argument("--format", choices=["yaml", "file_master"], default="yaml",
                    help="yaml (legacy per-node scenario.yaml) or file_master "
                         "(per-node write.txt/read.txt for pulp axi_file_master)")
```

Then, immediately after `n_nodes = len(nodes)` (before the per-pattern `if` chain), branch to a self-contained file_master path so the legacy YAML code below is untouched:

```python
    if a.format == "file_master":
        widths = axi_widths()
        base_local = 0x1000
        memory_size = a.memory_size if a.memory_size is not None else 0x40000
        rng = _random_module.Random(a.seed)
        if a.pattern == "transpose":
            _check_transpose_guard(x_dim, y_dim)   # square-mesh precondition (legacy parity)
        for (idx, x, y, src_cid) in nodes:
            if a.pattern in ("neighbor", "transpose"):
                dst_x, dst_y = _dst_for(a.pattern, x, y, x_dim, y_dim)
                dst_cids = [coord_id(dst_x, dst_y)] * a.transactions_per_node
            elif a.pattern == "uniform_random":
                dst_lin = uniform_random_dsts(idx, n_nodes, a.transactions_per_node,
                                              rng, a.exclude_self)
                dst_cids = [coord_id(*_linear_to_coord(d, x_dim)) for d in dst_lin]
            else:  # hotspot
                if a.hotspot is None:
                    ap.error("--hotspot is required for the hotspot pattern")
                dst_lin = hotspot_dsts(idx, n_nodes, a.transactions_per_node, rng,
                                       a.hotspot, a.hotspot_rates, a.exclude_self)
                dst_cids = [coord_id(*_linear_to_coord(d, x_dim)) for d in dst_lin]
            emit_file_master_node(os.path.join(a.out, f"node{idx}"), idx, dst_cids,
                                  n_nodes, base_local, memory_size,
                                  a.size, a.burst_len, widths["data"])
        return
```

- [ ] **Step 4: Run it, verify it passes**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py -k main_file_master -v`
Expected: PASS (all 4 patterns).

- [ ] **Step 5: Commit**

```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns_filemaster.py
git commit -m "feat(emitter): --format file_master for all 4 patterns"
```

---

### Task 5: gitignore + full regression of the untouched legacy path

**Files:**
- Modify: `.gitignore`

- [ ] **Step 1: Ignore generated file_master stimulus**

Add to `.gitignore` (near the existing `sim/test_patterns/*/node[2-9]/` rules):

```
# Generated file_master stimulus (emitter --format file_master; all nodes)
sim/test_patterns/*/node*/write.txt
sim/test_patterns/*/node*/read.txt
```

- [ ] **Step 2: Confirm the legacy YAML path is untouched**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns.py -v`
Expected: PASS (all pre-existing tests still green — this stage added code, changed nothing on the YAML path).

- [ ] **Step 3: Run the new emitter tests together**

Run: `cd sim/tools && python3 -m pytest test_gen_test_patterns_filemaster.py -v`
Expected: PASS (all tasks' tests).

- [ ] **Step 4: Commit**

```bash
git add .gitignore
git commit -m "chore(emitter): gitignore generated file_master stimulus"
```

## Self-Review

- **Spec coverage:** implements spec Stage 2 (emitter → file_master format, all 4 patterns, `data=f(addr)`, src-partitioned via `alloc_unique_offset`, INCR/atop=0/full strobe). Widths follow the DUT SSoT per the user's directive.
- **Add-only discipline:** the legacy YAML path, `--from`, `_emit_base_driven_node`, and the existing test file are untouched (Task 5 Step 2 proves it). Deletion of the old flow + `run_benchmark.py` + AX4-YAML/ctest decoupling are Stage 5 / separate-round work, tracked in `docs/backlog.md`.
- **Placeholder scan:** none — every step has runnable code/commands.
- **Type consistency:** `axi_widths()` / `encode_write_beats()` / `emit_file_master_node()` signatures match across Tasks 1-4; `data_width` threads from `axi_widths()["data"]` into the encoder.
- **Format fidelity:** write.txt = 12 ax fields + `len+1` beats; read.txt = 11 ax fields (no atop) — matches `axi_file_master.parse_write`/`parse_read` verified by Codex.
