cmake_minimum_required(VERSION 4.0)
project(serreg VERSION 2.2.6 LANGUAGES CXX
        DESCRIPTION "es::serreg — C++26 reflection-driven binary serialization with a load-time type registry")

# ---- options ------------------------------------------------------------------------------------------
option(ES_SERREG_BUILD_SHARED "Build libserreg as a shared library (required when dlopen'ed DSOs serialize)" ON)
option(ES_SERREG_BUILD_TOOLS  "Build the srdump tool" ON)
# configured and installed (header-only, C++23): no reflection compiler is needed.
option(ES_SERREG_IPC_ONLY     "Configure and install only es::ipc (no reflection compiler needed)" OFF)
option(ES_SERREG_BUILD_TESTS  "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(ES_SERREG_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
# 2.2.1 (maintainer): on by default — the tree, the tests and the examples compile clean under the full warning set
# (cmake/warnings.cmake) on every preset; a packager hit by a newer compiler's new warning passes
# -DES_SERREG_WERROR=OFF.
option(ES_SERREG_WERROR       "Treat warnings as errors (2.2.1: on by default; every preset builds with -Werror)" ON)
option(ES_SERREG_VALIDATE_READ "Read-side enum range check (DECISIONS A-16): one value for the whole program — set here, it reaches libserreg and, through the exported target, every consumer; a TU that disagrees fails to link" OFF)
# Executable-format backend of the registry (src/image.hpp, review addendum A4): `elf` (dladdr, DT_FLAGS_1, the
set(ES_SERREG_IMAGE_FORMAT "elf" CACHE STRING "Registry image backend: elf | none")
set_property(CACHE ES_SERREG_IMAGE_FORMAT PROPERTY STRINGS elf none)
if(NOT ES_SERREG_IMAGE_FORMAT MATCHES "^(elf|none)$")
  message(FATAL_ERROR "ES_SERREG_IMAGE_FORMAT must be elf or none (got '${ES_SERREG_IMAGE_FORMAT}')")
endif()
if(ES_SERREG_IMAGE_FORMAT STREQUAL "none" AND ES_SERREG_BUILD_SHARED)
  message(FATAL_ERROR "ES_SERREG_IMAGE_FORMAT=none needs -DES_SERREG_BUILD_SHARED=OFF: a shared libserreg must "
                      "observe the image unloads it can suffer")
endif()

# header-only shared-memory regions and SPSC/SPMC rings; C++23, no reflection, no dependency on es::serreg (and
# es::serreg does not depend on it) ---------------------------------------------------------------------------------
add_library(es_ipc INTERFACE)
add_library(es::ipc ALIAS es_ipc)
target_include_directories(es_ipc INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(es_ipc INTERFACE cxx_std_23)
set_target_properties(es_ipc PROPERTIES EXPORT_NAME ipc)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
# The package's config files and es::ipc's own export set, also used by the full build (below).
function(es_serreg_install_package_files)
  install(TARGETS es_ipc EXPORT serregIpcTargets)
  install(DIRECTORY include/es/ipc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/es)
  install(EXPORT serregIpcTargets NAMESPACE es:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/serreg)
  configure_package_config_file(cmake/serregConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/serregConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/serreg)
  # 2.2.1 (DECISIONS G-2): until 3.0 the minor is the compatibility unit — a wire-format or ABI change bumps it,
  # so find_package(serreg 2.2) accepts 2.2.x only, and the SONAME is major.minor (libserreg.so.2.2)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/serregConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/serregConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/serregConfigVersion.cmake
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/serreg)
  # pkg-config for es::ipc alone (header-only)
  file(RELATIVE_PATH ES_SERREG_PC_UP "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
  string(REGEX REPLACE "/$" "" ES_SERREG_PC_UP "${ES_SERREG_PC_UP}")
  if(ES_SERREG_PC_UP STREQUAL "")
    set(ES_SERREG_PC_UP ".")
  endif()
  set(ES_SERREG_PC_UP "${ES_SERREG_PC_UP}" PARENT_SCOPE)
  configure_file(cmake/es-ipc.pc.in ${CMAKE_CURRENT_BINARY_DIR}/es-ipc.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/es-ipc.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endfunction()

if(ES_SERREG_IPC_ONLY)
  es_serreg_install_package_files()
  message(STATUS "es::serreg: ES_SERREG_IPC_ONLY — configuring es::ipc only")
  return()
endif()

# parent's build type and standard are left alone. No C++20 module scanning: the tree has no modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 26)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default Release)")
  endif()
endif()

include(CheckCXXCompilerFlag)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "es::serreg needs GCC >= 16 (-freflection); found ${CMAKE_CXX_COMPILER_VERSION}")
  endif()
  set(ES_SERREG_REFLECTION_FLAG -freflection)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(ES_SERREG_REFLECTION_FLAG -freflection-latest)   # Bloomberg clang-p2996; upstream Clang 22/23 lack P2996
else()
  message(FATAL_ERROR "es::serreg supports GCC 16+ and clang-p2996 only")
endif()
set(CMAKE_REQUIRED_FLAGS "-std=c++26 ${ES_SERREG_REFLECTION_FLAG}")
check_cxx_compiler_flag("${ES_SERREG_REFLECTION_FLAG}" ES_SERREG_HAS_REFLECTION)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT ES_SERREG_HAS_REFLECTION)
  message(FATAL_ERROR "compiler does not accept ${ES_SERREG_REFLECTION_FLAG}")
endif()

include(cmake/warnings.cmake)

# ---- the library ----------------------------------------------------------------------------------------
set(ES_SERREG_SOURCES
  src/registry.cpp src/image_${ES_SERREG_IMAGE_FORMAT}.cpp src/dictionary.cpp src/elf_reader.cpp src/file_io.cpp
  src/reader.cpp src/decoder.cpp src/formatters.cpp)
set(ES_SERREG_TOOLS_SOURCES src/cli.cpp src/wireshark.cpp src/diff.cpp src/cxxgen.cpp)

if(ES_SERREG_BUILD_SHARED)
  add_library(serreg SHARED ${ES_SERREG_SOURCES})
  target_compile_definitions(serreg PUBLIC ES_SERREG_SHARED PRIVATE ES_SERREG_BUILDING)
  set_target_properties(serreg PROPERTIES VERSION ${PROJECT_VERSION}
                        SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}) # 2.2.1: minor = compatibility
else()
  add_library(serreg STATIC ${ES_SERREG_SOURCES})
endif()
add_library(es::serreg ALIAS serreg)

target_include_directories(serreg PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(serreg PUBLIC cxx_std_26)
target_compile_options(serreg PUBLIC ${ES_SERREG_REFLECTION_FLAG})
if(ES_SERREG_VALIDATE_READ)
  target_compile_definitions(serreg PUBLIC ES_SERREG_VALIDATE_READ=1) # whole program (A-16): the fuse in registry.hpp enforces it
endif()
target_link_libraries(serreg PUBLIC Threads::Threads)   
if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf")
  # ELF only 
  # per-image inline variables must stay unloadable (STB_GNU_UNIQUE would make glibc mark a DSO NODELETE, §6.3b),
  # and the backend calls dladdr/dlopen/dlclose/dlinfo.
  # GCC only 
  target_compile_options(serreg PUBLIC $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)
  target_link_libraries(serreg PUBLIC ${CMAKE_DL_LIBS})
endif()
es_serreg_apply_warnings(serreg)
if(ES_SERREG_IMAGE_FORMAT STREQUAL "none")
  target_compile_definitions(serreg PUBLIC ES_SERREG_EMBED_NONE)   # headers emit no es_serreg_dict section
endif()
set_target_properties(serreg PROPERTIES OUTPUT_NAME serreg EXPORT_NAME serreg POSITION_INDEPENDENT_CODE ON
                                        CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

# ---- es::serreg_tools srdump's command line (es::serreg::cli), the Wireshark generator and
# the dictionary diff; applications that embed the CLI link it, everything else needs only es::serreg ---------------
if(ES_SERREG_BUILD_SHARED)
  add_library(serreg_tools SHARED ${ES_SERREG_TOOLS_SOURCES})
  set_target_properties(serreg_tools PROPERTIES VERSION ${PROJECT_VERSION}
                        SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
else()
  add_library(serreg_tools STATIC ${ES_SERREG_TOOLS_SOURCES})
endif()
add_library(es::serreg_tools ALIAS serreg_tools)
target_link_libraries(serreg_tools PUBLIC serreg)
es_serreg_apply_warnings(serreg_tools)
set_target_properties(serreg_tools PROPERTIES OUTPUT_NAME serreg_tools EXPORT_NAME serreg_tools
                                              POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)

# ---- tools ------------------------------------------------------------------------------------------------
if(ES_SERREG_BUILD_TOOLS)
  add_executable(srdump tools/srdump/main.cpp)
  target_link_libraries(srdump PRIVATE es::serreg_tools)
  es_serreg_apply_warnings(srdump)
endif()

# ---- tests / examples --------------------------------------------------------------------------------------
if(ES_SERREG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(ES_SERREG_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
# sanitizer report printed after the matched line (LeakSanitizer at exit, a crash in atexit) must still fail it.
function(es_serreg_fail_on_sanitizer_reports dir)
  get_property(_tests DIRECTORY ${dir} PROPERTY TESTS)
  foreach(_t IN LISTS _tests)
    get_test_property(${_t} PASS_REGULAR_EXPRESSION DIRECTORY ${dir} _pass)
    if(_pass)
      set_tests_properties(${_t} DIRECTORY ${dir} PROPERTIES FAIL_REGULAR_EXPRESSION
                           "ERROR: [A-Za-z]+Sanitizer|WARNING: ThreadSanitizer|runtime error:")
    endif()
  endforeach()
  get_property(_subs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
  foreach(_s IN LISTS _subs)
    es_serreg_fail_on_sanitizer_reports(${_s})
  endforeach()
endfunction()
if(ES_SERREG_BUILD_TESTS OR ES_SERREG_BUILD_EXAMPLES)
  es_serreg_fail_on_sanitizer_reports(${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---- install / export (consumed via find_package(serreg CONFIG) or vcpkg) ---------------------------------

# Installed targets use the same $ORIGIN-relative RPATH in the build tree and after install, so CMake never
# needs to rewrite/relink RPATHs at install time (that step is refused by the Ninja generator whenever CMake
# could not classify the executable format as ELF — e.g. a compiler launcher/wrapper, a cross setup, or a
# stale CMAKE_EXECUTABLE_FORMAT=Unknown cache entry). Non-installed targets (tests, examples) keep the default
# build-tree RPATH.
set(ES_SERREG_INSTALLED_TARGETS serreg serreg_tools)
if(ES_SERREG_BUILD_TOOLS)
  list(APPEND ES_SERREG_INSTALLED_TARGETS srdump)
endif()
set_target_properties(${ES_SERREG_INSTALLED_TARGETS} PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
  INSTALL_RPATH_USE_LINK_PATH OFF)

install(TARGETS serreg serreg_tools EXPORT serregTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(ES_SERREG_BUILD_TOOLS)
  install(TARGETS srdump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/es/serreg DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/es)   # es/ipc: es_serreg_install_package_files()
install(EXPORT serregTargets NAMESPACE es:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/serreg)
es_serreg_install_package_files() # serregConfig.cmake (components ipc, serreg, tools), es::ipc, es-ipc.pc
# pkg-config for non-CMake consumers: carries the flags plugin reloading depends on (cc F12); ELF backend only.
if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf")
  set(ES_SERREG_PC_CFLAGS " -fno-gnu-unique")
  set(ES_SERREG_PC_LIBS " -ldl")
else()
  set(ES_SERREG_PC_CFLAGS " -DES_SERREG_EMBED_NONE")
  set(ES_SERREG_PC_LIBS "")
endif()
# multiarch libdir (lib/x86_64-linux-gnu) works
configure_file(cmake/serreg.pc.in ${CMAKE_CURRENT_BINARY_DIR}/serreg.pc @ONLY)
configure_file(cmake/serreg-tools.pc.in ${CMAKE_CURRENT_BINARY_DIR}/serreg-tools.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/serreg.pc ${CMAKE_CURRENT_BINARY_DIR}/serreg-tools.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
# `file(1)` magic for .ser / .rri captures (share/misc/magic fragment; see README "file command").
install(FILES share/serreg.magic DESTINATION ${CMAKE_INSTALL_DATADIR}/serreg)
