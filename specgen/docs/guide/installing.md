# Installing

This toolchain is a directory-based Python project. There is no `pip install`.

## Prerequisites

- **Python 3.9 or newer.** On Windows use `py -3`; on Linux and macOS use `python3`.
- **Git**, for `--check` to compare against committed artifacts.

## Optional dependencies

| For | Tool | Notes |
|-----|------|-------|
| Compile the sample C++ program | `g++` (C++17) | Or `cl.exe /std:c++17` on MSVC. Auto-skipped in tests if not on PATH. |
| Run the SV lint smoke test | `verilator` | `--lint-sv` mode exits cleanly when verilator is missing. |
| Run the Python test suite | `pytest` | `py -3 -m pip install pytest` |
| Params domain SSoT | `pyyaml` | Required for `--domain params` (`source/constants.yaml` is YAML). |

## Verifying the install

From the repo root:

```bash
py -3 specgen/tools/codegen.py --help
```

The argparse help text should list `--target`, `--domain`, `--check`, and `--lint-sv`. If you see `ModuleNotFoundError: No module named 'ni_spec'`, you are running from the wrong directory — see [Troubleshooting](troubleshooting.md).
