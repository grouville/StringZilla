# Levenshtein benchmark evidence

This directory preserves raw development captures supporting the temporary Levenshtein PR draft. The concise,
reviewed interpretation lives in `../LEVENSHTEIN-INDEX-PR-DRAFT.md`; raw logs include intermediate runs and must not
be quoted without checking the final methodology and commit named by the draft.

- `zen4-20260815/`: AMD EPYC 4245P captures, including corrected StringWars runs and the exact patch used remotely.
- `intel-coffee-lake-20260815.md`: independent local Intel AVX2 replication.

The public benchmark harnesses live under `bench/`. Large generated result streams are omitted because the harnesses
reproduce their versioned format and the reviewed documents record their SHA-256 hashes.
