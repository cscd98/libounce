#!/bin/sh
# SPDX-License-Identifier: ISC
#
# Builds libounce's pacman package from a commit and publishes it as a GitHub
# release of this repository:
#   sh packaging/release.sh VERSION [COMMIT]
# VERSION is the release and package version: the date, as 20261117, and
# 20261117.1 for a second release on the same day (pacman orders 20261117 <
# 20261117.1). COMMIT defaults to HEAD; it must be on the GitHub repository
# already. The package and its SHA-256 are written to packaging/release/VERSION/.
#
# Environment:
#   CONTAINER   podman or docker (default: podman when it is installed)
#   NO_UPLOAD=1 build the package only
set -eu

version="${1:?usage: release.sh VERSION [COMMIT]}"
commit=$(git rev-parse --verify "${2:-HEAD}^{commit}")
case "$version" in
    *[!0-9.]* | .* | *.)
        echo "release.sh: VERSION is digits and dots, as 20261117" >&2
        exit 1 ;;
esac

top=$(git rev-parse --show-toplevel)
out="$top/packaging/release/$version"
container="${CONTAINER:-$(command -v podman > /dev/null && echo podman || echo docker)}"
package="libounce-$version-1-any.pkg.tar.zst"

# The build sees the commit's files only, not the working tree.
context=$(mktemp -d)
trap 'rm -rf "$context"' EXIT
git -C "$top" archive "$commit" | tar -x -C "$context"

rm -rf "$out"
mkdir -p "$out"
"$container" build -f "$context/packaging/Dockerfile" \
    --build-arg VERSION="$version" \
    --build-arg SOURCE_DATE_EPOCH="$(git show -s --format=%ct "$commit")" \
    --output "type=local,dest=$out" "$context"
(cd "$out" && sha256sum "$package" > "$package.sha256")
cat "$out/$package.sha256"

if [ "${NO_UPLOAD:-0}" = 1 ]; then
    exit 0
fi
gh release create "$version" --target "$commit" --title "libounce $version" \
    --notes "Install with devkitPro's pacman: \`dkp-pacman -U $package\`." \
    "$out/$package" "$out/$package.sha256"
