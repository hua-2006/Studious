cmake_minimum_required(VERSION 3.21)

# Monocypher 是 C 代码，LANGUAGES 里必须有 C，否则 monocypher.c 无法编译。
project(studious_backend LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Monocypher 4.0.2 是 C99 代码（用了 stdint / size_t / for 循环内声明）。
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 目前不需要 moc（无 QObject 子类），开着无害，后续加 QObject 组件不用改构建脚本。
set(CMAKE_AUTOMOC ON)

# 统一输出目录，方便脚本直接取可执行文件。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql)

# 第三方：sentencepiece 0.2.0（Apache-2.0，源码内嵌）。
# 只用分词，不训练、不导出：关掉 shared / tcmalloc / 测试，
# 目标就是静态库 sentencepiece-static（含内部 protobuf-lite 与 absl）。
set(SENTENCEPIECE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sentencepiece)
set(SPM_ENABLE_SHARED OFF CACHE BOOL "Build shared libs" FORCE)
set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "Use tcmalloc" FORCE)
set(SPM_BUILD_TEST OFF CACHE BOOL "Build spm test binaries" FORCE)
set(SPM_ABSL_PROVIDER "internal" CACHE STRING "absl provider" FORCE)
set(SPM_PROTOBUF_PROVIDER "internal" CACHE STRING "protobuf provider" FORCE)
# EXCLUDE_FROM_ALL：spm_encode/spm_train 这些命令行工具和它的 install 规则都不要，
# 只有被 backend 真正链到时才编译 sentencepiece-static。
# （vendored 副本里把它的 cmake_minimum_required 从 3.1 抬到 3.5，
#  否则 CMake 3.31+ 会为 "<3.5 兼容性" 发弃用警告，见 third_party/sentencepiece/CMakeLists.txt）
add_subdirectory(${SENTENCEPIECE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/third_party/sentencepiece EXCLUDE_FROM_ALL)

# 第三方源码自带的警告（protobuf-lite 在 -O3 下的 -Warray-bounds）不是我们要修的：
# 单独对它静音，构建日志里就只剩我们自己的警告。
if(TARGET sentencepiece-static)
    target_compile_options(sentencepiece-static PRIVATE -w)
endif()

# 第三方：onnxruntime 1.31.1 预编译（CPU provider）。
# 头文件 + MSVC 导入库（MinGW 能直接链），运行期依赖 onnxruntime.dll，见文件末尾 POST_BUILD。
set(ONNXRUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime)
set(ONNXRUNTIME_DLL ${ONNXRUNTIME_DIR}/lib/onnxruntime.dll)
set(ONNXRUNTIME_LIB ${ONNXRUNTIME_DIR}/lib/onnxruntime.lib)

# 第三方：Monocypher 4.0.2（BSD-2-Clause OR CC0-1.0，单文件无依赖）。
# 两个 .c 必须都编进来：monocypher-ed25519.c 用到 monocypher.c 里的基础原语。
set(MONOCYPHER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher)

# 第三方：miniz 3.0.2（MIT / public domain amalgamation 单文件版）。
# 只用它的 zip **读取**（解压资产包），不用它的压缩写入。
set(MINIZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)

# 注意：只用 Qt Core / Network，不引 Widgets / Gui，后端保持无 UI 依赖。
add_library(backend STATIC
    src/contract/LanguageRegistry.cpp
    src/i18n/L10n.cpp
    src/i18n/LanguageDetector.cpp
    src/net/IHttpClient.cpp
    src/net/QtHttpClient.cpp
    src/translators/adapters/GoogleFreeAdapter.cpp
    src/translators/adapters/MyMemoryAdapter.cpp
    src/translators/adapters/AzureTranslatorAdapter.cpp
    src/translators/WebProvider.cpp
    src/translators/WebTranslator.cpp
    src/translators/MockTranslator.cpp
    src/translators/LocalMtTranslator.cpp
    src/translators/LlmTranslator.cpp
    src/llm/LlmCatalog.cpp
    src/router/TranslatorRouter.cpp
    src/cache/TranslationCache.cpp
    src/runtime/LlamaServer.cpp
    src/asset/ManifestParser.cpp
    src/asset/AssetRegistry.cpp
    src/asset/Downloader.cpp
    src/mt/MtTokenizer.cpp
    src/mt/MtModel.cpp
    src/Factory.cpp
    # ---- 第三方（C） ----
    ${MONOCYPHER_DIR}/monocypher.c
    ${MONOCYPHER_DIR}/monocypher-ed25519.c
    ${MINIZ_DIR}/miniz.c
)

target_include_directories(backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # 内部头文件统一以 "contract/xxx.h" 形式互相引用，因此把 include/studious 也挂上，
    # 外部既可写 <studious/contract/Types.h> 也可写 "contract/Types.h"。
    ${CMAKE_CURRENT_SOURCE_DIR}/include/studious
    # 适配器头文件放在 src 下（实现细节，不对外公开）。
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Monocypher 头文件只在库内部用（ManifestParser.cpp 里 `#include "monocypher-ed25519.h"`），
# 不对外暴露：外部不需要知道我们用了哪个加密库。
target_include_directories(backend PRIVATE ${MONOCYPHER_DIR})

# miniz 同理：解压是实现细节，调用方只关心 Downloader 的接口。
target_include_directories(backend PRIVATE ${MINIZ_DIR})

target_link_libraries(backend PUBLIC Qt6::Core Qt6::Network Qt6::Sql)

# 本地 MT：sentencepiece 与 onnxruntime 都是实现细节，头文件不外泄，
# 但静态库要把依赖往上传，否则可执行文件链不上（PRIVATE 在 STATIC 下等同 LINK_ONLY）。
target_include_directories(backend PRIVATE
    ${SENTENCEPIECE_DIR}/src
    ${ONNXRUNTIME_DIR}/include
)
target_link_libraries(backend PRIVATE sentencepiece-static ${ONNXRUNTIME_LIB})

# 控制台自测程序
add_executable(backend_selftest src/selftest_main.cpp)
target_link_libraries(backend_selftest PRIVATE backend)
set_target_properties(backend_selftest PROPERTIES
    WIN32_EXECUTABLE OFF          # 保持控制台程序，直接看输出
)
# 自测里用 TEST seed 现场签一份清单，所以也需要 Monocypher 头。
target_include_directories(backend_selftest PRIVATE ${MONOCYPHER_DIR})
# --assets 用例要现场造 zip（含 zip-slip 恶意包），所以自测也直接引 miniz。
target_include_directories(backend_selftest PRIVATE ${MINIZ_DIR})

# onnxruntime.dll 是运行期依赖：backend 是静态库不产出 exe，
# 谁链接了 backend 谁负责把 dll 放到自己旁边，否则 --mt 一启动就 0xC0000135。
add_custom_command(TARGET backend_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:backend_selftest>"
    COMMENT "复制 onnxruntime.dll 到 backend_selftest 输出目录"
)

# 开发/发布用清单工具（genkey / sign / verify）。不随产品分发，不进安装规则。
add_executable(manifest_tool tools/manifest_tool.cpp)
target_link_libraries(manifest_tool PRIVATE backend)
target_include_directories(manifest_tool PRIVATE ${MONOCYPHER_DIR})
set_target_properties(manifest_tool PROPERTIES
    WIN32_EXECUTABLE OFF
)
add_custom_command(TARGET manifest_tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:manifest_tool>"
    COMMENT "复制 onnxruntime.dll 到 manifest_tool 输出目录"
)
