# This file sets up a CMakeCache for Apple-style builds of compiler-rt.
# This configuration matches Apple uses when shipping Xcode releases.

set(COMPILER_RT_INCLUDE_TESTS OFF CACHE BOOL "")
set(COMPILER_RT_HAS_SAFESTACK OFF CACHE BOOL "")
set(COMPILER_RT_EXTERNALIZE_DEBUGINFO ON CACHE BOOL "")
set(CMAKE_MACOSX_RPATH ON CACHE BOOL "")

if(CMAKE_OSX_ARCHITECTURES MATCHES "ppc" OR CMAKE_OSX_ARCHITECTURES MATCHES "ppc64")
set(CMAKE_C_FLAGS_RELEASE "-Os" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-Os" CACHE STRING "")
set(CMAKE_ASM_FLAGS_RELEASE "-Os" CACHE STRING "")
else()
set(CMAKE_C_FLAGS_RELEASE "-O3" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-O3" CACHE STRING "")
set(CMAKE_ASM_FLAGS_RELEASE "-O3" CACHE STRING "")
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -gline-tables-only -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -gline-tables-only -DNDEBUG" CACHE STRING "")
set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-O3 -gline-tables-only -DNDEBUG" CACHE STRING "")
set(CMAKE_BUILD_TYPE RELEASE CACHE STRING "")
