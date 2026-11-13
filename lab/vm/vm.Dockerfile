# Image for the hard-reset test: QEMU plus an Ubuntu kernel to boot inside it.
FROM hostmortem-dev:0
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      qemu-system-arm qemu-efi-aarch64 linux-image-generic busybox-static e2fsprogs cpio socat \
    && rm -rf /var/lib/apt/lists/*
