if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO grpc/grpc
    REF "v${VERSION}"
    SHA512 92d603edf330fc8c421d8f6feaca3dafdb71bb70775ce45292bb62368d433d45012a89a854e681376a6cf1c46e036b857b0be157af989dc09260a7715e84c0b3
    HEAD_REF master
    PATCHES
        00001-fix-uwp.patch
        00002-static-linking-in-linux.patch
        00004-link-gdi32-on-windows.patch
        00005-fix-uwp-error.patch
        00006-utf8-range.patch
        00015-disable-download-archive.patch
        00016-fix-plugin-targets.patch
)

vcpkg_from_github(
    OUT_SOURCE_PATH GRPC_PROTO_SOURCE_PATH
    REPO grpc/grpc-proto
    REF dd549fc4ed04b60b0fdd3866b82ef0a7c87c6e4f
    SHA512 0ef851d29d6a39e2dee16c7b5b6ae12bbac0199a45be82cdb73105684fb9543f61ca439bf52cc7982c98ff186d85a53e4814bce08236735f97e4c6c9610d9bda
    HEAD_REF master
)
file(REMOVE_RECURSE "${SOURCE_PATH}/third_party/grpc-proto")
file(COPY "${GRPC_PROTO_SOURCE_PATH}/" DESTINATION "${SOURCE_PATH}/third_party/grpc-proto")
file(REMOVE_RECURSE
    "${SOURCE_PATH}/third_party/abseil-cpp"
    "${SOURCE_PATH}/third_party/cares"
    "${SOURCE_PATH}/third_party/protobuf"
    "${SOURCE_PATH}/third_party/re2"
    "${SOURCE_PATH}/third_party/utf8_range"
    "${SOURCE_PATH}/third_party/zlib"
)

if(VCPKG_CROSSCOMPILING)
    vcpkg_add_to_path(PREPEND "${CURRENT_HOST_INSTALLED_DIR}/tools/grpc")
endif()

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" gRPC_MSVC_STATIC_RUNTIME)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" gRPC_STATIC_LINKING)

set(cares_CARES_PROVIDER "package")
if(VCPKG_TARGET_IS_UWP)
    set(cares_CARES_PROVIDER "OFF")
endif()

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        codegen     gRPC_BUILD_CODEGEN
        systemd     gRPC_USE_SYSTEMD
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS ${FEATURE_OPTIONS}
        -DgRPC_INSTALL=ON
        -DgRPC_BUILD_TESTS=OFF
        -DgRPC_STATIC_LINKING=${gRPC_STATIC_LINKING}
        -DgRPC_MSVC_STATIC_RUNTIME=${gRPC_MSVC_STATIC_RUNTIME}
        -DgRPC_ZLIB_PROVIDER=package
        -DgRPC_SSL_PROVIDER=package
        -DgRPC_PROTOBUF_PROVIDER=package
        -DgRPC_ABSL_PROVIDER=package
        -DgRPC_RE2_PROVIDER=package
        -DgRPC_CARES_PROVIDER=${cares_CARES_PROVIDER}
        -DgRPC_BENCHMARK_PROVIDER=none
        -DgRPC_INSTALL_BINDIR:STRING=bin
        -DgRPC_INSTALL_LIBDIR:STRING=lib
        -DgRPC_INSTALL_INCLUDEDIR:STRING=include
        -DgRPC_INSTALL_CMAKEDIR:STRING=share/grpc
        "-D_gRPC_PROTOBUF_PROTOC_EXECUTABLE=${CURRENT_HOST_INSTALLED_DIR}/tools/protobuf/protoc${VCPKG_HOST_EXECUTABLE_SUFFIX}"
        "-DProtobuf_PROTOC_EXECUTABLE=${CURRENT_HOST_INSTALLED_DIR}/tools/protobuf/protoc${VCPKG_HOST_EXECUTABLE_SUFFIX}"
        -DgRPC_BUILD_GRPCPP_OTEL_PLUGIN=OFF
        -DgRPC_DOWNLOAD_ARCHIVES=OFF
    MAYBE_UNUSED_VARIABLES
        gRPC_MSVC_STATIC_RUNTIME
        gRPC_USE_SYSTEMD
)

vcpkg_cmake_install(ADD_BIN_TO_PATH)
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup()

if (VCPKG_TARGET_IS_WINDOWS)
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib/pkgconfig" "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig")
else()
    vcpkg_fixup_pkgconfig()
endif()

if (gRPC_BUILD_CODEGEN)
    vcpkg_copy_tools(
        AUTO_CLEAN
        TOOL_NAMES
            grpc_php_plugin
            grpc_python_plugin
            grpc_node_plugin
            grpc_objective_c_plugin
            grpc_csharp_plugin
            grpc_cpp_plugin
            grpc_ruby_plugin
    )
else()
    configure_file("${CMAKE_CURRENT_LIST_DIR}/gRPCTargets-vcpkg-tools.cmake" "${CURRENT_PACKAGES_DIR}/share/grpc/gRPCTargets-vcpkg-tools.cmake" @ONLY)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share" "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
