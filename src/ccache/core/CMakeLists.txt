set(
  sources
  atomicfile.cpp
  cacheentry.cpp
  common.cpp
  filerecompressor.cpp
  mainoptions.cpp
  manifest.cpp
  result.cpp
  resultextractor.cpp
  resultinspector.cpp
  resultretriever.cpp
  statistics.cpp
  statisticscounters.cpp
  statslog.cpp
  types.cpp
)

file(GLOB headers *.hpp)
list(APPEND sources ${headers})

target_sources(ccache_framework PRIVATE ${sources})

include(FetchContent)
set(REFLECTCPP_JSON OFF CACHE BOOL "" FORCE)
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
  set_target_properties(reflectcpp PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_rfl_inc}")
endif()
target_link_libraries(ccache_framework PRIVATE reflectcpp)
