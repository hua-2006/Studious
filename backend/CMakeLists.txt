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
    src/asset/ManifestParser.cpp
    src/asset/AssetRegistry.cpp
    src/asset/Downloader.cpp
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

# 开发/发布用清单工具（genkey / sign / verify）。不随产品分发，不进安装规则。
add_executable(manifest_tool tools/manifest_tool.cpp)
target_link_libraries(manifest_tool PRIVATE backend)
target_include_directories(manifest_tool PRIVATE ${MONOCYPHER_DIR})
set_target_properties(manifest_tool PROPERTIES
    WIN32_EXECUTABLE OFF
)
