# SPDX-License-Identifier: LGPL-3.0-or-later
#
# A few wlroots headers (wlr_scene.h, wlr_matrix.h) declare parameters like
# `const float color[static 4]`. That syntax is C99-only - a hard parse
# error in C++, not just a linkage issue `extern "C"` can paper over. The
# `static N` there is only a hint that the caller passes at least N
# elements; the parameter still decays to a plain pointer either way, so
# dropping the hint changes neither the signature nor the ABI. This copies
# just the affected headers into the build dir with the hint stripped, and
# puts that directory ahead of the system one on the include path so
# `#include <wlr/...>` resolves to the patched copy.

find_package(PkgConfig REQUIRED)
pkg_get_variable(WLROOTS_INCLUDEDIR wlroots-0.18 includedir)
set(BIOME_WLROOTS_SYSTEM_INCLUDE_DIR "${WLROOTS_INCLUDEDIR}/wlroots-0.18")

set(BIOME_WLROOTS_SHIM_DIR "${CMAKE_BINARY_DIR}/wlroots-cxx-shim")
set(BIOME_WLROOTS_SHIM_HEADERS "")

function(biome_patch_cxx_header relpath)
    set(src "${BIOME_WLROOTS_SYSTEM_INCLUDE_DIR}/${relpath}")
    set(dst "${BIOME_WLROOTS_SHIM_DIR}/${relpath}")
    get_filename_component(dst_dir ${dst} DIRECTORY)
    file(MAKE_DIRECTORY ${dst_dir})
    add_custom_command(
        OUTPUT ${dst}
        COMMAND sed -E "s/\\[static ([0-9]+)\\]/[\\1]/g" ${src} > ${dst}
        DEPENDS ${src}
        COMMENT "Patching ${relpath} for C++ compatibility"
        VERBATIM
    )
    set(BIOME_WLROOTS_SHIM_HEADERS ${BIOME_WLROOTS_SHIM_HEADERS} ${dst} PARENT_SCOPE)
endfunction()

biome_patch_cxx_header("wlr/types/wlr_scene.h")
biome_patch_cxx_header("wlr/types/wlr_matrix.h")

add_custom_target(biome_wlroots_cxx_shim DEPENDS ${BIOME_WLROOTS_SHIM_HEADERS})
