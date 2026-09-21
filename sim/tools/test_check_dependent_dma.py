"""Failure reporting for executable DMA functional checks."""

import subprocess

import pytest

import check_dependent_dma as checker


def test_timeout_preserves_partial_simulator_output(tmp_path, monkeypatch):
    def timeout(command, **kwargs):
        assert kwargs["timeout"] == 180
        raise subprocess.TimeoutExpired(command, 180, output=b"partial stdout\n",
                                        stderr=b"partial stderr\n")

    monkeypatch.setattr(checker.subprocess, "run", timeout)
    with pytest.raises(RuntimeError, match="wall-clock timeout after 180s"):
        checker.run_checks("unused-binary", "mesh_4x4", 64, tmp_path, "read",
                           timeout_seconds=180)
    assert (tmp_path / "read" / "run.log").read_text() == "partial stdout\npartial stderr\n"
    assert not (tmp_path / "read" / "operation.json").exists()


@pytest.mark.parametrize("limit", [0, -1])
def test_rejects_nonpositive_timeout(tmp_path, limit):
    with pytest.raises(ValueError, match="positive"):
        checker.run_checks("unused-binary", "mesh_4x4", 64, tmp_path, "read",
                           timeout_seconds=limit)
