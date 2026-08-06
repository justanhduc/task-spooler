# FindNVML.cmake
#
# Locate the NVIDIA Management Library (NVML).
#
# NVML is what `ts` actually needs for its GPU features -- NOT the CUDA compute
# toolkit. The runtime library `libnvidia-ml.so.1` ships with the NVIDIA driver,
# so on a machine that can run GPU jobs it is already present. This module tries
# to find the header (nvml.h) and a link library without requiring the full CUDA
# Toolkit to be installed. If a CUDA install is present, it is used only as an
# extra search hint (e.g. for the header and the `stubs/` link stub).
#
# Result variables:
#   NVML_FOUND          -- TRUE if both the header and library were found
#   NVML_INCLUDE_DIRS   -- directory containing nvml.h
#   NVML_LIBRARIES      -- the nvidia-ml link library
#
# Imported target (preferred):
#   NVML::NVML
#
# Hint variables (any of these, as CMake or environment variables, are honoured):
#   CUDA_HOME / CUDA_PATH / CUDA_ROOT / CUDAToolkit_ROOT

# Collect optional CUDA locations purely as search hints. Missing ones expand to
# empty strings, which find_*() safely ignores.
set(_nvml_hints
    "${CUDA_HOME}"        "$ENV{CUDA_HOME}"
    "${CUDA_PATH}"        "$ENV{CUDA_PATH}"
    "${CUDA_ROOT}"        "$ENV{CUDA_ROOT}"
    "${CUDAToolkit_ROOT}" "$ENV{CUDAToolkit_ROOT}"
    "/usr/local/cuda"
)

find_path(NVML_INCLUDE_DIR
    NAMES nvml.h
    HINTS ${_nvml_hints}
    PATH_SUFFIXES include
    PATHS
        /usr/include                    # standalone nvml.h, if packaged
        /usr/include/nvidia/current     # some Debian/Ubuntu driver-dev packages
)

set(_nvml_lib_suffixes
    lib64 lib/x64 lib                   # a real driver / toolkit install
    # The CUDA Toolkit ships a link-time stub so you can build on a host that
    # has no driver; the real libnvidia-ml.so.1 is resolved at runtime.
    lib64/stubs lib/x64/stubs lib/stubs
    targets/x86_64-linux/lib/stubs)

set(_nvml_lib_paths
    /usr/lib/wsl/lib                    # WSL2: the Windows driver, passed through.
                                        # Ships only libnvidia-ml.so.1, so the
                                        # find_file() fallback below is what hits.
    /usr/lib/x86_64-linux-gnu           # Debian / Ubuntu driver
    /usr/lib64                          # RHEL / Fedora driver
    /usr/lib
    /usr/lib/nvidia)

find_library(NVML_LIBRARY
    NAMES nvidia-ml nvml                # nvidia-ml on Linux, nvml on Windows
    HINTS ${_nvml_hints}
    PATH_SUFFIXES ${_nvml_lib_suffixes}
    PATHS ${_nvml_lib_paths}
)

# Driver-only hosts sometimes ship the runtime soname (libnvidia-ml.so.1) without
# the unversioned .so symlink that find_library() looks for. Fall back to linking
# the versioned file directly by absolute path.
if(NOT NVML_LIBRARY)
    find_file(NVML_LIBRARY
        NAMES libnvidia-ml.so.1
        HINTS ${_nvml_hints}
        PATH_SUFFIXES ${_nvml_lib_suffixes}
        PATHS ${_nvml_lib_paths}
    )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NVML
    REQUIRED_VARS NVML_LIBRARY NVML_INCLUDE_DIR)

if(NVML_FOUND)
    set(NVML_LIBRARIES    ${NVML_LIBRARY})
    set(NVML_INCLUDE_DIRS ${NVML_INCLUDE_DIR})

    if(NOT TARGET NVML::NVML)
        add_library(NVML::NVML UNKNOWN IMPORTED)
        set_target_properties(NVML::NVML PROPERTIES
            IMPORTED_LOCATION "${NVML_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NVML_INCLUDE_DIR}")
    endif()
endif()

mark_as_advanced(NVML_INCLUDE_DIR NVML_LIBRARY)
