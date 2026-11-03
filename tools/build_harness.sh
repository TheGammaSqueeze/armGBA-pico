#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 TheGammaSqueeze
# Build the host harness (tools/harness.c) for qemu-arm: arm-linux-gnueabihf-gcc, static.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
arm-linux-gnueabihf-gcc -O1 -g -static -Wall -o out/harness tools/harness.c -lpthread
echo "out/harness: run with qemu-arm-static out/harness out/armgba-pico.blob <bios> <rom> [frames] [out.ppm]"
