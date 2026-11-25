#!/bin/sh
# SPDX-License-Identifier: MIT
# Root half of the vds-mango setup wizard. pkexec starts it with the polkit
# action io.github.m0n7y5.vds-mango.setup.

set -eu

if [ "$#" -eq 0 ]; then
	echo "usage: setup-helper.sh start-service" >&2
	exit 2
fi

for action in "$@"; do
	case "$action" in
	start-service)
		# A unit that failed too often refuses to start until it is reset.
		systemctl reset-failed vdsd.service 2>/dev/null || true
		systemctl enable --now vdsd.service
		;;
	*)
		echo "unknown action: $action" >&2
		exit 2
		;;
	esac
done
