#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${JP2EMUELLA_BUILD_DIR:-"${project_dir}/build"}
: "${EMUELLA_J2K_SOURCE_DIR:?Set EMUELLA_J2K_SOURCE_DIR to the exact codec checkout}"
codec_source=${EMUELLA_J2K_SOURCE_DIR}
codec_library=${EMUELLA_J2K_LIBRARY:-"${codec_source}/target/release/libemuella_j2k_capi.so"}
cmake_command=${CMAKE_COMMAND:-cmake}
ctest_command=${CTEST_COMMAND:-ctest}
gdal_config=${GDAL_CONFIG:-gdal-config}
gdal_prefix=${GDAL_PREFIX:-"$(${gdal_config} --prefix)"}

"${cmake_command}" -S "${project_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${gdal_prefix}" \
    -DEmuellaJ2K_SOURCE_DIR="${codec_source}" \
    -DEmuellaJ2K_INCLUDE_DIR="${codec_source}/crates/emuella-j2k-capi/include" \
    -DEmuellaJ2K_LIBRARY="${codec_library}" \
    -DBUILD_TESTING=ON
"${cmake_command}" --build "${build_dir}" --parallel
"${ctest_command}" --test-dir "${build_dir}" --output-on-failure

install_root="${build_dir}/install-check"
"${cmake_command}" --install "${build_dir}" --prefix "${install_root}"
plugin=$(find "${install_root}" -type f -name 'gdal_JP2Emuella.so' -print -quit)
if [[ -z "${plugin}" ]]; then
    echo "installed gdal_JP2Emuella.so was not found" >&2
    exit 1
fi
for symbol in GDALRegister_JP2Emuella GDALRegisterMe; do
    if ! nm -D --defined-only "${plugin}" | awk '{print $3}' | grep -qx "${symbol}"; then
        echo "installed plugin does not export ${symbol}" >&2
        exit 1
    fi
done
if readelf -d "${plugin}" | grep -Eq '\((NEEDED|RPATH|RUNPATH)\).*\[/' ; then
    echo "installed plugin embeds an absolute dynamic dependency or search path" >&2
    readelf -d "${plugin}" >&2
    exit 1
fi

gdalinfo_command=${GDALINFO_COMMAND:-"${gdal_prefix}/bin/gdalinfo"}
if [[ ! -x "${gdalinfo_command}" ]]; then
    gdalinfo_command=$(command -v gdalinfo)
fi
plugin_dir=$(dirname "${plugin}")
codec_library_dir=$(dirname "${codec_library}")
runtime_library_path="${codec_library_dir}:${gdal_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
if ! GDAL_DRIVER_PATH="${plugin_dir}" LD_LIBRARY_PATH="${runtime_library_path}" \
    "${gdalinfo_command}" --formats | grep -F 'JP2Emuella' >/dev/null; then
    echo "installed plugin did not autoload through gdalinfo" >&2
    exit 1
fi
if ! GDAL_DRIVER_PATH="${plugin_dir}" LD_LIBRARY_PATH="${runtime_library_path}" \
    "${gdalinfo_command}" -checksum \
    "${project_dir}/tests/fixtures/gray-gradient-17x19.j2k" | \
    grep -F 'Checksum=3736' >/dev/null; then
    echo "installed plugin did not reproduce the fixture checksum" >&2
    exit 1
fi
