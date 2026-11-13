# Allocates until killed; used by the oom_kill lab scenario. Runs as
# /tmp/victim (a renamed python3) so the process watch sees comm "victim".
#
# Fail-safe: the scenario must only ever OOM *its own cgroup*. If the 64 MiB
# cgroup limit were missing, unbounded allocation would trigger the system-wide
# OOM killer, which happened once on 14 Nov 2026 (it killed only this process,
# but it could have chosen anything in the VM). RLIMIT_AS caps the process at
# 512 MiB, so without the cgroup it raises MemoryError instead.
import resource
import sys
import time

resource.setrlimit(resource.RLIMIT_AS, (512 << 20, 512 << 20))
time.sleep(3)
hog = []
try:
    while True:
        hog.append(bytearray(4 << 20))
except MemoryError:
    print("oom_victim: hit the 512 MiB safety cap; the cgroup limit was not applied", file=sys.stderr)
    sys.exit(3)
