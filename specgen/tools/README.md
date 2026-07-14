# specgen/tools/

Codegen tooling for the NoC NI spec.

## Workflow

Edit `specgen/generated/json/*.json` directly. The JSON files are the
canonical source. Regenerate downstream cpp/sv constants:

```bash
py -3 specgen/tools/codegen.py --target cpp --domain packet
py -3 specgen/tools/codegen.py --target sv  --domain packet
# (similar for signals, params)
```

Drift gate (CI):

```bash
py -3 specgen/tools/codegen.py --check
```
