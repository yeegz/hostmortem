**What and why**

**How it was tested**
- [ ] `ctest --preset debug` (and `asan`/`tsan` presets for concurrency or parser changes)
- [ ] New or changed playbook: a scenario fixture in `lab/gen_fixtures.py`, and a lab scenario where possible
- [ ] No compiler warnings (`-Werror -Wconversion`)

**Evidence and honesty**
- [ ] Any number in docs comes with the script that produced it
- [ ] New collectors report `collector.status` with a reason when they cannot see something
