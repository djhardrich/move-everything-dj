# DJ Deck Module Build Environment
# Copyright (c) 2026 DJ Hard Rich
# Licensed under CC BY-NC-SA 4.0
#
# Targets: Ableton Move (aarch64 Linux)
# Bungee requires C++20, so we use Debian Bookworm with GCC 12.

FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    make \
    file \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Set cross-compilation environment
ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV CC=aarch64-linux-gnu-gcc
ENV CXX=aarch64-linux-gnu-g++
