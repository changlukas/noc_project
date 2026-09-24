# Validation corrections

1. Initial output arbitration connected raw arbiter grant to FIFO pop. In the upstream ready/valid arbiter, grant may be high without a request. The unchanged FIFO assertion caught empty reads. Pop and credit_take now use grant AND eligible nonempty request.
2. Increasing B output depth to 32 made the original single-ID capacity pattern unable to fill the receive B FIFO. Its saturation assertion failed as intended. The workload was strengthened to multiple IDs/destinations with disjoint memory, without removing checks.
3. The extended transaction count exposed an existing generator AWUSER marker overflowing into collective control at transaction 256. Opaque user markers are now masked to their defined 8-bit field. No collective protocol or DUT check was changed.

Two alternatives to the capacity workload change were considered: use explicitly smaller FIFO profiles, or enlarge the SAM aperture. Default-depth coverage was retained, and the existing address map was preserved by distributing requests over the four existing destinations. Lower-depth tests remain separate.

4. Final standalone audit found its synthetic response sender still seeded from Router receive depth. It now reads dut.DAT_RX_VC_DEPTH, so the advertised capacity follows the actual NMU receiver rather than an unrelated transmit destination.

5. Final alignment split two equality operators into separate tokens. The standalone VCS compile caught this before simulation. The operators were restored, all 12 formatted RTL files were lexically compared with the exact tested source blobs recovered from Git, and post-format lint passed. Whitespace-stripped character equality alone is insufficient for this check.
