#!/usr/bin/env bash
set -euo pipefail

root="$(realpath -m "${1:-$HOME/hical-contract-oatpp-1.3.1}")"
prefix="$root/install"
mkdir -p -- "$(dirname -- "$root")"
if ! mkdir -- "$root"; then
    printf '没能创建依赖目录 %s，请换一个新目录再试。\n' "$root" >&2
    exit 1
fi
mkdir -- "$root/downloads" "$root/source"

fetch_source() {
    local module="$1" commit="$2" sha256="$3"
    local archive="$root/downloads/$module-${commit:0:7}.tar.gz"
    local source="$root/source/$module"
    curl --fail --location --retry 2 --connect-timeout 15 --max-time 180 \
        --silent --show-error --proto '=https' --proto-redir '=https' \
        "https://github.com/oatpp/$module/archive/$commit.tar.gz" -o "$archive.part"
    printf '%s  %s\n' "$sha256" "$archive.part" | sha256sum --check
    mv "$archive.part" "$archive"
    mkdir -- "$source"
    tar -xzf "$archive" --no-same-owner --strip-components=1 -C "$source"
}

fetch_source oatpp 17ef2a7f6c8a932498799b2a5ae5aab2869975c7 \
    633787f10ea5976d41f2b83ad920829ea1f3bd4cb6d5187b2a53ec891e78cde0
fetch_source oatpp-swagger ed5251c580e2e98beb50d818bcea8ddc91419d8c \
    afe026d6ba10f5cf43e65578acb80bb82f189d311ed6a0d7c4b8f4bd2b07e55f

common_args=(
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-16
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "-DCMAKE_INSTALL_PREFIX=$prefix"
    -DBUILD_SHARED_LIBS=OFF -DOATPP_BUILD_TESTS=OFF -DOATPP_INSTALL=ON
)
cmake -S "$root/source/oatpp" -B "$root/build-oatpp" "${common_args[@]}" -DOATPP_LINK_TEST_LIBRARY=ON
cmake --build "$root/build-oatpp" --parallel 2
cmake --install "$root/build-oatpp"

cmake -S "$root/source/oatpp-swagger" -B "$root/build-oatpp-swagger" "${common_args[@]}" \
    -DOATPP_MODULES_LOCATION=INSTALLED "-DCMAKE_PREFIX_PATH=$prefix"
cmake --build "$root/build-oatpp-swagger" --parallel 2
cmake --install "$root/build-oatpp-swagger"
