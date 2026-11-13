# Building, testing and reproducing every result

Everything runs in Docker, so a macOS or Linux host only needs Docker (on
macOS, Colima works). Nothing needs real hardware or a cloud account.
Verified from a clean `git clone` on 14 Nov 2026.

```bash
git clone <repo> hostmortem && cd hostmortem
scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'
```

`scripts/dev.sh` builds the `hostmortem-dev:0` image (Ubuntu 24.04, GCC 13,
Clang 18, CMake, SQLite, GoogleTest) on first use and runs the command with the
repository mounted at `/src`.

| What | Command | Expect |
|---|---|---|
| Unit, integration and scenario tests | `scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'` | 82 tests pass |
| ASan + UBSan | same with preset `asan` | 82 pass |
| TSan | `HM_DOCKER_FLAGS="--security-opt seccomp=unconfined" scripts/dev.sh 'cmake --preset tsan && cmake --build --preset tsan && setarch -R ctest --preset tsan'` | 82 pass (`setarch -R`: TSan needs ASLR off on 6.x arm64 kernels) |
| Fuzzing | `scripts/dev.sh 'cmake --preset clang-fuzz && cmake --build --preset clang-fuzz && build/clang-fuzz/tests/fuzz/fuzz_kmsg -max_total_time=60 tests/fuzz/corpus/kmsg'` | no crash; `tests/fuzz/regressions/` holds past crashes |
| Real-fault lab (8 scenarios, ~10 min) | `scripts/lab.sh` | `lab/out/summary.txt`: 8 × ok; HTML reports in `lab/out/` |
| Hard reset of a VM mid-incident (~2 min) | `scripts/hard-reset-test.sh` | `lab/out/vm/result.txt`: PASS; loss < 5 s; incident spans 2 boots |
| Pipeline benchmark | `scripts/dev.sh 'cmake --preset release && cmake --build --preset release && bench/run_bench.sh 10 4 5000 5'` | raw rows in `bench/results-*.csv`; summary printed |
| Debian package | `scripts/dev.sh 'cmake --preset release && cmake --build --preset release --target package'` | `build/release/hostmortem_0.1.0_<arch>.deb` |
| Package install test | `cp build/release/*.deb lab/out/deb/ && docker run --rm -v "$PWD/lab/out/deb":/t -w /t ubuntu:24.04 bash install-test.sh` | user created, store 0640, `systemd-analyze security` 2.2 OK, purge removes all |

The lab and the hard-reset test need privileged containers (`--privileged`
for network namespaces; QEMU runs unprivileged). They change nothing outside
the throwaway container.

## Installing on a real Linux box

```bash
sudo apt install ./hostmortem_0.1.0_arm64.deb
sudo systemctl enable --now hmd
sudo usermod -aG hostmortem "$USER"   # then log in again to use hm
hm doctor
```

Edit `/etc/hostmortem/hmd.conf` (every key is documented there), then run
`sudo systemctl restart hmd`.

## Release checklist (owner)
1. `ctest` on debug/asan/tsan, `scripts/lab.sh`, `scripts/hard-reset-test.sh`
   all green on the release commit.
2. Bench re-run; `docs/PERFORMANCE.md` updated.
3. Build the `.deb` for amd64 and arm64 (CI runners), run the install test, write SHA-256 sums.
4. Decide: open the source or ship binaries only. Then tag `v0.1.0` and publish.
