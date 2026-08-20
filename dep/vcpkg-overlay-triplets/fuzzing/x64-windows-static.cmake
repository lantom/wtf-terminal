# Same as the official x64-windows-static triplet
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# ...but with explicit platform toolset, so that future toolsets
# aren't automatically picked up (it defaults to the latest one).
#
# src/common.build.pre.props selects v145 only on Visual Studio 18 and newer and falls
# back to v143 otherwise, so pinning v145 unconditionally here breaks the build on
# Visual Studio 2022. Honour the same choice by letting the build pass it in.
if(DEFINED ENV{VCPKG_PLATFORM_TOOLSET})
    set(VCPKG_PLATFORM_TOOLSET $ENV{VCPKG_PLATFORM_TOOLSET})
else()
    set(VCPKG_PLATFORM_TOOLSET v145)
endif()

set(VCPKG_CXX_FLAGS /fsanitize=address)
set(VCPKG_C_FLAGS /fsanitize=address)
