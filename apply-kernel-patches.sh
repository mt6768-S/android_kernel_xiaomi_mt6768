#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'

usage() {
	cat <<'EOF'
Usage: apply-kernel-patches.sh [options]

Options:
  --config FILE       Kernel .config to update.
  --patch-dir DIR     Directory containing .patch/.diff files.
  --dry-run           Check patches and show config changes without writing.
  -h, --help          Show this help.

Environment:
  KERNEL_CONFIG       Default config path when --config is omitted.
  PATCH_DIR           Default patch directory when --patch-dir is omitted.

When no config is specified, the script uses .config in the kernel source
tree, or out/.config when the source-tree .config does not exist.
EOF
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$repo_root" ]]; then
	repo_root="$script_dir"
fi

patch_dir="${PATCH_DIR:-$repo_root/patches}"
config_file="${KERNEL_CONFIG:-}"
dry_run=0

while (($#)); do
	case "$1" in
		--config)
			(($# >= 2)) || die "--config requires a file path"
			config_file=$2
			shift 2
			;;
		--patch-dir)
			(($# >= 2)) || die "--patch-dir requires a directory path"
			patch_dir=$2
			shift 2
			;;
		--dry-run)
			dry_run=1
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			die "unknown option: $1 (use --help for usage)"
			;;
	esac
done

if [[ "$patch_dir" != /* ]]; then
	patch_dir="$repo_root/$patch_dir"
fi

if [[ -z "$config_file" ]]; then
	if [[ -f "$repo_root/.config" ]]; then
		config_file="$repo_root/.config"
	elif [[ -f "${KBUILD_OUTPUT:-${O:-$repo_root/out}}/.config" ]]; then
		config_file="${KBUILD_OUTPUT:-${O:-$repo_root/out}}/.config"
	else
		die "could not find .config; pass --config PATH or set KERNEL_CONFIG"
	fi
elif [[ "$config_file" != /* ]]; then
	config_file="$repo_root/$config_file"
fi

[[ -d "$patch_dir" ]] || die "patch directory does not exist: $patch_dir"
[[ -f "$config_file" ]] || die "config file does not exist: $config_file"

mapfile -t patch_files < <(
	find "$patch_dir" -maxdepth 1 -type f \
		\( -name '*.patch' -o -name '*.diff' \) -print | sort
)
((${#patch_files[@]} > 0)) || die "no .patch or .diff files found in $patch_dir"

cd -- "$repo_root"

for patch_file in "${patch_files[@]}"; do
	patch_name="${patch_file#"$repo_root"/}"
	if git apply --check -- "$patch_file" >/dev/null 2>&1; then
		if ((dry_run)); then
			printf 'would apply %s\n' "$patch_name"
		else
			git apply -- "$patch_file"
			printf 'applied %s\n' "$patch_name"
		fi
	elif git apply --reverse --check -- "$patch_file" >/dev/null 2>&1; then
		printf 'already applied %s\n' "$patch_name"
	else
		die "cannot apply or reverse-check patch: $patch_name"
	fi
done

rewrite_config() {
	awk '
		/^# BEGIN KERNELSU PATCH CONFIG$/ {
			in_managed_block = 1
			next
		}
		/^# END KERNELSU PATCH CONFIG$/ {
			in_managed_block = 0
			next
		}
		in_managed_block {
			next
		}
		/^CONFIG_KSU=/ ||
		/^# CONFIG_KSU is not set$/ ||
		/^CONFIG_KSU_HACK_ARM64_BRANCH_LINK=/ ||
		/^# CONFIG_KSU_HACK_ARM64_BRANCH_LINK is not set$/ ||
		/^CONFIG_NOMOUNT=/ ||
		/^# CONFIG_NOMOUNT is not set$/ ||
		/^CONFIG_KSU_SUSFS=/ ||
		/^# CONFIG_KSU_SUSFS is not set$/ {
			next
		}
		{ lines[++count] = $0 }
		END {
			while (count > 0 && lines[count] == "")
				count--
			for (i = 1; i <= count; i++)
				print lines[i]
			print ""
			print "# BEGIN KERNELSU PATCH CONFIG"
			print "CONFIG_KSU=y"
			print "CONFIG_KSU_HACK_ARM64_BRANCH_LINK=y"
			print ""
			print "CONFIG_NOMOUNT=y"
			print ""
			print "CONFIG_KSU_SUSFS=y"
			print "# END KERNELSU PATCH CONFIG"
		}
	' "$1"
}

new_config="$(mktemp "${config_file}.XXXXXX")"
cleanup() {
	rm -f -- "$new_config"
}
trap cleanup EXIT

rewrite_config "$config_file" >"$new_config"

if ((dry_run)); then
	printf '%s\n' "config changes for ${config_file#"$repo_root"/}:"
	diff -u -- "$config_file" "$new_config" || true
else
	chmod --reference="$config_file" "$new_config"
	mv -- "$new_config" "$config_file"
	printf 'updated %s\n' "${config_file#"$repo_root"/}"
fi
