# vcpkg overlay port for es::serreg. Until a tagged release is published, the port builds from the source tree
# that contains this ports/ directory (SOURCE_PATH points two levels up). For a release, replace the block
# below with vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO erez-strauss/serreg REF v2.2.6 SHA512 <hash>).
get_filename_component(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" ES_SERREG_SHARED)
vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
  FEATURES
    tools ES_SERREG_BUILD_TOOLS
    tests ES_SERREG_BUILD_TESTS)

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    ${FEATURE_OPTIONS}
    -DES_SERREG_BUILD_SHARED=${ES_SERREG_SHARED}
    -DES_SERREG_BUILD_EXAMPLES=OFF
    -DES_SERREG_WERROR=OFF)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME serreg CONFIG_PATH lib/cmake/serreg)
if("tools" IN_LIST FEATURES)
  vcpkg_copy_tools(TOOL_NAMES srdump AUTO_CLEAN)
endif()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
