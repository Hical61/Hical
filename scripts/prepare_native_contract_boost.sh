#!/usr/bin/env bash
# 给宏轨和原生轨编译同一份 Boost.JSON。CI 固定源码版本和编译器。
set -euo pipefail

root=${1:?请指定 Boost 构建工作目录}
cxx=${HICAL_CXX_NATIVE:-/usr/bin/g++-16}
jobs=${HICAL_CONTRACT_JOBS:-2}
mkdir -p "$root"
root=$(cd -- "$root" && pwd -P)
archive="$root/boost_1_90_0.tar.bz2"
sha=49551aff3b22cbc5c5a9ed3dbc92f0e23ea50a0f7325b0d198b705e8ee3fc305
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 2 --connect-timeout 15 --max-time 600 \
        --silent --show-error --proto '=https' --proto-redir '=https' \
        https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2 -o "$archive.part"
    printf '%s  %s\n' "$sha" "$archive.part" | sha256sum --check
    mv -- "$archive.part" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check
sha256sum "$archive"
tar -xjf "$archive" -C "$root" --no-same-owner
cd "$root/boost_1_90_0"
CXX="$cxx" ./bootstrap.sh --with-toolset=gcc --with-libraries=json --prefix="$root/prefix"
printf 'using gcc : 16 : "%s" ;\n' "$cxx" > "$root/user-config.jam"
./b2 -d0 "--user-config=$root/user-config.jam" toolset=gcc-16 cxxstd=20 \
    link=static runtime-link=shared threading=multi variant=release --layout=system -j"$jobs" install
