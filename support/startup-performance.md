# Startup loading optimizations

The native server reuses an open settings directory while reading its children,
using the existing filebundle API so packaged-default fallback and compression
handling stay intact. Sorted idnode sets locate the insertion point with a binary
search and shift the remaining pointers with `memmove`, preserving duplicate
ordering and the append fast path. Array insertion still has linear copy cost;
this does not turn sorted arrays into a logarithmic-insertion data structure.

## Local validation (2026-11-12)

Compared a baseline built from `40e88d4ce` against the modified build. Each run
loaded a fresh copy of the same configuration, containing 58,868 channels and
58,865 muxes. Timed process launch through the server's START readiness message.
Used isolated loopback ports, disabled Linux DVB adapters and SAT>IP discovery,
and stopped each test process after querying the API. The final pair ran in
reverse order. These are local filesystem-cache benchmarks on tmpfs, not cold-disk, cold-boot
or full TVHplayer UI timings. They predate the stricter 2026-11-13 investigation;
these timings must not be presented as cold-disk performance.

| Pair | Baseline | Optimized |
| --- | ---: | ---: |
| 1 | 20.811 s | 17.035 s |
| 2 | 20.354 s | 14.974 s |
| 3 (reverse order) | 19.929 s | 17.201 s |
| Median | 20.354 s | 17.035 s |

Median startup improved by 16.3%. Median bouquet resolution improved from
3.357 s to 1.777 s (47.1%).

All six runs produced identical sorted API snapshots for the selected fields:

- Channels: UUID, name, number, enabled, services, tags (58,868 rows).
- Channel tags: UUID, name, enabled (1,034 rows).
- Muxes: UUID, enabled, IPTV URL (58,865 rows).
- Bouquets: UUID, name, enabled, channel mapping (21 rows).

## Regression checks

After configuring/building the native server:

```sh
sh support/test-idnode-set.sh
CFLAGS='-fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined' \
  sh support/test-idnode-set.sh
python3 support/test-settings-load.py
```

The set test compiles the production implementation and checks ascending,
descending, duplicate, random, unsorted and filtered insertions against an
independently sorted expected array, using 60,000 entries per case. The loader
test invokes the built executable's real functions under GDB without continuing
main or starting a server. It requires symbols, zlib and bundled defaults; it
covers individual files, directory reads, recursion, gzip, symlinks, ignored
hidden/backup files, missing paths and packaged fallback. All checks passed,
including AddressSanitizer and UndefinedBehaviorSanitizer for set insertion.
