const flooNoCPath = process.env.FLOONOC_REFERENCE ?? "E:/05_NoC/FlooNoC";
const taxiPath = process.env.TAXI_REFERENCE ?? "E:/03_Learning/taxi";

export default {
  image: "ic-design-team:latest",
  baseBranch: "main",
  maxParallel: 1,
  references: [
    {
      name: "FlooNoC",
      path: flooNoCPath,
      policy: "production-approved",
    },
    {
      name: "Taxi",
      path: taxiPath,
      policy: "reference-only",
    },
  ],
  verification: [
    {
      name: "baseline",
      clean: "make clean-generated",
      command: "python3 specgen/tools/codegen.py --check && git diff --check",
    },
  ],
};
