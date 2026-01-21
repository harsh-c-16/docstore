# DocStore design notes

This file records every design decision, the alternatives that were rejected, the lock order,
and every place the implementation deviates from its original requirements. It grows with the project.

## Decisions

| # | Decision | Alternatives / reason |
|---|---|---|
| D1 | Dependencies are pulled as release tarballs with a SHA-256 (`URL_HASH`) instead of `GIT_TAG`. | Same pinned versions (json v3.11.3, googletest v1.15.2); the hash pins the exact bytes and a tarball download is faster than a clone. |
| D2 | One test binary (`docstore_tests`); every gtest case is registered with ctest under the `fast` label. The `long` label re-runs the scalable tests with `DOCSTORE_LONG=1`. | A binary per component links slower and duplicates setup; one binary still lets `ctest -j` run cases in parallel. |
| D3 | Tests keep scratch files in `build/<preset>/test_tmp` (via `TEST_TMPDIR`), not `/tmp`. | `/tmp` is a tmpfs on many machines, which would hide fsync behaviour and fill RAM. |

## Deviations from the original requirements

None yet.

## Lock order

To be written.
