#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 RECOIL_SOURCE BUILD_DIRECTORY" >&2
  exit 2
fi

source_dir=$(realpath "$1")
build_dir=$(realpath -m "$2")
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${script_dir}/.." && pwd)
patch_file="${repo_dir}/engine-patches/barc-01.5/recoil-rally-api.patch"
compat_file="${repo_dir}/engine-patches/barc-01.5/gcc16-rmlui.cmake"
base_commit=2639eedac7d1fd67d793ec93ebd27f014f336a14
cmake_bin=${BARC_CMAKE_BIN:-/tmp/barc-native-assets/pydeps/cmake/data/bin/cmake}
sysroot=${BARC_SYSROOT:-/tmp/barc-native-assets/sysroot/usr}

if [[ $(git -C "${source_dir}" rev-parse HEAD) != "${base_commit}" ]]; then
  echo "Recoil source must be checked out at ${base_commit}" >&2
  exit 1
fi
if [[ -n $(git -C "${source_dir}" status --porcelain --untracked-files=no --ignore-submodules=all) ]]; then
  echo "Recoil source has tracked changes" >&2
  exit 1
fi

git -C "${source_dir}" apply --check "${patch_file}"
git -C "${source_dir}" apply "${patch_file}"
git -C "${source_dir}" submodule update --init --recursive

"${cmake_bin}" -S "${source_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAI_TYPES=NATIVE \
  -DBUILD_spring-headless=ON \
  -DBUILD_spring-dedicated=ON \
  -DBUILD_spring-legacy=ON \
  -DCMAKE_PREFIX_PATH="${sysroot}" \
  -DSDL2_DIR="${sysroot}/lib/cmake/SDL2" \
  -DIL_INCLUDE_DIR="${sysroot}/include/IL" \
  -DIL_LIBRARIES="${sysroot}/lib/libIL.so" \
  -DOPENAL_INCLUDE_DIR="${sysroot}/include/AL" \
  -DOPENAL_LIBRARY="${sysroot}/lib/libopenal.so" \
  -DSEVENZIP_BIN="${sysroot}/lib/7zip/7z" \
  -DCMAKE_PROJECT_INCLUDE="${compat_file}" \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=gold \
  -DVERSION_ADDITIONAL=BARC-01.5-rally-api-v1

ninja -C "${build_dir}" -j"${BARC_BUILD_JOBS:-2}" \
  spring-headless C-AIInterface Cpp-AIWrapper test_RallyQueueCallback
"${build_dir}/test/test_RallyQueueCallback"
