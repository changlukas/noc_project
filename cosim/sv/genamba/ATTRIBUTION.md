# OSS Attribution — cosim/sv/genamba/

AMBA AXI verification IP from the gen_amba project, vendored for the gen_amba
integration feasibility spike (used as golden AXI master BFM + memory model under
Verilator). Files are vendored unmodified from upstream.

## Upstream

- Source: https://github.com/adki/gen_amba_2021
- Commit: 4ba7903c569b25439f8d89362662c77b9f20a31e
- License: 2-clause BSD

## Files

| This repo                                | Upstream path                                   | Status     |
|------------------------------------------|--------------------------------------------------|------------|
| `cosim/sv/genamba/mem_axi.v`            | `gen_amba_axi/verification/ip/mem_axi.v`         | Unmodified |
| `cosim/sv/genamba/mem_axi_beh.v`        | `gen_amba_axi/verification/ip/mem_axi_beh.v`     | Unmodified |
| `cosim/sv/genamba/mem_axi_dpram_sync.v` | `gen_amba_axi/verification/ip/mem_axi_dpram_sync.v` | Unmodified |
| `cosim/sv/genamba/axi_master_tasks.v`   | `gen_amba_axi/verification/ip/axi_master_tasks.v`| Unmodified |
| `cosim/sv/genamba/mem_test_tasks.v`     | `gen_amba_axi/verification/ip/mem_test_tasks.v`  | Unmodified |
| `cosim/sv/genamba/axi_tester.v`         | `gen_amba_axi/verification/ip/axi_tester.v`      | Unmodified (template/reference for the BFM signal environment) |

## Notes

Only the point-to-point spike IP is vendored (golden master tasks + memory model);
the gen_amba crossbar generator and RTL are not included — they belong to the later
crossbar/role-2 effort. 2-clause BSD permits redistribution with this notice.
