# Build environment for the Amiga/Warp kernel and Buildroot rootfs.
# Used by ../build.sh; the m68k cross toolchain itself is built by Buildroot.
FROM debian:bookworm

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        bash bc binutils bison build-essential bzip2 ca-certificates cpio curl \
        diffutils file findutils flex g++ gawk git gzip kmod libelf-dev \
        libncurses-dev libssl-dev locales make patch perl pkg-config python3 \
        rsync sed tar unzip wget xz-utils zstd \
        dropbear-bin e2fsprogs fakeroot python3-venv util-linux \
    && rm -rf /var/lib/apt/lists/*

# amitools (rdbtool, xdftool) for the CF card image: Amiga RDB + FFS
RUN python3 -m venv /opt/amitools \
    && /opt/amitools/bin/pip install --no-cache-dir amitools==0.8.1
ENV PATH=/opt/amitools/bin:$PATH

ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

# Buildroot (and some of its packages) refuse to build as root.
# /work is backed by a named volume; Docker seeds it with this ownership.
RUN useradd -m -u 1000 -s /bin/bash builder \
    && mkdir -p /work && chown builder:builder /work
USER builder
WORKDIR /work
