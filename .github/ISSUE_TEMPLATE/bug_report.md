---
name: Bug report
about: Something Hostmortem did wrong, including a wrong or overconfident diagnosis
labels: bug
---

**What happened**

**What you expected**

**Environment**
- Distribution and version:
- Kernel (`uname -r`):
- Hostmortem version (`hm --version`):
- Output of `hm doctor`:

**Evidence**
If it is about a diagnosis, attach `hm explain <id> --json` (or `hm export --redact`
for the window). `--redact` masks IP addresses; DNS names are already hashed.
