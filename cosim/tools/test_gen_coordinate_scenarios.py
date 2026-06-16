import os, subprocess, sys, tempfile, yaml

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen_coordinate_scenarios.py")
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(REPO, "tests", "scenarios",
                   "AX4-BAS-001_single_write_no_read", "scenario.yaml")


def test_node1_variant_shifts_addr_and_base():
    with tempfile.TemporaryDirectory() as out:
        subprocess.run([sys.executable, GEN, SRC, out], check=True)
        n0 = yaml.safe_load(open(os.path.join(out, "node0", "scenario.yaml")))
        n1 = yaml.safe_load(open(os.path.join(out, "node1", "scenario.yaml")))
        # node0 = identity for addresses/base
        assert int(str(n0["transactions"][0]["addr"]), 0) == 0x1000
        assert int(str(n0["config"]["memory_base"]), 0) == 0x1000
        # node1 = +0x100000000 on addr and memory_base
        assert int(str(n1["transactions"][0]["addr"]), 0) == 0x1000 + 0x100000000
        assert int(str(n1["config"]["memory_base"]), 0) == 0x1000 + 0x100000000
        # data_file rewritten to an absolute path that exists
        assert os.path.isabs(n1["transactions"][0]["data_file"])
        assert os.path.exists(n1["transactions"][0]["data_file"])
