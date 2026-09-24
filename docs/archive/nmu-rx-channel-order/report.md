# NMU RX channel-assignment order validation

RX now follows NoC -> RSP/DAT input FIFO -> channel assignment -> unpack -> ordering/ROB -> AXI output CDC. RSP uses one raw FIFO, preserving arrival order and waiting behind a blocked B or control-R head. DAT retains independent per-read-VC storage. No payload FIFO was added between assignment and unpack. Default RSP depth is 32; ROB, CDC and unpack register parameters are unchanged.

VCS M-2017.03-SP1 on be16: one focused RX test and five co-simulation cases passed. Validation was deliberately limited per user request; no full regression or C++ model rebuild.

| Case | Writes | Reads | R beats | Checked bytes | Result |
|---|---:|---:|---:|---:|---|
| ctrl_write_burst | 12 | 12 | 56 | 210 | PASS |
| ctrl_read_burst | 12 | 12 | 56 | 210 | PASS |
| data_write_burst | 12 | 12 | 56 | 694 | PASS |
| data_read_burst | 12 | 12 | 56 | 694 | PASS |
| cross_id_out_of_order | 16 | 16 | 16 | 128 | PASS |

Focused test: NUM_DAT_VC=2, DAT_VC_MODE=0, DAT_RX_VC_DEPTH=2, RSP_FIFO_DEPTH=2, REG_TYPE=0. It checks blocked B then R heads, no RSP bypass, full/recovery, independent DAT progress, payload/metadata, stalled R stability, reset flush and per-VC credit conservation. The existing throughput check completes 64 DAT beats in 65 cycles. Cross-ID integration observed B and R ingress disorder (b_ooo=1, r_ooo=12). It does not claim same-ID ROB retirement coverage in this round.

All integration runs used seed 1, MODE=auto, WAVE=1, output REG_TYPE=0 and default transport depth 32. RSP delay is enabled only by the ordering pattern. Four burst cases include initialized memory readback. This round does not rerun register-mode/VC-count matrices, capacity patterns, corruption tests or in-flight reset integration.

## Commands and evidence

On be16 under `/home/mingwei/noc_project/nmu-standalone/cosim`:

```sh
python3 test_pipeline.py --rx-only --report build/rx-channel-order/focused
make compile WAVE=1
make sim WAVE=1 CASE=ctrl_write_burst
make sim WAVE=1 CASE=ctrl_read_burst
make sim WAVE=1 CASE=data_write_burst
make sim WAVE=1 CASE=data_read_burst
make sim WAVE=1 CASE=cross_id_out_of_order
```

Reports: `build/rx-channel-order/` and the five corresponding `build/report_wave1/<case>.log/.fsdb` files. `collect.py` retrieved 38 reports/configuration files with SHA256 verification. Raw files remain in local `remote/`; the tracked manifest records their hashes. `results.json` contains checked counts and library metadata.

Both workstation source trees were synchronized and SHA256 verified: 675 co-simulation files and 204 standalone files. The final C++ DPI library hash, size and mtime exactly match the pre-run values. The dry-run compile recipe confirmed no model compilation. Existing workstation clock-skew warnings remain; the co-simulation build uses a fresh source-content-keyed directory and all simulations passed.

Co-simulation RC: all 515 signal paths resolve in the new cross-ID FSDB. The 20 changed standalone RC paths map to the same verified NMU hierarchy; a new standalone simulation was not run. Both RC originals and the migration map are archived. GUI rendering was not exercised.

Current specification references updated: `docs/nmu-spec.md`, `docs/nmu-verification-plan.md`, `docs/trade-off.md` and `rtl/README.md`. Pre-existing unrelated local document edits are preserved; `document-updates.patch` records this round's complete document delta. Issue #84 remains OPEN for user acceptance.
