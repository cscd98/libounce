# SPDX-License-Identifier: ISC
#
# Builds libounce's pacman package in devkitPro's image. From the repository's
# top folder:
#   docker build -f packaging/Dockerfile --build-arg VERSION=20261117 --output out .
# writes out/libounce-20261117-1-any.pkg.tar.zst. release.sh builds it from a
# commit and publishes it.
FROM docker.io/devkitpro/devkita64:20260219 AS build

ARG VERSION
# The commit time, so that the package's file times do not depend on the
# build.
ARG SOURCE_DATE_EPOCH=0

# makepkg packages with fakeroot and zstd, and does not run as root. It builds
# in /build, which the debug information names.
RUN apt-get update && \
    apt-get install -y --no-install-recommends fakeroot zstd && \
    rm -rf /var/lib/apt/lists/*
RUN useradd --create-home builder && \
    mkdir /build /home/builder/out && chown builder /build /home/builder/out
COPY --chown=builder . /home/builder/libounce
USER builder
WORKDIR /home/builder/libounce/packaging
RUN test -n "$VERSION" && \
    LIBOUNCE_VERSION="$VERSION" SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    BUILDDIR=/build PKGDEST=/home/builder/out PACKAGER=ouncebrew dkp-makepkg --nodeps --noconfirm

FROM scratch
COPY --from=build /home/builder/out/ /
