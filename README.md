<p align="center">
  <img src="frontend/resources/brand.png" alt="Studious" width="112">
</p>

# Studious

跨平台截图翻译工具（Windows 优先）。

> **状态：翻译后端 + 桌面界面已完成（当前版本 0.7.4）；
> 截图 / OCR / 浮层 / 全局热键只留了接口，尚未实现。**

---

## 当前进度

### 已完成

| 能力 | 说明 |
|---|---|
| **四类翻译引擎** | Web（Google / MyMemory）、云端 LLM、本地 LLM（llama.cpp）、本地 MT（OPUS-MT） |
| **引擎自动降级** | 按优先级逐个尝试，网络/超时/5xx 换下一个，语言对不支持等语义错误直接停 |
| **本地 MT 多语言** | 6 个语言对 + 多语言模型兜底，可离线运行，无需显卡 |
| **本地 LLM** | CPU / Vulkan 双后端，模型热切换，上下文 / 线程 / GPU 层数 / 附加参数可调 |
| **本地服务生命周期** | Job Object 兜底防孤儿进程、切走就停 / 启动预热 / 空闲自动停（均默认关） |
| **翻译缓存** | SQLite 落地，键含引擎 / 模型 / 采样参数指纹，换模型不串味 |
| **语言检测** | 14 种语言，无需外部模型 |
| **资产系统** | 清单验签（bundle 内）、多源下载、断点续传、原子安装、删除、按文件导入 |
| **桌面界面** | Qt Quick（QML）五页：翻译 / 快捷键 / 配置 / 设置 / 外观 |
| **引擎配置** | 分段控件分 Web / 云端 LLM / 本地 LLM / 本地 MT，带状态点，与引擎列表联动 |
| **主题** | 由强调色（seed）按 OKLab 混色推导整套配色，明暗两套，可持久化 |
| **桌面集成** | 开机自启、最小化到托盘、关闭到托盘、单实例（Windows 原生实现） |
| **数据管理** | 安装根目录迁移、按文件导入资产、清理旧日志、重置配置与缓存 |
| **健壮性** | 失败重试、目标语种校验、复读检测、输入预检、全局错误通知条 |

### 尚未实现

| 项 | 现状 |
|---|---|
| 截图抓取 / 框选交互 / 结果浮层 / 全局热键 | 只有 `frontend/src/platform/IPlatform.h` 接口与 `NullPlatform` 空实现 |
| OCR | 同上，接口预留（`recognize()` 已定好，实现留空） |
| 外观页 6 项、快捷键页热键表 | 界面在，未接后端（橙色「未实现」标记） |

界面上没有接后端的控件会显示橙色「未实现」标记，方便逐项补齐。

---

## 架构

前后端分离，为兄弟目录：

```
frontend/   界面层（Qt Quick / QML + 少量 C++ 桥）
            qml/          页面与组件
            src/          AppBackend（QML 与后端的唯一桥）、主题、设置
            src/platform/ 截图 / OCR / 浮层 / 热键的接缝（尚未实现）
                          Desktop 已实现：开机自启 / 托盘 / 单实例 / 关闭行为
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

降级链：`本地 LLM → 本地 MT → 云端 LLM → Web`（默认顺序，可在「配置 → 引擎」里拖动改）。

本地 MT 的路径选择：直连 → 英文中转 → 多语言模型兜底。

---

## 界面

五个页面，默认 1120×720 逻辑像素（1680×1080 @1.5x 缩放），最小 1040×660：

| 页面 | 内容 |
|---|---|
| **翻译** | 源/目标语言、引擎切换、引擎状态条、原文对照、原文与译文双栏、错误通知条 |
| **快捷键** | 全局热键列表（平台层未接，键位当前留空） |
| **配置** | 引擎优先级与强制引擎、四类引擎的分段式配置（Web / 云端 LLM / 本地 LLM / 本地 MT）、高级设置折叠区 |
| **设置** | 通用 / 翻译行为 / 缓存 / 网络 / 资产 / 数据 / 关于 |
| **外观** | 明暗模式、强调色、字体、字号缩放、布局密度、语言显示名 |

未接后端的控件带橙色「未实现」标记，目前只剩：

```powershell
grep -rn "Stub {" frontend/qml/pages/
#   AppearancePage.qml  6 处（字体选择、布局密度、状态栏信息密度、语言显示名）
#   HotkeysPage.qml     1 处（整页，等平台层热键）
```

接好接口后删掉对应那一行即可；想临时隐藏全部标记，把 `Theme.showStubs` 设为 `false`。

界面文案双语（中/英），可在「设置 → 通用 → 界面语言」切换，切完点「重启软件」生效。

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
发版只需改 `version.json`，再打一个 `vX.Y.Z` 标签。

当前：`0.7.4`（beta）。

---

## 路线图

三步主线：**小窗翻译 → 截图翻译 → 实时翻译**。前两步共用同一套平台层与 OCR，
所以顺序不能倒过来做。

| 阶段 | 事项 | 依赖 |
|---|---|---|
| **M1** | 平台层落地：整屏/区域抓图、交互式框选遮罩、结果浮层、全局热键、多屏与高 DPI | 无（`IPlatform` 接口已定） |
| **M2** | 小窗翻译：置顶浮动小窗、热键唤起、回车翻译、失焦/ESC 收起 | M1（热键） |
| **M3** | OCR：截图 → 文本，中英为主要目标，返回词级坐标供浮层对齐 | 资产系统（模型按需下载） |
| **M4** | 截图翻译：热键框选 → OCR → 小窗出译文，可复制、可重识别 | M2 + M3 |
| **M5** | 实时翻译：锁定屏幕区域，按增量做 OCR + 翻译，浮层贴边显示 | M4 |
| P1 | 外观页剩余 6 项、快捷键页热键自定义 | M1 |
| P2 | Linux 平台实现（X11 抓图 / xdg-desktop-portal） | M1 |
| P3 | 预翻译与缓存导入导出 | — |

---

## 许可

[MIT](LICENSE)

第三方组件与字体的许可证见程序内「设置 → 关于 → 第三方声明」。

## 贡献者

见 [`CONTRIBUTORS.md`](CONTRIBUTORS.md)。
