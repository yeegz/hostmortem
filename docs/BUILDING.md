# Building, testing and reproducing every result

Everything runs in Docker, so a macOS or Linux host only needs Docker (on
macOS, Colima works). Nothing needs real hardware or a cloud account.
Verified from a clean `git clone` on 14 Nov 2026 (82 tests then; 92 as of v0.2.0, 24 Nov 2026).

```bash
git clone <repo> hostmortem && cd hostmortem
scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'
```

`scripts/dev.sh` builds the `hostmortem-dev:0` image (Ubuntu 24.04, GCC 13,
Clang 18, CMake, SQLite, GoogleTest) on first use and runs the command with the
repository mounted at `/src`.

| What | Command | Expect |
|---|---|---|
| Unit, integration and scenario tests | `scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'` | 92 tests pass |
| ASan + UBSan | same with preset `asan` | 92 pass |
| TSan | `HM_DOCKER_FLAGS="--security-opt seccomp=unconfined" scripts/dev.sh 'cmake --preset tsan && cmake --build --preset tsan && setarch -R ctest --preset tsan'` | 92 pass (`setarch -R`: TSan needs ASLR off on 6.x arm64 kernels) |
| Fuzzing | `scripts/dev.sh 'cmake --preset clang-fuzz && cmake --build --preset clang-fuzz && build/clang-fuzz/tests/fuzz/fuzz_kmsg -max_total_time=60 tests/fuzz/corpus/kmsg'` | no crash; `tests/fuzz/regressions/` holds past crashes |
| Real-fault lab (11 scenarios, ~13 min) | `scripts/lab.sh` | `lab/out/summary.txt`: all ok; HTML reports in `lab/out/` |
| Hard reset of a VM mid-incident (~2 min) | `scripts/hard-reset-test.sh` | `lab/out/vm/result.txt`: PASS; loss < 5 s; incident spans 2 boots |
| Pipeline benchmark | `scripts/dev.sh 'cmake --preset release && cmake --build --preset release && bench/run_bench.sh 10 4 5000 5'` | raw rows in `bench/results-*.csv`; summary printed |
| Debian package (as released: oldest supported glibc) | `docker run --rm -v "$PWD":/src -w /src ubuntu:22.04 bash -c 'apt-get update && apt-get install -y build-essential cmake ninja-build python3 file dpkg-dev libsqlite3-dev libgtest-dev libgmock-dev nlohmann-json3-dev && cmake -S . -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build/pkg --target package'` | `build/pkg/hostmortem_0.2.0_<arch>.deb`, `Depends: libc6 (>= 2.34)` |
| Package install test | `mkdir -p lab/out/deb && cp build/release/*.deb lab/out/deb/ && cp scripts/deb-install-test.sh lab/out/deb/install-test.sh && docker run --rm -v "$PWD/lab/out/deb":/t -w /t ubuntu:24.04 bash install-test.sh` | user created, store 0640, `systemd-analyze security` exposure printed, purge removes all (no systemd in a container) |
| Fresh-VM smoke test with real systemd | on a throwaway VM (e.g. `limactl create --name=t template://debian-12`), from the repo: `scripts/install-smoke.sh DIR_WITH_DEB_AND_SHA256SUMS [--upgrade-from 0.1.0] [--footprint 60]` | `SMOKE PASS`; prints a `FOOTPRINT` line |

The lab and the hard-reset test need privileged containers (`--privileged`
for network namespaces; QEMU runs unprivileged). They change nothing outside
the throwaway container.

## Installing on a real Linux box

See [QUICKSTART.md](QUICKSTART.md). In short:

```bash
curl -fsSL https://github.com/yeegz/hostmortem/releases/latest/download/install.sh | sudo sh
sudo hm drill && sudo hm report
```

A locally built package installs the same way: put the `.deb` and a
`SHA256SUMS` file (`sha256sum *.deb > SHA256SUMS`) in one directory and run
`sudo HM_DEB_DIR=that/dir sh install.sh`.

Edit `/etc/hostmortem/hmd.conf` (every key is documented there), then run
`sudo systemctl restart hmd`.

## Releases

Pushing a `v*` tag runs `.github/workflows/release.yml`. It builds and tests on
amd64 and arm64, packages both `.deb`s, install-tests each in a clean container,
and creates a public GitHub Release with `SHA256SUMS`.

## Release checklist (owner)
1. `ctest` on debug/asan/tsan, `scripts/lab.sh`, `scripts/hard-reset-test.sh`
   all green on the release commit.
2. Bench re-run; `docs/PERFORMANCE.md` updated.
3. Build the `.deb` for amd64 and arm64 (CI runners), run the install test, write SHA-256 sums.
4. Tag the version (for example `v0.1.1`) and push the tag; the workflow publishes it.
