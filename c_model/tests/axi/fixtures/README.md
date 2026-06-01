# axi/fixtures — Integration test scenarios

Scenarios inspired by alexforencich/cocotbext-axi `test_axi.py` directed cases (MIT).
Each `<name>.yaml` describes a scenario; the matching `<name>_data.txt` (when
present) holds hex per-beat payload bytes used by `data_file:` transactions and
also serves as the expected `dump_file` content for file-diff verification.

## Phase A coverage

- Single write/read aligned (`single_write_read_aligned`)
- INCR bursts of len 2 and 8 beats (`burst_incr_2beat`, `burst_incr_8beat`)
- Multi-transaction same-ID and different-ID (`multi_txn_same_id`, `multi_txn_diff_id`)
- DECERR paths: OOB write, OOB read, burst crossing OOB boundary
  (`decerr_oob_write`, `decerr_oob_read`, `burst_crosses_oob_boundary`)
- Default fill: read of an unwritten address returns 0x00
  (`single_read_default_fill`)
- Latency stress: `write_latency = read_latency = 20` (`latency_stress`)
- Backpressure retry: small memory pending queue + multi outstanding
  (`backpressure_retry`)
- Multi-outstanding stress: `max_outstanding_{write,read} = 8`
  (`multi_outstanding_stress`)

## Data file format

- One AXI beat per line.
- `DATA_BYTES = 32` (= `WSTRB_WIDTH`) hex bytes per beat, space-separated.
- For sub-beat transactions (`size < 5`), only the first `1 << size` bytes of
  each line are observed by the dump, so the data file should still provide a
  full 32-byte line (extra bytes are ignored on the read side).

## Per-fixture data files

Each diff-pass fixture owns its own `<name>_data.txt` whose contents equal the
expected `dump_file` output exactly (one line per beat, in the order beats are
returned). This keeps the file diff trivially comparable.

## Adding fixtures

1. Add `<name>.yaml` and (if diff is expected) `<name>_data.txt`.
2. Add a `FixtureParam` entry to `INSTANTIATE_TEST_SUITE_P` in
   `c_model/tests/axi/test_integration.cpp`.
3. Rebuild — the CMake `POST_BUILD` step copies `fixtures/` into the test bin
   directory.
