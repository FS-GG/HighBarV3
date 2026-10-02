#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 RECOIL_SOURCE HIGHBAR_SOURCE BUILD_DIRECTORY" >&2
  exit 2
fi

recoil_source=$(realpath "$1")
highbar_source=$(realpath "$2")
build_dir=$(realpath -m "$3")
expected_recoil_commit=de69361239d8c8b1012dba3f5aa3122954ea4da3
expected_recoil_tree=081bfe95b4e76e3212afa5805ab5a82e474c0b47
expected_contract_sha=ac0c94390a1b5da600e49e2d00d9a36d4782964a33603ddbc2c4bafbd3db8c28
cmake_bin=${BARC_CMAKE_BIN:-cmake}
sysroot=${BARC_SYSROOT:-/tmp/barc-native-assets/sysroot/usr}
jobs=${BARC_BUILD_JOBS:-4}

if (( jobs < 1 || jobs > 8 )); then
  echo "BARC_BUILD_JOBS must be between 1 and 8" >&2
  exit 2
fi
if [[ $(git -C "${recoil_source}" rev-parse HEAD) != "${expected_recoil_commit}" \
   || $(git -C "${recoil_source}" rev-parse HEAD^{tree}) != "${expected_recoil_tree}" ]]; then
  echo "Recoil source is not the admitted stock 2026.07.04 commit/tree" >&2
  exit 1
fi
if [[ -n $(git -C "${recoil_source}" status --porcelain --untracked-files=no --ignore-submodules=all) ]]; then
  echo "Recoil source has tracked changes" >&2
  exit 1
fi
if [[ $(realpath "${recoil_source}/AI/Skirmish/BARb") != "${highbar_source}" ]]; then
  echo "AI/Skirmish/BARb must resolve to the supplied HighBar source" >&2
  exit 1
fi
contract_sha=$(sha256sum "${highbar_source}/contracts/barc-stock-queue-v1/contract.json" | cut -d' ' -f1)
if [[ ${contract_sha} != "${expected_contract_sha}" ]]; then
  echo "HighBar stock contract does not match the independently accepted bytes" >&2
  exit 1
fi

"${cmake_bin}" -S "${recoil_source}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAI_TYPES=NATIVE \
  -DBUILD_spring-headless=OFF \
  -DBUILD_spring-dedicated=OFF \
  -DBUILD_spring-legacy=OFF \
  -DCMAKE_PREFIX_PATH="${sysroot}" \
  -DSDL2_DIR="${sysroot}/lib/x86_64-linux-gnu/cmake/SDL2" \
  -DIL_INCLUDE_DIR="${sysroot}/include/IL" \
  -DIL_LIBRARIES="${sysroot}/lib/x86_64-linux-gnu/libIL.so" \
  -DOPENAL_INCLUDE_DIR="${sysroot}/include" \
  -DOPENAL_LIBRARY="${sysroot}/lib/x86_64-linux-gnu/libopenal.so" \
  -DCMAKE_PROJECT_INCLUDE="${highbar_source}/engine-patches/barc-01.5/gcc16-rmlui.cmake" \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=gold \
  -DHIGHBAR_STOCK_RECOIL=ON \
  -DHIGHBAR_BUILD_TESTS=ON

ninja -C "${build_dir}" -j"${jobs}" \
  BARb live_control_state_test stock_queue_reader_test tactical_native_state_test
ctest --test-dir "${build_dir}" --output-on-failure \
  -R '^(live_control_state_test|stock_queue_reader_test|tactical_native_state_test)$'
