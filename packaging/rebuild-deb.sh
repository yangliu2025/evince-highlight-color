#!/usr/bin/env bash
#
# Rebuild the Ubuntu evince packages with the highlight-colour patch applied.
#
# Run this whenever Ubuntu ships a new evince: the archive version supersedes
# the local "+hl" one, so an ordinary `apt upgrade` silently removes the
# feature. Rebuilding restores it on top of whatever the archive now has,
# including any security patches Ubuntu carries over upstream.
#
# Requires: deb-src enabled, plus
#   sudo apt install devscripts quilt fakeroot && sudo apt build-dep evince

set -euo pipefail

# Commit the packaging diff is taken against; move this when rebasing onto a
# newer upstream tarball.
BASE_REF="upstream/46.3.1"
PATCH_NAME="highlight-annotation-colors.patch"
PATCH_PATHS=(backend libview shell)
NEW_SYMBOLS=(ev_view_add_annotation ev_view_get_selected_annotation ev_view_set_annotation_color)

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="${1:-$PWD/evince-deb-build}"

for tool in dpkg-buildpackage dch quilt; do
	command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done
if [[ -z $(apt-cache showsrc evince 2>/dev/null) ]]; then
	echo "no deb-src for evince; add a deb-src entry for noble main under" >&2
	echo "/etc/apt/sources.list.d/ and run apt update" >&2
	exit 1
fi
git -C "$REPO" rev-parse --verify --quiet "$BASE_REF" >/dev/null ||
	{ echo "no such ref in $REPO: $BASE_REF" >&2; exit 1; }

mkdir -p "$WORKDIR"
cd "$WORKDIR"
apt-get source evince
cd "$(find . -maxdepth 1 -type d -name 'evince-*' | sort -V | tail -1)"

# dch --local always restarts from the archive version, which collides with an
# already-installed +hlN and makes apt report "already the newest version".
ARCHIVE_VERSION="$(dpkg-parsechangelog -S Version)"
INSTALLED="$(dpkg-query -W -f='${Version}' evince 2>/dev/null || true)"
suffix=1
if [[ $INSTALLED == "$ARCHIVE_VERSION"+hl* ]]; then
	suffix=$(( ${INSTALLED##*+hl} + 1 ))
fi
LOCAL_VERSION="${ARCHIVE_VERSION}+hl${suffix}"

git -C "$REPO" diff "$BASE_REF" HEAD -- "${PATCH_PATHS[@]}" > "debian/patches/$PATCH_NAME"
grep -qxF "$PATCH_NAME" debian/patches/series || echo "$PATCH_NAME" >> debian/patches/series

# dpkg-gensymbols only fails on *removed* symbols, but keep the file honest.
symbols=debian/libevview3-3t64.symbols
if [[ -f $symbols ]]; then
	for sym in "${NEW_SYMBOLS[@]}"; do
		grep -q "$sym" "$symbols" ||
			sed -i "/^ ev_view_set_enable_spellchecking@Base/i\\ $sym@Base $LOCAL_VERSION" "$symbols"
	done
fi

QUILT_PATCHES=debian/patches quilt push -a
QUILT_PATCHES=debian/patches quilt pop -a

DEBEMAIL="${DEBEMAIL:-$(git -C "$REPO" config user.email)}" \
DEBFULLNAME="${DEBFULLNAME:-$(git -C "$REPO" config user.name)}" \
	dch --newversion "$LOCAL_VERSION" --distribution "$(lsb_release -cs)" \
	    "Local build: highlight colour picker in the view popup menu."

DEB_BUILD_OPTIONS="parallel=$(nproc)" dpkg-buildpackage -b -uc -us

cat <<EOF

Built in $(cd .. && pwd). Install with:

  cd $(cd .. && pwd)
  sudo apt install ./evince_*_amd64.deb ./evince-common_*_all.deb ./libevdocument3-4t64_*_amd64.deb ./libevview3-3t64_*_amd64.deb
EOF
