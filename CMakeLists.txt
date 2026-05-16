cmake_minimum_required(VERSION 3.16)
project(karo_sdk_cpp
  VERSION 3.2.15
  LANGUAGES CXX
  DESCRIPTION "Karo X2 robot third-party C++ SDK")

# ----------------------------------------------------------------------------
# Settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(KARO_SDK_BUILD_EXAMPLES "Build example programs" ON)
# 默认 OFF: 普通客户 build SDK + examples 不应该被强迫装 GTest; 仅 SDK 维护方
# 跑单测时显式传 -DKARO_SDK_BUILD_TESTS=ON.
option(KARO_SDK_BUILD_TESTS    "Build unit tests (需 GTest, 普通 build 留 OFF)" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ----------------------------------------------------------------------------
# Dependencies
# ----------------------------------------------------------------------------
# Boost (Beast WebSocket + Asio): >= 1.71 (Ubuntu 20.04 默认 libboost-dev = 1.71).
# Beast WSS / Asio SSL 在 1.71 已稳定可用; 1.66 起即支持 ssl_stream<websocket::stream>.
#
# Boost.System 1.69+ 已经是 header-only, 实际不需要 link runtime 库; 但老版
# Ubuntu 20.04 + Boost 1.71 的 BoostConfig 仍有 Boost::system 别名, 新版 1.85+
# (brew 1.90) 把 system 改成 headers 模块. 用 headers (跨版本通用) 来 link.
find_package(Boost 1.71 REQUIRED)

# OpenSSL: client X.509 cert load + TLS context
find_package(OpenSSL REQUIRED)

# Protobuf: SDK / v1 message codec
find_package(Protobuf REQUIRED)

# absl: protobuf 7.34+ 依赖 absl, examples 链接 .a 时需要显式找它的 hash 库.
# 老 protobuf (3.x) 没有 absl 依赖, find_package 允许失败.
find_package(absl QUIET)

# Threads
find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
# Generated proto sources
# ----------------------------------------------------------------------------
# proto 源码可能位于 ./proto/ (本仓默认 layout) 或 ../proto/ (上一层目录), 自动
# 探测以兼容不同 unpack 方式. 找不到时给明确报错.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../proto/sdk/common.proto")
  set(KARO_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/proto/sdk/common.proto")
  set(KARO_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
else()
  message(FATAL_ERROR
    "Cannot locate proto/ directory; expected either ./proto or ../proto. "
    "Looked for sdk/common.proto as sentinel.")
endif()
message(STATUS "KaroSDK proto source: ${KARO_PROTO_DIR}")

set(KARO_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY ${KARO_PROTO_OUT})

# v1 阶段需要的 proto: sdk envelope + v1 RPC envelope + control + safety + system
# + stream / telemetry_stream (订阅 robot.* 推送 typed update).
set(KARO_PROTO_FILES
  sdk/common.proto
  sdk/session.proto
  # SDK v1 telemetry: SdkRobotStateUpdate. 服务端 publish 前已完成字段投影 +
  # errors[] 白名单过滤; 客户端用 fromBinary 直接反序列化得到 plain struct.
  sdk/v1/telemetry.proto
  karo/v1/common.proto
  karo/v1/rpc.proto
  karo/v1/control.proto
  karo/v1/safety.proto
  karo/v1/stream.proto
  karo/v1/telemetry_stream.proto
  karo/v1/transport.proto
  karo/v1/tasks.proto
)

set(KARO_PROTO_GEN_SRCS)
set(KARO_PROTO_GEN_HDRS)
foreach(_p ${KARO_PROTO_FILES})
  string(REPLACE ".proto" ".pb.cc" _src "${KARO_PROTO_OUT}/${_p}")
  string(REPLACE ".proto" ".pb.h"  _hdr "${KARO_PROTO_OUT}/${_p}")
  list(APPEND KARO_PROTO_GEN_SRCS ${_src})
  list(APPEND KARO_PROTO_GEN_HDRS ${_hdr})
  add_custom_command(
    OUTPUT ${_src} ${_hdr}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
      # 部分 .proto 使用 proto3 optional (protoc 3.15+ 标准). 兼容
      # Ubuntu 20.04/22.04 默认 libprotobuf-dev (3.6/3.12) — 它们的 protoc 把
      # optional 当 experimental, 必须显式开 flag. 3.15+ 上无副作用.
      --experimental_allow_proto3_optional
      --proto_path=${KARO_PROTO_DIR}
      --cpp_out=${KARO_PROTO_OUT}
      ${_p}
    DEPENDS ${KARO_PROTO_DIR}/${_p}
    WORKING_DIRECTORY ${KARO_PROTO_DIR}
    COMMENT "protoc ${_p}"
    VERBATIM)
endforeach()

# ----------------------------------------------------------------------------
# Library
# ----------------------------------------------------------------------------
add_library(karo_sdk_cpp
  src/rate_limiter.cpp
  src/error.cpp
  src/connection_state.cpp
  src/subscription.cpp
  src/diagnostics.cpp
  src/log.cpp
  src/dispatcher.cpp
  src/proto_codec.cpp
  src/ws_client.cpp
  src/robot_impl.cpp
  src/robot.cpp
  ${KARO_PROTO_GEN_SRCS}
)
# Alias 对齐 find_package(KaroSDK CONFIG) 导出的 imported target:
#   target_link_libraries(my_app PRIVATE KaroSDK::karo_sdk_cpp)
# 同源构建 (examples / tests) 同样走该 alias, 与下游一致.
add_library(KaroSDK::karo_sdk_cpp ALIAS karo_sdk_cpp)

target_include_directories(karo_sdk_cpp
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${KARO_PROTO_OUT}
)
target_link_libraries(karo_sdk_cpp
  PUBLIC  Threads::Threads
  # 静态库 (默认 STATIC) 的传递性 link: protobuf / OpenSSL / Boost 的符号
  # 在 karo_sdk_cpp.a 内部用到, examples / 客户 app link 时必须能解析这些符号.
  PUBLIC  Boost::headers OpenSSL::SSL OpenSSL::Crypto protobuf::libprotobuf
)

# protobuf 7.34+ 依赖 absl::hash 等; CMake 链接静态 lib 时不自动传递, 显式加.
if(absl_FOUND)
  target_link_libraries(karo_sdk_cpp PUBLIC
    absl::hash absl::base absl::strings absl::log absl::log_internal_check_op
    absl::status absl::statusor)
endif()

# ----------------------------------------------------------------------------
# Examples
# ----------------------------------------------------------------------------
if(KARO_SDK_BUILD_EXAMPLES)
  foreach(ex teleop subscribe_status emergency_stop estop_cmdvel_e2e)
    add_executable(${ex} examples/${ex}.cpp)
    target_link_libraries(${ex} PRIVATE KaroSDK::karo_sdk_cpp)
  endforeach()
endif()

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
# KARO_SDK_BUILD_TESTS=ON 时启用单元测试. 发行包不携带 test/ 源码, EXISTS 检查
# 失败时自动 no-op — 客户即便误传 -DKARO_SDK_BUILD_TESTS=ON 也不会 fail.
if(KARO_SDK_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/test_rate_limiter.cpp")
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(test_rate_limiter test/test_rate_limiter.cpp)
  target_link_libraries(test_rate_limiter PRIVATE
    KaroSDK::karo_sdk_cpp GTest::gtest GTest::gtest_main)
  add_test(NAME test_rate_limiter COMMAND test_rate_limiter)

  add_executable(test_proto_codec test/test_proto_codec.cpp)
  target_link_libraries(test_proto_codec PRIVATE
    KaroSDK::karo_sdk_cpp GTest::gtest GTest::gtest_main)
  target_include_directories(test_proto_codec PRIVATE ${KARO_PROTO_OUT})
  add_test(NAME test_proto_codec COMMAND test_proto_codec)
endif()

# ----------------------------------------------------------------------------
# Install + CMake package export
# ----------------------------------------------------------------------------
# 双层命名约定:
#   - find_package(KaroSDK CONFIG REQUIRED)
#   - target_link_libraries(my_app PRIVATE KaroSDK::karo_sdk_cpp)
#   - 实际 library file = libkaro_sdk_cpp.a
install(TARGETS karo_sdk_cpp
  EXPORT KaroSDKTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/karo
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")

install(EXPORT KaroSDKTargets
  FILE KaroSDKTargets.cmake
  NAMESPACE KaroSDK::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KaroSDK)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KaroSDKConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/KaroSDKConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KaroSDK
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/KaroSDKConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/KaroSDKConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/KaroSDKConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KaroSDK)

# ----------------------------------------------------------------------------
# CPack — release artifact 打包
# ----------------------------------------------------------------------------
# 按 karo-{model}-sdk[-{lang}] 命名约定: karo-x2-sdk-cpp 表示 X2 机型的 C++ SDK.
# 后续机型 (例 X3) 单独发行同一格式 .deb, 可与本包共存.
set(CPACK_PACKAGE_NAME "karo-x2-sdk-cpp")
set(CPACK_PACKAGE_VENDOR "Kanesir Robotics")
set(CPACK_PACKAGE_CONTACT "developer@kanesir.com")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Kanesir-Developer/karo-x2-sdk-cpp")

# 产物按平台条件:
#   Linux (Ubuntu / Debian, 有 dpkg): DEB + TGZ
#   macOS / 其他: 只 TGZ (DEB 需要 dpkg-deb, 在 macOS 上 cpack 直接 fail)
if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;TGZ")
else()
  set(CPACK_GENERATOR "TGZ")
endif()

# DEB 元数据
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
# SHLIBDEPS=OFF: 关闭 dpkg-shlibdeps 自动依赖探测 (依赖宿主 `file` 命令, 部分
# 构建环境未安装). 改为下面显式列 CPACK_DEBIAN_PACKAGE_DEPENDS.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
# Protobuf 版本下限对齐 README "≥ 3.6" — Ubuntu 20.04 默认 3.6.1, Ubuntu 22.04
# 默认 3.12.4. find_package(Protobuf REQUIRED) 不锁 3.21+ 上限.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "libboost-dev (>= 1.71), libssl-dev, libprotobuf-dev (>= 3.6)")

# Replaces / Conflicts — 防御性条目, 保证 apt 安装时能干净替换早期未发布命名
# (karo-sdk-cpp), 不报 file conflicts.
set(CPACK_DEBIAN_PACKAGE_REPLACES "karo-sdk-cpp")
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "karo-sdk-cpp")
# 若 build host 装了 absl (= protobuf 7.34+ transitive dep), KaroSDKConfig 会
# 要 find_dependency(absl), DEB 也必须把 libabsl-dev 列进依赖, 否则客户 apt
# install karo-sdk-cpp 后 find_package(KaroSDK CONFIG) 在干净机器上 absl
# 不可解析. 无 absl build (老 protobuf 3.x) 时不加, 跟 KaroSDK_WITH_ABSL=0 同步.
if(absl_FOUND)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libabsl-dev")
endif()

# Source TGZ 元数据
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME
  "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-source")
# 用 CMake bracket-arguments [[...]] 避免 \. 被字符串解析当作无效转义
# (CMake 3.20+ CMP0010 NEW 会把无效转义当 error).
set(CPACK_SOURCE_IGNORE_FILES
  [[/build.*/]]
  [[/\.git/]]
  [[/\.cache/]]
  [[\.DS_Store]]
)
# 源码 tarball 必须含 proto/, 否则解开后 cpp/ 单独 cmake 时探测不到 .proto.
# CPACK_SOURCE_INSTALLED_DIRECTORIES 把 proto/ 一并打入 source tarball.
set(CPACK_SOURCE_INSTALLED_DIRECTORIES
  "${CMAKE_CURRENT_SOURCE_DIR}" "."
  "${KARO_PROTO_DIR}" "proto")

include(CPack)
