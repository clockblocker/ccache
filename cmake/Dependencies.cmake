include(FindPackageHandleStandardArgs)

if(POLICY CMP0135)
  # Set timestamps on extracted files to time of extraction.
  cmake_policy(SET CMP0135 NEW)
endif()

# Hint about usage of the previously supported OFFLINE variable.
if(OFFLINE)
  message(FATAL_ERROR "Please use -D DEPS=LOCAL instead of -D OFFLINE=ON")
endif()

# How to locate/retrieve dependencies. See the Dependencies section in
# doc/install.md.
set(DEPS AUTO CACHE STRING "How to retrieve third party dependencies")
set_property(CACHE DEPS PROPERTY STRINGS AUTO DOWNLOAD LOCAL)

if(FETCHCONTENT_FULLY_DISCONNECTED)
  message(STATUS "Setting DEPS=LOCAL as FETCHCONTENT_FULLY_DISCONNECTED is set")
  set(DEPS LOCAL)
endif()

find_package(Blake3 1.4.0 MODULE REQUIRED)
find_package(Fmt 8.0.0 MODULE REQUIRED)
find_package(TlExpected 1.1.0 MODULE REQUIRED)
find_package(Xxhash 0.8.0 MODULE REQUIRED)
find_package(Zstd 1.3.4 MODULE REQUIRED)

if(ENABLE_TESTING)
  find_package(Doctest 2.4.6 MODULE REQUIRED)
endif()

include(FetchContent)
set(REFLECTCPP_JSON ON CACHE BOOL "" FORCE)
set(REFLECTCPP_USE_BUNDLED_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(REFLECTCPP_USE_VCPKG OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  reflectcpp
  GIT_REPOSITORY https://github.com/clockblocker/reflect-cpp.git
  GIT_TAG showcase/all-lanes
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(reflectcpp)
get_target_property(_rfl_inc reflectcpp INTERFACE_INCLUDE_DIRECTORIES)
if(_rfl_inc)
  set_target_properties(
    reflectcpp
    PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_rfl_inc}"
  )
endif()
