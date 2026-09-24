import csv,hashlib,json,subprocess
from pathlib import Path
root=Path('/home/mingwei/noc_project/nmu-standalone/cosim')
out=root/'build/rx-channel-order'
wave=root/'build/report_wave1/cross_id_out_of_order.fsdb'
rc=root/'signals.rc'
signals=[line.split()[-1] for line in rc.read_text().splitlines() if line.startswith('addSignal ')]
aliases=['sig%04d'%i for i in range(len(signals))]
cmd=['/cadtools/synopsys/verdi/M-2017.03-SP1/bin/fsdbreport',str(wave),'-bt','0ns','-et','1ns','-csv','-s']
for signal,alias in zip(signals,aliases):cmd.extend([signal,'-a',alias])
cmd.extend(['-o',str(out/'rc-signals.csv')])
with (out/'rc-check.log').open('w') as log:
 result=subprocess.call(cmd,stdout=log,stderr=subprocess.STDOUT)
if result:raise RuntimeError('fsdbreport failed')
with (out/'rc-signals.csv').open() as f:header=next(csv.reader(f))
missing=[signal for signal,alias in zip(signals,aliases) if not any(alias in x for x in header)]
standalone=root.parent/'script/nWaveLog/signals.rc'
changed=[line.split()[-1] for line in standalone.read_text().splitlines() if line.startswith('addSignal ') and ('/i_rx_channel_assign/' in line or '/i_rx_buffer/i_rsp_fifo/' in line)]
unmapped=[x for x in changed if x.replace('/tb_nmu_standalone/','/tb_nmu_cosim/') not in signals]
record={'cosim_signals':len(signals),'missing':missing,'standalone_changed_paths_mapped_to_same_dut':len(changed),'unmapped':unmapped,'fsdb_sha256':hashlib.sha256(wave.read_bytes()).hexdigest()}
(out/'rc-check.json').write_text(json.dumps(record,indent=2))
print(json.dumps(record),flush=True)
if missing or unmapped:raise SystemExit(1)
