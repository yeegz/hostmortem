# Quickstart: from install to your first incident report

About five minutes on Ubuntu 22.04 or 24.04, Debian 12 or 13, or Raspberry Pi
OS (bookworm, 64-bit), on amd64 or arm64. You need sudo.

## 1. Install (one command)

```bash
curl -fsSL https://github.com/yeegz/hostmortem/releases/latest/download/install.sh | sudo sh
```

The script downloads the `.deb` for your architecture, checks it against the
release's `SHA256SUMS` (and stops on a mismatch), installs it with apt, starts
the `hmd` service, and adds you to the `hostmortem` group. Read it first if you
prefer: it is short, and it is attached to every
[release](https://github.com/yeegz/hostmortem/releases).

Manual install, same result:

```bash
v=0.2.0; arch=$(dpkg --print-architecture)
curl -fsSLO "https://github.com/yeegz/hostmortem/releases/download/v$v/hostmortem_${v}_${arch}.deb"
curl -fsSLO "https://github.com/yeegz/hostmortem/releases/download/v$v/SHA256SUMS"
sha256sum --check --ignore-missing SHA256SUMS
sudo apt install "./hostmortem_${v}_${arch}.deb"
sudo systemctl enable --now hmd
sudo usermod -aG hostmortem "$USER"
```

Group membership applies at your next login; until then, prefix `hm` with `sudo`.

## 2. Check what it can see

```bash
sudo hm doctor
```

Each `[missing]` line says what that gap costs (for example, without a
readable kernel log an OOM kill can never be ruled out). Diagnoses list the
same gaps under "Could not see".

## 3. Record a first incident

```bash
sudo hm drill
```

The drill starts a harmless process called `hm-drill` and kills it with
SIGKILL. `hmd` always watches that name, so it records the exit exactly as it
would record one of your services dying, and opens an incident. Takes about
ten seconds.

## 4. Read it

```bash
sudo hm incidents          # list
sudo hm explain 1          # diagnosis: facts, hypotheses, what it could not see
sudo hm report             # everything as one HTML file in the current directory
```

`hm report` writes `hostmortem-report-<time>.html`. Copy it to your laptop
(`scp`) and open it in any browser; it makes no network requests. To send it to
someone else, add `--redact` to replace IP and MAC addresses.

For the drill, expect the conclusion "unknown" with "process exited" as a
*possible* hypothesis: without eBPF (planned for v0.3), a crash, a `kill -9`
and a clean stop look the same, and Hostmortem says so instead of guessing.

## 5. Point it at what you care about

Edit `/etc/hostmortem/hmd.conf`, then `sudo systemctl restart hmd`:

```ini
[watch]
processes = nginx, mosquitto     # comm names whose exits and restarts you want recorded

[probe]
dns_names = example.com          # names to resolve every 10 s
tcp_targets = 192.0.2.10:443     # IP:port to connect to
```

Then leave it running. The next time the box loses DNS, a link or a service,
`sudo hm incidents` shows what happened first, across the reboot if there was one.

## Upgrade, remove

- Upgrade: run the install command again. The evidence store is kept and
  migrated, and the running agent is restarted on the new version.
- Remove, keep the evidence: `sudo apt remove hostmortem`
- Remove everything (store and service user): `sudo apt purge hostmortem`

Defaults: evidence in `/var/lib/hostmortem`, at most 14 days or 256 MB; the
agent runs as the `hostmortem` user with two capabilities and no network listener.
