#!/bin/bash
# Package install test: run inside a clean ubuntu:24.04 container with the .deb
# in the working directory (see docs/BUILDING.md and .github/workflows/release.yml).
# Runs inside a clean ubuntu:24.04 container.
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq ./hostmortem_*.deb systemd >/dev/null 2>&1
echo "== installed: $(dpkg-query -W -f='${Package} ${Version} ${Status}' hostmortem)"
echo "== user: $(getent passwd hostmortem)"
echo "== state dir: $(stat -c '%A %U:%G %n' /var/lib/hostmortem)"
echo "== conffiles: $(dpkg-query -W -f='${Conffiles}' hostmortem | tr -s ' ')"
echo "== versions: $(hmd --version) / $(hm --version)"
echo "== agent as the service user, 5 s:"
setpriv --reuid=hostmortem --regid=hostmortem --init-groups hmd --config /etc/hostmortem/hmd.conf --run-for 5s 2>&1 | tail -1
echo "== store: $(stat -c '%A %U:%G %s bytes' /var/lib/hostmortem/events.db)"
hm --db /var/lib/hostmortem/events.db status | head -3
echo "== systemd-analyze verify:"
systemd-analyze verify /usr/lib/systemd/system/hmd.service 2>&1 | grep -v "^$" || echo "(no findings)"
echo "== systemd-analyze security (offline):"
systemd-analyze security --offline=true /usr/lib/systemd/system/hmd.service 2>&1 | tail -3
echo "== edit conffile, then remove keeps it, purge removes everything:"
echo "# local edit" >> /etc/hostmortem/hmd.conf
apt-get remove -y -qq hostmortem >/dev/null 2>&1
echo "after remove: conf $(test -f /etc/hostmortem/hmd.conf && echo kept || echo gone), store $(test -d /var/lib/hostmortem && echo kept || echo gone)"
apt-get purge -y -qq hostmortem >/dev/null 2>&1
echo "after purge: conf $(test -f /etc/hostmortem/hmd.conf && echo kept || echo gone), store $(test -d /var/lib/hostmortem && echo kept || echo gone), user $(getent passwd hostmortem >/dev/null && echo kept || echo gone)"
