cmake_minimum_required(VERSION 3.21)

project(studious_frontend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MainWindow / TranslateJob 都是 QObject 子类，必须开 moc。
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent)

# 后端作为子目录引入，目标是静态库 backend（对外导出 include 目录 + Qt6::Core / Qt6::Network）。
# 注意：backend/CMakeLists.txt 里的 set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ...) 只作用于它自己的
# 子目录 scope，不会把我们的 studious.exe 挪进 backend/build。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../backend ${CMAKE_BINARY_DIR}/backend)

# 故意不加 WIN32：
#   WIN32 子系统会砍掉控制台，--smoke 的输出就看不见了。
#   保持控制台子系统时，GUI 依然能正常弹窗，且 smoke 输出直接可见（亲测）。
add_executable(studious
    src/main.cpp
    src/MainWindow.cpp
    src/TranslateJob.cpp
    src/AppSettings.cpp
    src/LocalEngine.cpp
    src/SettingsDialog.cpp
    src/AssetDialog.cpp
)

target_include_directories(studious PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Qt6::Concurrent 提供 QtConcurrent::run（工作线程跑翻译）。
target_link_libraries(studious PRIVATE backend Qt6::Widgets Qt6::Concurrent)

set_target_properties(studious PROPERTIES
    WIN32_EXECUTABLE OFF
)
