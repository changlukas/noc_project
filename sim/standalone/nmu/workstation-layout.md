# NMU workstation layout

All NMU workstation environments are under /home/mingwei/noc_project/nmu-standalone/.

- Root: the existing deterministic standalone loopback environment, including script/, build/ and user waveform RC files.
- cosim/: RTL NMU with one C++ Router/NSU and AXI memory, plus its independent build cache, reports and 22-case pattern.txt.
- archive/nmu-vcs-validation-20260922/: retained September 22 standalone validation snapshot. Its sources use older names and its logs describe that earlier version. It is historical evidence, not the current source tree.

From nmu-standalone/:

```sh
make list
make list TESTBENCH=cosim
make run TESTBENCH=cosim CASE=data_partial_write
make nWave TESTBENCH=cosim CASE=data_read_write
```

Commands without TESTBENCH select the existing standalone environment. Direct commands inside cosim/ also work. Both environments use some identical case names, so select the intended testbench explicitly when running memory acceptance.

Existing reports, FSDB files and C++ libraries were moved intact. The active co-sim SV binary is linked at the new path because old binaries embedded the previous library path. C++ libraries are reused. Archived logs retain their original path strings as evidence. The root standalone clean target cleans its own products and preserves cosim/ and archive/.

The development checkout remains /home/agent/projects/noc_project in WSL. Source organization remains sim/standalone/nmu and sim/cosim/nmu, with shared sim/test_patterns. The retained Windows checkout is unchanged.
