#!/usr/bin/env bash
# Package PurrX r1 for Xiaomi curtana only.
# Run from the kernel source root after a successful GitHub Actions build.
set -euxo pipefail

IMAGE_DIR="out/arch/arm64/boot"
ZIP_NAME="PurrX-curtana-r1-KSU-SUSFS-${GITHUB_RUN_NUMBER}.zip"

# Refuse to package missing or empty build products.
test -s "$IMAGE_DIR/Image.gz"
test -s "$IMAGE_DIR/dtb.img"
test -s "$IMAGE_DIR/dtbo.img"
test -s "$IMAGE_DIR/dts/qcom/curtana-atoll-ab-idp-overlay.dtbo"
test -s kernel.release
test -s SHA256SUMS
test -s build-info.txt
test -s toolchain-info.txt

# The flashable package must identify curtana and must not allow another
# miatoll-family device. The generated DTBO image is retained unchanged for
# r1 compatibility; AnyKernel's device check is the installation safety gate.
grep -qx 'device.name1=curtana' AnyKernel3/anykernel.sh
if grep -Eq '^device\.name[2-9]=' AnyKernel3/anykernel.sh; then
  echo 'Refusing to package: non-curtana device entry found in AnyKernel3.' >&2
  exit 1
fi
if grep -Eq '^device\.name1=(excalibur|gram|joyeuse|miatoll)$' AnyKernel3/anykernel.sh; then
  echo 'Refusing to package: generic/non-curtana target found.' >&2
  exit 1
fi
grep -q 'kernel.string=PurrX curtana r1' AnyKernel3/anykernel.sh

rm -rf package artifacts
mkdir -p artifacts package

rsync -a \
  --exclude='.gitignore' \
  --exclude='README.md' \
  --exclude='placeholder' \
  AnyKernel3/ package/

cp "$IMAGE_DIR/Image.gz" package/Image.gz
cp "$IMAGE_DIR/dtb.img" package/dtb
cp "$IMAGE_DIR/dtbo.img" package/dtbo.img

# KernelSU, SUSFS and NoMount are built into the kernel. No external kernel
# modules or third-party performance modules are bundled in PurrX r1.
mkdir -p package/ramdisk package/patch

(
  cd package
  zip -r9 "../artifacts/$ZIP_NAME" .
)

# Publish the flashable ZIP, raw images and reproducibility metadata as one
# GitHub Actions artifact. r1 is not automatically published as a release.
cp kernel.release SHA256SUMS build-info.txt toolchain-info.txt artifacts/
cp "$IMAGE_DIR/Image.gz" "$IMAGE_DIR/dtb.img" "$IMAGE_DIR/dtbo.img" artifacts/
sha256sum "artifacts/$ZIP_NAME" >> artifacts/SHA256SUMS

# Final package checks.
test -s "artifacts/$ZIP_NAME"
unzip -t "artifacts/$ZIP_NAME"
unzip -l "artifacts/$ZIP_NAME"
grep -F "artifacts/$ZIP_NAME" artifacts/SHA256SUMS
