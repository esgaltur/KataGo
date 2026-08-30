#!/usr/bin/env bash
# Build the versioned KataGo C ABI as a Linux shared library.
#
# From Windows/WSL:
#   wsl -d Ubuntu -- bash /mnt/c/path/to/KataGo/cpp/build_shared_linux.sh OPENCL
#
# Optional environment variables:
#   KATAGO_BUILD_DIR       output directory (default: cpp/build_linux_<backend>)
#   KATAGO_BUILD_JOBS      parallel jobs (default: nproc)
#   KATAGO_DEP_PREFIX      unpacked dependency prefix ending in /usr
#   KATAGO_OPENCL_LIBRARY  explicit libOpenCL.so path
#   KATAGO_USE_AVX2        1 or 0 (default: 1 on x86-64, 0 elsewhere)
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
backend="${1:-OPENCL}"
backend="${backend^^}"

case "${backend}" in
  OPENCL|EIGEN|CUDA|TENSORRT) ;;
  *)
    echo "Unsupported backend '${backend}'. Use OPENCL, EIGEN, CUDA, or TENSORRT." >&2
    exit 2
    ;;
esac

backend_lower="${backend,,}"
build_dir="${KATAGO_BUILD_DIR:-${script_dir}/build_linux_${backend_lower}}"
jobs="${KATAGO_BUILD_JOBS:-$(nproc)}"

if [[ -n "${KATAGO_USE_AVX2:-}" ]]; then
  use_avx2="${KATAGO_USE_AVX2}"
else
  case "$(uname -m)" in
    x86_64|amd64) use_avx2=1 ;;
    *) use_avx2=0 ;;
  esac
fi

if [[ "${use_avx2}" != "0" && "${use_avx2}" != "1" ]]; then
  echo "KATAGO_USE_AVX2 must be 0 or 1, got '${use_avx2}'." >&2
  exit 2
fi

cmake_args=(
  -S "${script_dir}"
  -B "${build_dir}"
  -DBUILD_AS_DLL=1
  -DBUILD_DLL_SMOKE=1
  -DNO_GIT_REVISION=1
  -DUSE_BACKEND="${backend}"
  -DUSE_AVX2="${use_avx2}"
  -DCMAKE_BUILD_TYPE=Release
)

if [[ -n "${KATAGO_DEP_PREFIX:-}" ]]; then
  dep_prefix="${KATAGO_DEP_PREFIX%/}"
  if [[ "${backend}" == "OPENCL" ]]; then
    opencl_library="${KATAGO_OPENCL_LIBRARY:-${dep_prefix}/lib/x86_64-linux-gnu/libOpenCL.so}"
    cmake_args+=(
      -DOpenCL_INCLUDE_DIR="${dep_prefix}/include"
      -DOpenCL_LIBRARY="${opencl_library}"
    )
  elif [[ "${backend}" == "EIGEN" ]]; then
    cmake_args+=(-DEIGEN3_INCLUDE_DIRS="${dep_prefix}/include/eigen3")
  fi
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --parallel "${jobs}"

library="${build_dir}/libkatago.so"
smoke="${build_dir}/katago_dll_smoke"
c_abi_smoke="${build_dir}/katago_dll_c_abi_smoke"
if [[ ! -f "${library}" || ! -f "${smoke}" || ! -f "${c_abi_smoke}" ]]; then
  echo "Expected build outputs were not produced." >&2
  exit 1
fi

"${c_abi_smoke}"

export_count="$(nm -D --defined-only "${library}" | awk '{print $3}' | grep -c '^katago_' || true)"
unexpected_exports="$(nm -D --defined-only "${library}" | awk '{print $3}' | grep -Ev '^(katago_|KATAGO_1$)' || true)"
if [[ "${export_count}" -ne 20 || -n "${unexpected_exports}" ]]; then
  echo "ABI export check failed: expected 20 katago_* functions and no implementation symbols." >&2
  nm -D --defined-only "${library}" >&2
  exit 1
fi

echo "Linux KataGo C ABI build complete:"
echo "  library: ${library}"
echo "  C ABI:   ${c_abi_smoke} (passed)"
echo "  smoke:   ${smoke}"
echo "  exports: ${export_count} (C API only)"
