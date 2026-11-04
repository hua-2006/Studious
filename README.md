# Studious

跨平台截图翻译工具（Windows 优先）。

> **状态：翻译核心已完成，界面层（截图 / OCR / 浮层）尚未开始。**

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
| **健壮性** | 失败重试、目标语种校验、复读检测、输入预检 |

### 未开始

- 截图抓取 / 框选交互
- OCR
- 结果浮层覆盖

---

## 架构

前后端分离，为兄弟目录：

```
frontend/   界面层（Qt Widgets）
            后续承担：截图 / 选区 / 浮层 / 热键 / OCR
backend/    翻译引擎（C++20 + Qt Core，平台无关）
            contract / router / translators / mt / asset / cache / i18n
```

**边界**：`backend/` 不接收图像、不返回坐标、不含平台相关头文件。OCR 属于前端。

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

## 构建

### 依赖

- Qt 6.12+（Core / Network / Sql / Widgets / Concurrent）
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

---

## 使用

```powershell
.\frontend\build\studious.exe
```

界面上可选择源/目标语言、引擎、Web 来源，以及启用本地引擎 / 缓存。

模型与运行时通过「资产」页按需下载。

### 命令行

```powershell
# 文本翻译
studious.exe --smoke "Hello, world!" --to zh --engine llm
#   可选：--from <lang> --provider <google|mymemory> --cache on|off
#         --local --local-gpu on|off --local-model <file>

# 本地 MT 自测
backend_selftest.exe --mt          # 直连语言对
backend_selftest.exe --mt-mul      # 多语言兜底
backend_selftest.exe --mt-bench    # 性能对比

# 资产
studious.exe --assets-cli backend\asset\manifest.json
studious.exe --asset-download backend\asset\manifest.json <assetId>
```

---

## 路线图

| 优先级 | 事项 |
|---|---|
| **P0** | 截图 → 框选 → OCR → 浮层（产品主线） |
| P1 | 界面完整多语言 |
| P2 | 失败原因分级提示 |
| P3 | 预翻译与缓存导入导出 |

---

## 许可

待定（倾向 Apache-2.0）。

## 贡献者

见 [`CONTRIBUTORS.md`](CONTRIBUTORS.md)。
