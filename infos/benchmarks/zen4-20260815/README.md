# AMD Zen 4 capture index

Host: AMD EPYC 4245P, 6 cores / 12 threads, 32 MiB L3, GCC 13.3. The machine exposed AVX2 and AVX-512 and was
terminated after the captures were copied locally.

The logs preserve experimentation, including failed or superseded runs:

- `run.log`: broad initial capture across the dense primitive and indexed experiments.
- `zen4_rerun.log`, `zen4_rerun2.log`: corrected reruns after benchmark-methodology fixes.
- `stringwars_*.log`: focused StringWars captures on generated, Leipzig, and ACGT inputs.
- `sweep_*.log`: compiler-target and workload sweeps.
- `k9_remeasure.log`: high-bound exploratory remeasurement.
- `within_k_bench.patch`: exact StringWars working patch copied from the server; the reviewed equivalent is pushed at
  [`grouville/StringWars@1f81925`](https://github.com/grouville/StringWars/commit/1f81925).

Use the final tables and caveats in `../../LEVENSHTEIN-INDEX-DESIGN.md`. In particular, portable x86-64,
Haswell/AVX2, and native AVX-512 were compiler targets on this one AMD host, not independent architectures.
