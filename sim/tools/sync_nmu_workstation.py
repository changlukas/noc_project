#!/usr/bin/env python3
"""Synchronize the prepared NMU source manifest over SSH, without SCP or remote Git.

The SSH login banner is tolerated. Only manifest-owned files are updated;
remote build products, reports and unrelated files are retained.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess

REMOTE = r'''import base64, hashlib, json, os, sys, tempfile, time
payload = json.load(sys.stdin)
root = os.path.realpath(payload["root"])
if not os.path.isdir(root):
    os.makedirs(root)
updated = 0
unchanged = 0
for entry in payload["files"]:
    relative = entry["path"]
    target = os.path.realpath(os.path.join(root, relative))
    if not target.startswith(root + os.sep):
        raise RuntimeError("file escapes sync root: " + relative)
    content = base64.b64decode(entry["data"])
    digest = hashlib.sha256(content).hexdigest()
    if digest != entry["sha256"]:
        raise RuntimeError("transfer checksum mismatch: " + relative)
    if os.path.isfile(target):
        with open(target, "rb") as current:
            if hashlib.sha256(current.read()).hexdigest() == digest:
                if os.stat(target).st_mtime > time.time():
                    now = time.time()
                    os.utime(target, (now, now))
                unchanged += 1
                continue
    parent = os.path.dirname(target)
    if not os.path.isdir(parent):
        os.makedirs(parent)
    descriptor, temporary = tempfile.mkstemp(prefix=".noc-sync-", dir=parent)
    try:
        with os.fdopen(descriptor, "wb") as destination:
            destination.write(content)
        os.chmod(temporary, 0o644)
        os.rename(temporary, target)
        now = time.time()
        os.utime(target, (now, now))
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    updated += 1
for entry in payload["files"]:
    with open(os.path.join(root, entry["path"]), "rb") as current:
        if hashlib.sha256(current.read()).hexdigest() != entry["sha256"]:
            raise RuntimeError("readback checksum mismatch: " + entry["path"])
print("NOC_SYNC_RESULT=" + json.dumps({"root": root, "updated": updated,
    "unchanged": unchanged, "verified": len(payload["files"])}))
'''


def sync(source, host, remote_dir, ssh, key):
    source = Path(source).resolve()
    entries = []
    names = ["SHA256SUMS"]
    for line in (source / "SHA256SUMS").read_text().splitlines():
        expected, name = line.split("  ", 1)
        path = (source / name).resolve()
        if not path.is_relative_to(source):
            raise ValueError("manifest path escapes source")
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError("local manifest checksum mismatch: " + name)
        names.append(name)
    for name in names:
        content = (source / name).read_bytes()
        entries.append({"path": name, "sha256": hashlib.sha256(content).hexdigest(),
                        "data": base64.b64encode(content).decode("ascii")})
    code = base64.b64encode(REMOTE.encode()).decode("ascii")
    command = 'python3 -c "import base64;exec(base64.b64decode(\'' + code + '\'))"'
    invocation = [ssh, "-i", key, "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, command]
    result = subprocess.run(invocation, input=json.dumps({"root": remote_dir, "files": entries}).encode(),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
    stdout = result.stdout.decode(errors="replace")
    if result.returncode:
        raise RuntimeError(stdout + result.stderr.decode(errors="replace"))
    marker = "NOC_SYNC_RESULT="
    messages = [line[len(marker):] for line in stdout.splitlines() if line.startswith(marker)]
    if len(messages) != 1:
        raise RuntimeError("SSH completed without a verified synchronization result: " + stdout)
    print(json.dumps(json.loads(messages[0]), indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--host", default="mingwei@172.16.16.16")
    parser.add_argument("--remote-dir", default="/home/mingwei/noc_project/nmu-standalone")
    parser.add_argument("--ssh", default="/mnt/c/Windows/System32/OpenSSH/ssh.exe")
    parser.add_argument("--key", default=r"C:\Users\user\.ssh\noc_workstation_ed25519")
    args = parser.parse_args()
    sync(args.source, args.host, args.remote_dir, args.ssh, args.key)
