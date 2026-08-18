import { createSandbox } from "@ai-hero/sandcastle";
import { docker } from "@ai-hero/sandcastle/sandboxes/docker";

await using sandbox = await createSandbox({
  branch: "sandcastle/noc-docker-smoke",
  copyToWorktree: ["sim/verilator/Makefile"],
  sandbox: docker({
    imageName: "noc-dev:verilator-5.048",
    containerUid: 1000,
    containerGid: 1000,
  }),
});

const result = await sandbox.exec("make -C sim/verilator hello");
process.stdout.write(result.stdout);
process.stderr.write(result.stderr);

const restore = await sandbox.exec("git restore -- sim/verilator/Makefile");
if (restore.exitCode !== 0) {
  throw new Error(`Failed to clean sandbox worktree:\n${restore.stderr}`);
}

process.exitCode = result.exitCode;
