#!/usr/bin/env bash
# Lays out a built tree as the package's file system. Shared by build-arch.sh
# and the PKGBUILD so both produce the same package.
set -euo pipefail

if [ "$#" -ne 4 ]; then
	echo "usage: stage.sh REPO_ROOT BUILD_DIR DEST_ROOT DKMS_VERSION" >&2
	exit 2
fi
repo_root=$1
build_dir=$2
dest=$3
dkms_version=$4
dkms_source_dir="${dest}/usr/src/vds_hcd-${dkms_version}"

DESTDIR="$dest" cmake --install "$build_dir"

install -Dm0644 "$build_dir/vdsd.service" \
	"${dest}/usr/lib/systemd/system/vdsd.service"
install -Dm0644 "${repo_root}/data/99-vds-dualsense-udev.rules" \
	"${dest}/usr/lib/udev/rules.d/99-vds-dualsense-udev.rules"
install -Dm0644 "${repo_root}/data/72-vds-dualsense-hide.rules" \
	"${dest}/usr/lib/udev/rules.d/72-vds-dualsense-hide.rules"
install -Dm0755 "${repo_root}/scripts/install-service.sh" \
	"${dest}/usr/share/vds-mango/install-service.sh"
install -Dm0755 "${repo_root}/scripts/uninstall-service.sh" \
	"${dest}/usr/share/vds-mango/uninstall-service.sh"
install -Dm0644 "${repo_root}/data/logrotate-vds.conf" \
	"${dest}/etc/logrotate.d/vds"

install -d "$dkms_source_dir/include/uapi" "$dkms_source_dir/include/vds"
install -m0644 "${repo_root}/module/Makefile" "${repo_root}/module/Kbuild" \
	"$dkms_source_dir/"
for source in "${repo_root}/module/"*.c "${repo_root}/module/"*.h; do
	case "$(basename "$source")" in
	*.mod.c)
		continue
		;;
	esac
	install -m0644 "$source" "$dkms_source_dir/"
done
sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${dkms_version}\"/" \
	"${repo_root}/module/dkms.conf" >"${dkms_source_dir}/dkms.conf"
install -m0644 "${repo_root}/include/uapi/"*.h \
	"${dkms_source_dir}/include/uapi/"
install -m0644 "${repo_root}/include/vds/"*.h \
	"${dkms_source_dir}/include/vds/"

install -Dm0644 "${repo_root}/README.md" \
	"${dest}/usr/share/doc/vds-mango/README.md"
install -Dm0644 "${repo_root}/LICENSE" \
	"${dest}/usr/share/licenses/vds-mango/LICENSE"
