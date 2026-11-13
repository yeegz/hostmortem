# Security policy

Hostmortem runs with Linux capabilities on edge devices, so security reports
matter. Please report vulnerabilities privately through GitHub's
"Report a vulnerability" (Security Advisories) on this repository rather than
in a public issue: https://github.com/yeegz/hostmortem/security/advisories/new.
Expect an acknowledgement within 7 days.

Supported versions: the latest release (currently 0.1.x).

In scope: the agent (`hmd`), the CLI (`hm`), parsers, the systemd unit and the
generated HTML reports. Out of scope: the lab scripts (they deliberately need
a privileged throwaway container).

See `docs/THREAT_MODEL.md` for assumptions and known gaps.
