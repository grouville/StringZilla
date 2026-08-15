# Independent Intel AVX2 replication

## Environment

- CPU: Intel Core i5-9300H, Coffee Lake, 4 cores / 8 threads, 8 MiB L3
- compiler: GCC 13.3.0
- flags: `-O3 -DNDEBUG -std=c++20 -march=haswell -mtune=haswell`
- affinity: `taskset -c 2`
- StringZilla commit: `6a16d64f`
- RapidFuzz commit: `b5830af53bd1b3c7460a8de1e9f7095df99b3470`
- dictionary: 370,105 lines, SHA-256 `3ed0c94610d8bcf7c11bbb49c56aa49c7234d32b66824df91f554169e572da48`
- queries: current checked-in generator, 10,000 `mixed` queries, seed 243, SHA-256
  `69a36c6f27e70fe548b664cb159fb519b472f199a66d430dbc2553a4abc819b9`

The historical AMD query file had not been copied from the server. This run is therefore an independent deterministic
replication, not a cross-machine comparison on byte-identical query input.

## Warm serial results

| Engine | Repeats | `k=1` median | `k=2` median | Matches `k=1 / k=2` |
|:---|---:|---:|---:|---:|
| StringZilla | 10 / 7 | 8.274 ms | 88.167 ms | 12,053 / 144,160 |
| RapidFuzz cached scan | 3 / 3 | 41.304 s | 69.228 s | 12,053 / 144,160 |
| Speedup | | 4,992x | 785x | exact parity |

StringZilla repeat seconds:

- `k=1`: 0.00911828, 0.00825914, 0.00823398, 0.00840632, 0.00827733, 0.00826982, 0.00823725,
  0.00858792, 0.00823806, 0.00834753
- `k=2`: 0.0879013, 0.0897462, 0.0887331, 0.0880920, 0.0868363, 0.0881671, 0.0931255

RapidFuzz repeat seconds:

- `k=1`: 41.3044, 41.4082, 41.2722
- `k=2`: 69.2366, 69.0562, 69.2276

Single observed StringZilla builds took 0.367 s and 2.006 s. Persistent index bytes were 34,422,900 and 134,048,996;
the owned dictionary occupied 6,455,548 bytes.

## Exactness

StringZilla and RapidFuzz independently emitted the same sorted `(u32 dictionary ID, u8 distance)` stream at both
bounds; `cmp` succeeded and both sides produced:

- `k=1`: `5f47ecede8837788287f20e2deb6b7567bacfd2617e76876ae34607bec171222`
- `k=2`: `8af4d3b1d54ad6efb1945af718c813e81c487e05642da537411c82229936933f`

Laptop frequency and thermal state were not controlled. These measurements establish exact Intel AVX2 portability
and a large qualitative indexed-retrieval margin; they are not an ISA comparison with the server.
