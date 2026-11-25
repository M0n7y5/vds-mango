#!/usr/bin/env bash
# Lays out a built tree as the package's file system. Shared by build-arch.sh
# and the PKGBUILD so both produce the same package.
set -euo pipefail

if [ "$#" -ne 3 ]; then
	echo "usage: stage.sh REPO_ROOT BUILD_DIR DEST_ROOT" >&2
	exit 2
fi
repo_root=$1
build_dir=$2
dest=$3

DESTDIR="$dest" cmake --install "$build_dir"

install -Dm0644 "$build_dir/vdsd.service" \
	"${dest}/usr/lib/systemd/system/vdsd.service"
install -Dm0644 "${repo_root}/data/72-vds-dualsense-hide.rules" \
	"${dest}/usr/lib/udev/rules.d/72-vds-dualsense-hide.rules"
install -Dm0755 "${repo_root}/scripts/install-service.sh" \
	"${dest}/usr/share/vds-mango/install-service.sh"
install -Dm0755 "${repo_root}/scripts/uninstall-service.sh" \
	"${dest}/usr/share/vds-mango/uninstall-service.sh"
install -Dm0644 "${repo_root}/data/logrotate-vds.conf" \
	"${dest}/etc/logrotate.d/vds"

install -Dm0644 "${repo_root}/README.md" \
	"${dest}/usr/share/doc/vds-mango/README.md"
install -Dm0644 "${repo_root}/LICENSE" \
	"${dest}/usr/share/licenses/vds-mango/LICENSE"
