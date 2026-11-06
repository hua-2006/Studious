cmake_minimum_required(VERSION 3.21)

project(studious_frontend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt Quick 渲染栈。不再需要 Widgets：界面全部由 QML 描述。
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Qml Concurrent LinguistTools)

qt_standard_project_setup(REQUIRES 6.5)

# 后端作为子目录引入（静态库 backend，导出 include + Qt6::Core/Network/Sql）。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../backend ${CMAKE_BINARY_DIR}/backend)

# 平台抽象层（截图 / OCR / 浮层 / 热键的接缝，实现仍是空桩）。
add_library(studious_platform STATIC
    src/platform/Desktop.cpp
    src/platform/Desktop.h
    src/platform/NullPlatform.cpp
    src/platform/PlatformFactory.cpp
    src/platform/ProcessGuard.cpp
    src/platform/ProcessGuard.h
)
target_include_directories(studious_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Network：Desktop 的单实例走 QLocalServer/QLocalSocket（托盘与自启是纯 Win32，不需要额外依赖）。
target_link_libraries(studious_platform PUBLIC Qt6::Core Qt6::Gui Qt6::Network)

# Windows 可执行文件图标（资源管理器 / 任务栏 / Alt-Tab 用）。
# 走 .rc 而不是 WIN32_EXECUTABLE：本项目保留控制台子系统，
# 否则 --smoke / --assets-cli 这些命令行模式的 stdout 就没了。
if(WIN32)
    set(STUDIOUS_WIN_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc")
else()
    set(STUDIOUS_WIN_ICON "")
endif()

qt_add_executable(studious
    src/main.cpp
    src/AppBackend.cpp
    src/AppBackend.h
    src/AppSettings.cpp
    src/AppSettings.h
    src/LocalEngine.cpp
    src/LocalEngine.h
    src/TranslateJob.cpp
    src/TranslateJob.h
    src/AssetFormat.h
    src/i18n/Strings.cpp
    src/i18n/Strings.h
    ${STUDIOUS_WIN_ICON}
)

# QML 模块：URI = Studious，页面与组件全部注册进去，运行期用 import Studious 1.0。
#
# Theme.qml 必须显式标成 singleton：qmldir 生成器没有从 `pragma Singleton`
# 自动推断出来（生成的是 `Theme 1.0` 而不是 `singleton Theme 1.0`），
# 结果所有 Theme.xxx 绑定静默失败、布局全塌。
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(studious
    URI Studious
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/components/SectionCard.qml
        qml/components/FieldRow.qml
        qml/components/AppChip.qml
        qml/components/Segmented.qml
        qml/components/AppToggle.qml
        qml/components/Badge.qml
        qml/components/StatusDot.qml
        qml/components/AppButton.qml
        qml/components/AppTextField.qml
        qml/components/AppComboBox.qml
        qml/components/Stub.qml
        qml/components/NoticeBar.qml
        qml/pages/TranslatePage.qml
        qml/pages/HotkeysPage.qml
        qml/pages/ConfigPage.qml
        qml/pages/SettingsPage.qml
        qml/pages/AppearancePage.qml
    RESOURCES
        resources/brand.png
        resources/icons/brand.svg
        resources/icons/translate.svg
        resources/icons/keyboard.svg
        resources/icons/tune.svg
        resources/icons/settings.svg
        resources/icons/palette.svg
        resources/icons/database.svg
        resources/icons/globe.svg
        resources/icons/download.svg
        resources/icons/folder.svg
        resources/icons/info.svg
        resources/icons/mini.svg
        resources/icons/screenshot.svg
        resources/icons/realtime.svg
)

target_include_directories(studious PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 提交号在配置期从 git 取（版本号/渠道名读仓库根的 version.json，不在这里写死）。
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
    OUTPUT_VARIABLE STUDIOUS_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT STUDIOUS_GIT_COMMIT)
    set(STUDIOUS_GIT_COMMIT "unknown")
endif()

# 字体目录：仓库根的 font/（思源黑体做界面、霞鹜文楷做译文）。
# 直接指源码目录，不往输出目录拷 —— 两套字体加起来 200 MB，拷贝纯属浪费。
target_compile_definitions(studious PRIVATE
    STUDIOUS_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../font"
    STUDIOUS_COMMIT="${STUDIOUS_GIT_COMMIT}"
)

target_link_libraries(studious PRIVATE
    backend
    studious_platform
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Concurrent
    Qt6::Qml
)

set_target_properties(studious PROPERTIES
    WIN32_EXECUTABLE OFF
)

# 本地 MT（OPUS-MT）运行期需要 onnxruntime.dll 与 exe 同目录。
if(WIN32)
    set(ORT_DLL "${CMAKE_CURRENT_SOURCE_DIR}/../backend/third_party/onnxruntime/lib/onnxruntime.dll")
    if (EXISTS "${ORT_DLL}")
        add_custom_command(TARGET studious POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ORT_DLL}" "$<TARGET_FILE_DIR:studious>/onnxruntime.dll"
            COMMENT "Copying onnxruntime.dll next to studious.exe (needed by local MT)"
        )
    else()
        message(WARNING "onnxruntime.dll not found at ${ORT_DLL}; local MT will fail at runtime")
    endif()
endif()

# ---------------------------------------------------------------------------
# 界面文案翻译
#
# 源码里的字符串**就是中文原文**（qsTr("源语言")），所以：
#   · studious_zh_CN.qm 是恒等映射，只为占位与一致性；
#   · 真正需要维护的是 studious_en.ts —— 每条中文原文对应一条英文译文。
#
# 新增控件的规矩：QML 里所有面向用户的文字一律写 qsTr("中文")。
# 之后跑 `cmake --build build --target update_translations`（lupdate）
# 把新条目补进 .ts，再补英文。
# ---------------------------------------------------------------------------
set(STUDIOUS_TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/studious_zh_CN.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/studious_en.ts
)

qt_add_lupdate(studious TS_FILES ${STUDIOUS_TS_FILES}
                # 后端也有面向用户的文案（语言名），跟界面一起抽，别让前端各存一份映射表。
                SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../backend/src/contract/LanguageRegistry.cpp
                        # 平台层文案（托盘菜单 / 开机自启报错）走 QCoreApplication::translate。
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/Desktop.cpp)
qt_add_lrelease(studious TS_FILES ${STUDIOUS_TS_FILES}
                QM_FILES_OUTPUT_VARIABLE STUDIOUS_QM_FILES)
qt_add_resources(studious "translations"
                 PREFIX "/i18n"
                 BASE "${CMAKE_CURRENT_BINARY_DIR}"
                 FILES ${STUDIOUS_QM_FILES})

# 资产清单编译进二进制（:  /assets/manifest.json）。
#
# 为什么不是运行时读 exe 旁边的 manifest.json：那个目录对所有本进程的用户都可写，
# 能改清单的人就能把下载 URL 与 sha256 一起换掉，让客户端下载并执行任意
# llama-server.exe —— 这是代码执行级别的口子。编译进 qrc 之后，改清单等于改 exe。
# 开发期想反复调清单时设 STUDIOUS_DEV_MANIFEST=1 才回退到磁盘那份（见 AppBackend）。
qt_add_resources(studious "asset_manifest"
                 PREFIX "/assets"
                 BASE "${CMAKE_CURRENT_SOURCE_DIR}/../backend/asset"
                 FILES "${CMAKE_CURRENT_SOURCE_DIR}/../backend/asset/manifest.json")
