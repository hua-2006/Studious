# Studious

跨平台截图翻译工具（Windows 优先）。

> **状态：翻译后端 + 界面骨架已完成；截图 / OCR / 浮层只留了接口，尚未实现。**

---

## 当前进度

### 已完成

| 能力 | 说明 |
|---|---|
| **四类翻译引擎** | Web（Google / MyMemory）、云端 LLM、本地 LLM（llama.cpp）、本地 MT（OPUS-MT） |
| **本地 MT 多语言** | 6 个语言对 + 多语言模型兜底，可离线运行，无需显卡 |
| **本地 LLM** | CPU / Vulkan 双后端，支持模型热切换 |
| **翻译缓存** | SQLite 落地，重复文本毫秒级返回 |
| **语言检测** | 14 种语言，无需外部模型 |
| **资产系统** | 清单验签、多源下载、断点续传、原子安装、删除、手动导入 |
| **桌面界面** | Qt Quick（QML）实现的五页骨架：翻译 / 快捷键 / 配置 / 设置 / 外观 |
| **主题** | 由强调色（seed）按 OKLab 混色推导整套配色，明暗两套，可持久化 |
| **健壮性** | 失败重试、目标语种校验、复读检测、输入预检 |

### 尚未实现

| 项 | 现状 |
|---|---|
| 截图抓取 / 框选交互 / 结果浮层 / 全局热键 | 只有 `frontend/src/platform/IPlatform.h` 接口与空实现 |
| OCR | 同上，接口预留 |
| 部分界面配置项 | 见下文「界面」一节 |

界面上没有接后端的控件会显示橙色「未实现」标记，方便逐项补齐。

---

## 架构

前后端分离，为兄弟目录：

```
frontend/   界面层（Qt Quick / QML + 少量 C++ 桥）
            qml/          页面与组件
            src/          AppBackend（QML 与后端的唯一桥）、主题、设置
            src/platform/ 截图 / OCR / 浮层 / 热键的接缝（目前是空实现）
backend/    翻译引擎（C++20 + Qt Core，平台无关）
            contract / router / translators / mt / asset / cache / i18n
```

**边界约定：**

- `backend/` 不接收图像、不返回坐标、不含平台相关头文件。OCR 属于前端。
- QML 只通过 `AppBackend` 访问后端，不直接引用 `studious::*` 类型。
- 平台差异（截图 / 热键 / 浮层）全部收在 `frontend/src/platform/` 后面，
  由运行时能力查询驱动界面，不用编译期 `#ifdef`。

---

## 翻译引擎

| 引擎 | 离线 | 依赖 |
|---|---|---|
| **web** | ❌ | 免 key（Google / MyMemory），Azure 仅留接口 |
| **llm（云端）** | ❌ | OpenAI 兼容 API |
| **llm（本地）** | ✅ | llama.cpp runtime + GGUF 模型 |
| **local-mt** | ✅ | OPUS-MT 语言包（约 100 MB/对） |

降级链：`web → llm → local-mt`，可配置。

本地 MT 的路径选择：直连 → 英文中转 → 多语言模型兜底。

---

## 界面

五个页面：

| 页面 | 内容 |
|---|---|
| **翻译** | 源/目标语言、引擎切换、原文对照、原文与译文双栏 |
| **快捷键** | 全局热键列表（平台层未接，键位当前留空） |
| **配置** | 引擎优先级、引擎状态面板、Web 来源、云端 LLM、本地 MT、本地 LLM |
| **设置** | 通用 / 翻译行为 / 缓存 / 网络 / 资产 / 数据 / 关于 |
| **外观** | 明暗模式、强调色、字体、字号缩放、布局密度、语言显示名 |

当前未接后端的控件带橙色「未实现」标记。清单：

```powershell
grep -rn "Stub {" frontend/qml/pages/
```

接好接口后，删掉对应那一行即可；想临时隐藏全部标记，把 `Theme.showStubs` 设为 `false`。

---

## 构建

### 依赖

- Qt 6.12+（Core / Gui / Quick / QuickControls2 / Qml / Concurrent / Network / Sql）
- MinGW GCC 13.1 或 MSVC 2022
- CMake 3.30+、Ninja

第三方库已内置（Monocypher、miniz、sentencepiece、onnxruntime），**构建过程无需联网**。

### 命令

```powershell
# 一键（自动定位 Qt，可从任意目录调用）
.\build.ps1 -Test

# 自定义 Qt 位置
.\build.ps1 -QtRoot D:\Qt -QtVer 6.12.0
```

> 运行前需把 Qt 的 `bin` 目录加入 `PATH`，否则会以 `0xC0000135`（找不到 DLL）静默退出。

### 字体

界面与译文分别使用思源黑体与霞鹜文楷。字体体积较大（约 260 MB），**不随仓库分发**；
缺失时程序自动回落到系统字体，功能不受影响。想恢复原样，把字体放到仓库根的 `font/`：

```
font/UI_OTF/SimplifiedChinese/SourceHanSansSC-*.otf    界面字体
font/centent-v1.530/LXGWWenKai-*.ttf                   译文字体
font/centent-v1.530/LXGWWenKaiMono-*.ttf               等宽字体
```

两套字体均遵循 SIL Open Font License 1.1。

---

## 使用

```powershell
.\frontend\build\studious.exe
```

模型与运行时通过「设置 → 资产」按需下载（LLM 与本地 MT 分两类，支持带进度的下载与卸载）。

### 命令行

```powershell
# 文本翻译（不开 GUI）
studious.exe --smoke "Hello, world!" --to zh --engine llm
#   可选：--from <lang> --provider <google|mymemory> --cache on|off
#         --local --local-gpu on|off --local-model <file>

# 模型目录
studious.exe --list-models

# 本地 MT 自测
backend_selftest.exe --mt          # 直连语言对
backend_selftest.exe --mt-mul      # 多语言兜底
backend_selftest.exe --mt-bench    # 性能对比

# 资产
studious.exe --assets-cli backend\asset\manifest.json
studious.exe --asset-download backend\asset\manifest.json <assetId>
```

---

## 版本信息

版本号 / 渠道名读仓库根的 `version.json`；提交号在构建期从 git 取，Qt 版本与架构运行期取。
发版只需改 `version.json`。

---

## 路线图

| 优先级 | 事项 |
|---|---|
| **P0** | 截图 → 框选 → OCR → 浮层（产品主线） |
| P1 | 补齐界面里标「未实现」的配置项 |
| P2 | Linux 平台实现（接口已预留） |
| P3 | 预翻译与缓存导入导出 |

---

## 许可

[MIT](LICENSE)

第三方组件与字体的许可证见程序内「设置 → 关于 → 第三方声明」。

## 贡献者

见 [`CONTRIBUTORS.md`](CONTRIBUTORS.md)。
