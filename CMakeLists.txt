cmake_minimum_required(VERSION 3.20)

project(TailGovernor
    VERSION 1.0.0
    DESCRIPTION "Tail Governor: governs p95/p99/p999 tail latency"
    LANGUAGES CXX)

# ---- standards / warnings (strict) ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TAILGOVERNOR_BUILD_TESTS "Build tests" ON)
option(TAILGOVERNOR_BUILD_EXAMPLES "Build examples" ON)
option(TAILGOVERNOR_BUILD_BENCHMARKS "Build benchmarks" ON)
option(TAILGOVERNOR_BUILD_TOOLS "Build CLI tool" ON)
option(TAILGOVERNOR_WITH_CUDA "Build CUDA real-hardware proof (requires CUDA toolkit)" OFF)

# ---- library ----
add_library(tailgovernor STATIC src/governor.cpp)
add_library(TailGovernor::tailgovernor ALIAS tailgovernor)
target_include_directories(tailgovernor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(tailgovernor PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(tailgovernor PRIVATE /W4 /WX)
else()
    target_compile_options(tailgovernor PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

set_target_properties(tailgovernor PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

if(TAILGOVERNOR_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)
    set(CMAKE_CUDA_STANDARD 20)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    enable_language(CUDA)
endif()

# ---- install / export ----
include(GNUInstallDirs)
install(TARGETS tailgovernor EXPORT TailGovernorTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/tailgovernor DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT TailGovernorTargets
    FILE TailGovernorTargets.cmake
    NAMESPACE TailGovernor::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TailGovernor)

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/TailGovernorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/TailGovernorConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TailGovernor)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/TailGovernorConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/TailGovernorConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/TailGovernorConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TailGovernor)

# ---- subdirs ----
if(TAILGOVERNOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(TAILGOVERNOR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(TAILGOVERNOR_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
if(TAILGOVERNOR_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

add_subdirectory(distributed)
if(TAILGOVERNOR_WITH_CUDA)
    add_subdirectory(cuda)
endif()

