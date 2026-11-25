#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'USAGE'
Usage: packaging/ArchLinux/build-arch.sh [options]

Options:
  --pkgrel REL          Override the Arch package release. Defaults to 1.
  --output-dir DIR      Directory for the generated package.
  --build-dir DIR       Temporary build/staging directory.
  --jobs N              Parallel build jobs. Defaults to nproc.
  -h, --help            Show this help text.
USAGE
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/../.." && pwd)
package_name="vds-mango"
build_root="${repo_root}/build/packaging/ArchLinux"
output_dir="${repo_root}/dist/ArchLinux"
pkgrel="1"
jobs=$(nproc)

while [ "$#" -gt 0 ]; do
	case "$1" in
	--pkgrel)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --pkgrel" >&2
			exit 2
		fi
		pkgrel=$2
		shift 2
		;;
	--output-dir)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --output-dir" >&2
			exit 2
		fi
		output_dir=$2
		shift 2
		;;
	--build-dir)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --build-dir" >&2
			exit 2
		fi
		build_root=$2
		shift 2
		;;
	--jobs)
		if [ "$#" -lt 2 ]; then
			echo "missing value for --jobs" >&2
			exit 2
		fi
		jobs=$2
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

require_tool() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "required tool not found: $1" >&2
		exit 1
	fi
}

substitute_template() {
	local input=$1
	local output=$2

	sed \
		-e "s|@PKGVER@|${pkgver}|g" \
		-e "s|@PKGREL@|${pkgrel}|g" \
		-e "s|@ARCH@|${arch}|g" \
		-e "s|@SIZE@|${installed_size}|g" \
		-e "s|@BUILDDATE@|${build_date}|g" \
		-e "s|@BUILDROOT@|${build_root}|g" \
		-e "s|@REPOROOT@|${repo_root}|g" \
		"$input" >"$output"
}

require_tool bsdtar
require_tool cmake
require_tool gzip
require_tool install
require_tool make
require_tool mktemp
require_tool nproc
require_tool sed
require_tool zstd

arch=$(uname -m)
version_args=(--internal arch)
package_args=(--package arch --name "$package_name" --pkgrel "$pkgrel" --arch "$arch")
pkgver=$("${repo_root}/scripts/generate-version.sh" "${version_args[@]}")
build_date=$(date +%s)
build_dir="${build_root}/cmake"
package_root="${build_root}/${package_name}-${pkgver}-${pkgrel}-${arch}"
output_dir=$(realpath -m "$output_dir")
package_path="${output_dir}/$("${repo_root}/scripts/generate-version.sh" "${package_args[@]}")"

rm -rf "$build_dir" "$package_root"
mkdir -p "$build_dir" "$package_root" "$output_dir"

cmake -S "$repo_root" -B "$build_dir" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DINSTALL_SERVICE=OFF
cmake --build "$build_dir" --parallel "$jobs"
"${script_dir}/stage.sh" "$repo_root" "$build_dir" "$package_root"

installed_size=$(du -sb "$package_root" | awk '{print $1}')
substitute_template "${script_dir}/PKGINFO.in" "${package_root}/.PKGINFO"
substitute_template "${script_dir}/BUILDINFO.in" "${package_root}/.BUILDINFO"
install -m0644 "${script_dir}/vds-mango.install" "${package_root}/.INSTALL"

(
	cd "$package_root"
	mtree_tmp=$(mktemp)
	trap 'rm -f "$mtree_tmp"' EXIT
	bsdtar --format=mtree \
		--uid 0 --gid 0 --uname root --gname root \
		--options='!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link' \
		-cf - \
		--exclude .BUILDINFO \
		--exclude .INSTALL \
		--exclude .MTREE \
		--exclude .PKGINFO \
		. | gzip -c >"$mtree_tmp"
	mv "$mtree_tmp" .MTREE
	trap - EXIT
		bsdtar --uid 0 --gid 0 --uname root --gname root \
			-cf - .PKGINFO .BUILDINFO .INSTALL .MTREE etc usr |
			zstd -T0 -q -c >"$package_path"
	)

echo "$package_path"
