cmake_minimum_required(VERSION 3.21)

project(studious_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 目前不需要 moc（无 QObject 子类），开着无害，后续加 QObject 组件不用改构建脚本。
set(CMAKE_AUTOMOC ON)

# 统一输出目录，方便脚本直接取可执行文件。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql)

# 注意：只用 Qt Core / Network，不引 Widgets / Gui，后端保持无 UI 依赖。
add_library(backend STATIC
    src/contract/LanguageRegistry.cpp
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
    src/Factory.cpp
)

target_include_directories(backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # 内部头文件统一以 "contract/xxx.h" 形式互相引用，因此把 include/studious 也挂上，
    # 外部既可写 <studious/contract/Types.h> 也可写 "contract/Types.h"。
    ${CMAKE_CURRENT_SOURCE_DIR}/include/studious
    # 适配器头文件放在 src 下（实现细节，不对外公开）。
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(backend PUBLIC Qt6::Core Qt6::Network Qt6::Sql)

# 控制台自测程序
add_executable(backend_selftest src/selftest_main.cpp)
target_link_libraries(backend_selftest PRIVATE backend)
set_target_properties(backend_selftest PROPERTIES
    WIN32_EXECUTABLE OFF          # 保持控制台程序，直接看输出
)
