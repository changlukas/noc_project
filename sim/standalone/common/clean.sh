#!/usr/bin/env bash
# Clean generated standalone artifacts and the project-specific VCS cache.
set -euo pipefail
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
package_dir=$(cd "$script_dir/.." && pwd -P)
[[ -f "$package_dir/files.f" && -f "$script_dir/Makefile" ]] || {
    echo "Not a standalone simulation tree" >&2
    exit 1
}

# Fixed locations, independent of command-line run_dir or other Make overrides.
rm -rf -- "$package_dir/build"
cache_key=$(printf '%s' "$package_dir" | cksum | cut -d' ' -f1)
cache_root="/tmp/noc-vcs-$(id -u)-$cache_key"
if [[ -e "$cache_root" || -L "$cache_root" ]]; then
    [[ ! -L "$cache_root" && -O "$cache_root" ]] || {
        echo "Refusing unexpected cache owner or symlink: $cache_root" >&2
        exit 1
    }
    rm -rf -- "$cache_root"
fi
shopt -s nullglob
for directory in "$package_dir" "$script_dir"; do
    rm -rf -- "$directory"/csrc "$directory"/simv "$directory"/simv.* \
        "$directory"/DVEfiles "$directory"/AN.DB "$directory"/work.lib++ \
        "$directory"/*.daidir "$directory"/*.vdb "$directory"/*.fsdb \
        "$directory"/*.vpd "$directory"/*.vcd "$directory"/*.fst \
        "$directory"/*.log "$directory"/ucli.key "$directory"/vc_hdrs.h \
        "$directory"/novas.conf "$directory"/novas.rc \
        "$directory"/core "$directory"/core.[0-9]* \
        "$directory"/nmu-*-results.tar.gz "$directory"/master_wrap_*_dump*.txt
    for name in nWaveLog verdiLog VerdiLog; do
        log_dir="$directory/$name"
        if [[ -L "$log_dir" ]]; then
            # Remove the link itself, never clean a directory outside this tree.
            rm -f -- "$log_dir"
        elif [[ -d "$log_dir" ]]; then
            rm -f -- "$log_dir/novas.rc"
            # Preserve user waveform restore files and their backups.
            find "$log_dir" -depth -type f ! -name '*.rc' ! -name '*.rc.*' -delete
            find "$log_dir" -depth -type l -delete
            find "$log_dir" -depth -type d -empty -delete
        fi
    done
done
printf '%s\n' 'Cleaned build, waveforms, simulation reports and GUI artifacts; sources, patterns and signal RC files retained.'
