# Studious

跨平台截图翻译工具（Windows 优先，Linux 预留）。

> **当前阶段：v0.6 —— 翻译链路完整可用（Web / 云端 LLM / 本地 LLM / 本地 MT + 多语言兜底），前端为可测试 demo。**
> 截图、OCR、浮层尚未开始。
>
> 📄 进度报告：[`docs/progress-report.md`](docs/progress-report.md) ｜ 测试报告：[`docs/test-report-2026-11-05.md`](docs/test-report-2026-11-05.md) ｜ **接手先读**：[`docs/HANDOFF.md`](docs/HANDOFF.md)

---

## 目录

- [这是什么](#这是什么)
- [架构](#架构)
- [功能矩阵](#功能矩阵)
- [实测性能](#实测性能)
- [目录结构](#目录结构)
- [构建](#构建)
- [运行](#运行)
- [命令行模式](#命令行模式)
- [资产系统](#资产系统)
- [配置项](#配置项)
- [文档](#文档)
- [路线图](#路线图)

---

## 这是什么

一个**面向游戏/软件界面文本**的翻译工具：截取屏幕区域 → OCR 识别文字 → 翻译 → 覆盖回原位。

**当前已完成的是"中间那一段"**：拿到文字之后的翻译链路。

| 已有 | 没有 |
|---|---|
| 四类翻译引擎 + 降级链 | ❌ 截图 |
| **本地 MT（OPUS-MT，6 个语言对 + 多语言兜底）** | ❌ 框选 |
| 本地 llama.cpp 推理（CPU / Vulkan） | ❌ OCR |
| 模型热切换（0.5B ↔ 1.5B） | ❌ 结果浮层 |
| 翻译缓存（重复文本 9ms） | ❌ 前端界面完整 i18n |
| 语言检测（14 种语言） | |
| 资产下载 / 校验 / 安装 / 删除 / 手动导入 | |
| 清单 ed25519 验签 | |
| 失败重试（最多 5 次）+ 语种校验 | |
| 后端错误提示中英双语 | |

---

## 架构

前后端严格分离，为兄弟目录：

```
┌──────────────────────────────────────────────┐
│  frontend/   Qt Widgets（当前是测试 UI）       │
│  后续：截图 / 选区 / 浮层 / 热键 / OCR          │
└───────────────────┬──────────────────────────┘
                    │ 只传纯文本 + 语言对
                    ▼
┌──────────────────────────────────────────────┐
│  backend/    纯翻译引擎（平台无关，C++20+Qt Core）│
│                                              │
│  TranslatorRouter ── 选引擎 / 降级 / 缓存      │
│    ├─ web         Google · MyMemory · Azure(占位)│
│    ├─ llm         云端 OpenAI 兼容（可换模型）  │
│    ├─ mock        离线自测                     │
│    └─ local-mt    桩（v2）                    │
│                                              │
│  asset/   清单验签 · 下载 · 安装 · 删除         │
│  cache/   翻译记忆（SQLite）                   │
│  i18n/    中英双语错误提示                     │
└──────────────────────────────────────────────┘
```

**边界铁律**：`backend/` 内不得出现 `windows.h` / `X11` 等平台头文件（CI 可 grep 校验），不接收图像、不返回坐标。

详见 [`docs/backend-architecture.md`](docs/backend-architecture.md)。

---

## 功能矩阵

### 翻译引擎

| 引擎 | 状态 | 质量 | 延迟 | 依赖 |
|---|---|---|---|---|
| **local-mt（本地 MT）** | ✅ | 中 | **237~751 ms** | 下载语言包（约 100 MB/对） |
| **llm（本地）** | ✅ | 好 | **102ms**（Vulkan 0.5B） | 需下载 runtime + 模型 |
| **llm（云端）** | ✅ | 最好 | 0.9~1.2s | API Key |
| **web / Google** | ✅ | 中 | ~1s | 免费，国内常需代理 |
| **web / MyMemory** | ✅ | 中 | ~1.3s | 免费兜底 |
| **web / Azure** | 🚧 仅接口 | — | — | 需 Azure 订阅 Key |
| **mock** | ✅ | — | 4ms | 离线自测 |

降级链：`mock → web → llm → local-mt`（可配置）。

### 后端能力

| 能力 | 状态 | 说明 |
|---|---|---|
| 三引擎统一接口 | ✅ | `ITranslator` + `resolve()` 资产就绪检查 |
| 翻译缓存 | ✅ | SQLite，键含引擎标签；重复文本 0ms |
| 批量合并 | ✅ | 多段合成 1 次请求，5 段 6.4s → 0.9s |
| 思考等级控制 | ✅ | 默认关闭，实测 `reasoning_tokens=0` |
| 模型目录拉取 | ✅ | `GET /models`，21 个聊天模型 |
| 资产清单 + ed25519 验签 | ✅ | 8 条结构校验，篡改必拒 |
| 多源下载 + 断点续传 | ✅ | Range / 206/200/416 三分支 |
| zip 安全解压 | ✅ | zip-slip 三道闸 |
| 安装 / 删除 / 手动导入 | ✅ | 删除幂等，损坏可检测 |
| 中英双语错误提示 | ✅ | 跟随系统语言，可覆盖 |

---

## 实测性能

| 场景 | 耗时 | 备注 |
|---|---|---|
| 本地 0.5B + **Vulkan** | **102 ms** | 预热后 47~71 ms |
| 本地 0.5B + CPU | 450~563 ms | 6 线程 |
| 本地 1.5B + CPU | 1,526 ms | 质量更好 |
| 云端 LLM 单段 | **1.0~1.2s** | 上游固定首字延迟 |
| 云端 LLM 5 段批量 | **0.9s** | 7 倍提速 |
| 命中缓存 | **9ms** | 跨进程复用 |
| 同语言直返 | **1ms** | 不调引擎 |
| 网关纯网络开销 | 6~62ms | `GET /models` |

> 延迟剖析：token 从 32 涨到 69、输出 4→5，耗时几乎不变 → 瓶颈是**每次请求固定的上游首字延迟**。所以优化方向是「减少请求次数」（批量）和「完全不请求」（缓存、同语言直返）。

### 健壮性

小模型会犯的错，代码层都拦住了：

| 失败模式 | 处理 |
|---|---|
| 输出 = 原文（没翻） | 检测 → 重试 → 报错 |
| 复读刷屏 | 输出长度按输入封顶 + 截断 |
| 目标语言不符（如中文目标吐意大利语） | 字符集校验 → 重试 |
| 碎片化输出 | 单字符片段占比检测 |
| 纯数字/符号输入 | 预检直接返回，不发请求 |
| 源语言选错 | **黄字提示**（不阻断，LLM 能自纠） |
| 源 = 目标 | 原样返回 + 提示 |

重试策略：**最多 5 次，每次换一种 prompt**（默认 → 加禁止照抄 → 纯英文指令 → 极简 → 高温度），总时长预算 30 秒。

---

## 目录结构

```
Studious/
├─ backend/                      纯翻译引擎（46 文件 / 6.4k 行）
│  ├─ include/studious/
│  │  ├─ contract/               ITranslator / Types / LanguageRegistry
│  │  ├─ net/                    IHttpClient 端口 + QtHttpClient（含流式）
│  │  ├─ translators/            web / mock / llm / local-mt
│  │  ├─ router/                 TranslatorRouter
│  │  ├─ cache/                  TranslationCache
│  │  ├─ asset/                  Manifest / AssetRegistry / Downloader / TrustKeys
│  │  ├─ llm/                    LlmCatalog（模型目录 + 能力探测）
│  │  ├─ i18n/                   L10n（中英双语）
│  │  ├─ config/                 LlmConfig
│  │  └─ Factory.h               组件装配入口
│  ├─ src/                       实现
│  ├─ tools/manifest_tool.cpp    genkey / sign / verify（发布用）
│  ├─ asset/manifest.json        资产清单示例（占位哈希）
│  └─ third_party/               monocypher 4.0.2 · miniz 3.0.2（均已 vendor）
│
├─ frontend/                     Qt Widgets 测试 UI（12 文件 / 2.4k 行）
│  └─ src/                       MainWindow / SettingsDialog / AssetDialog / TranslateJob
│
├─ docs/backend-architecture.md  架构文档（20 章 + ADR + 踩坑）
├─ build.ps1                     一键构建
└─ README.md
```

---

## 构建

### 依赖

| 组件 | 版本 | 说明 |
|---|---|---|
| Qt | **6.12.0**（mingw_64） | 只用 Core / Network / Sql / Widgets / Concurrent |
| MinGW | GCC 13.1.0 | 用 Qt 自带那套，**不要**混用系统里的其它 GCC |
| CMake | 3.30+ | |
| Ninja | 1.12+ | |

第三方库已 vendor 进仓库（monocypher、miniz），**构建过程零网络**。

### 一键构建

```powershell
cd C:\Code\code\project\Studious
.\build.ps1 -Test
```

脚本会自动定位 Qt 并设置 PATH，可从任意目录调用。自定义 Qt 位置：

```powershell
.\build.ps1 -QtRoot D:\Qt -QtVer 6.12.0
```

### 手动构建

```powershell
$env:Path = "C:\Code\language\Qt\Tools\mingw1310_64\bin;C:\Code\language\Qt\Tools\CMake_64\bin;C:\Code\language\Qt\Tools\Ninja;C:\Code\language\Qt\6.12.0\mingw_64\bin;" + $env:Path

cmake -S backend -B backend/build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/Code/language/Qt/6.12.0/mingw_64" `
  -DCMAKE_CXX_COMPILER="C:/Code/language/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build backend/build

cmake -S frontend -B frontend/build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/Code/language/Qt/6.12.0/mingw_64" `
  -DCMAKE_CXX_COMPILER="C:/Code/language/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build frontend/build
```

> ⚠️ 运行前必须把 `Qt\6.12.0\mingw_64\bin` 加进 PATH，否则报 `0xC0000135`（DLL_NOT_FOUND）且**没有任何输出**。

---

## 运行

```powershell
.\frontend\build\studious.exe
```

界面元素：

| 位置 | 内容 |
|---|---|
| 第一行 | 源语言 / 目标语言 / 引擎 / 显示原文对照 / 翻译（Ctrl+Enter） |
| 第二行 | Web 来源下拉 · **启用缓存** · **资产…**（Ctrl+Alt+A） · **设置…** |
| 中间 | 左原文 / 右译文（可勾选对照显示） |
| 底部 | 引擎 / 耗时 / 段落数 / 缓存命中 · 各引擎就绪状态 |

设置页：LLM 地址 / Key / 模型 / **获取模型** / 思考等级 / 超时 / Temperature / System Prompt / 界面语言 / 缓存清空。

---

## 命令行模式

### 前端

```powershell
# 文本翻译冒烟
.\frontend\build\studious.exe --smoke "Hello, world!" --to zh --engine llm
#   --engine  auto|mock|web|llm|local-mt
#   --provider auto|google|azure|mymemory
#   --cache on|off
#   --from <lang>                     源语言（默认 auto，走语言检测）
#   --llm-base / --llm-key / --llm-model

# 本地引擎
.\frontend\build\studious.exe --smoke "Hello, world!" --to zh --engine llm --local
#   --local-gpu on|off                默认 on（Vulkan）
#   --local-model <文件名>            如 model.qwen2.5-1.5b-instruct.q4km
#   --switch-demo                     同一进程内逐个模型切换（验证热插拔）
#   --repeat <n>                      同一句跑 n 次（验证服务复用）

# 拉取网关模型列表
.\frontend\build\studious.exe --list-models

# 资产清单状态 / 真实下载
.\frontend\build\studious.exe --assets-cli backend\asset\manifest.json
.\frontend\build\studious.exe --asset-download backend\asset\manifest.json model.qwen2.5-0.5b-instruct.q4km
```

> 本地引擎需要代理时可设环境变量：`HTTPS_PROXY=http://127.0.0.1:7897`（下载器会读取）。

### 后端自测

```powershell
.\backend\build\backend_selftest.exe              # 全量（含联网）
.\backend\build\backend_selftest.exe --offline    # 离线核心用例
.\backend\build\backend_selftest.exe --providers  # Web 来源表
.\backend\build\backend_selftest.exe --llm        # 真实 LLM 翻译
.\backend\build\backend_selftest.exe --llm-batch  # 批量 vs 逐段对比
.\backend\build\backend_selftest.exe --models     # 模型目录 + 思考等级探测
.\backend\build\backend_selftest.exe --cache      # 缓存命中/失效
.\backend\build\backend_selftest.exe --manifest   # 清单验签（11 用例）
.\backend\build\backend_selftest.exe --i18n       # 中英双语
.\backend\build\backend_selftest.exe --assets     # 下载/续传/取消/zip-slip/删除（13 用例）
.\backend\build\backend_selftest.exe --mt         # 本地 MT 4 个直连语言对
.\backend\build\backend_selftest.exe --mt-mul     # 多语言模型兜底（5 正向 + 2 拒译）
.\backend\build\backend_selftest.exe --mt-bench   # KV cache / beam 对比
.\backend\build\backend_selftest.exe --mt-tok     # 分词诊断
```

---

## 资产系统

模型与推理引擎不随安装包分发，**按需下载**。

```
清单（ed25519 签名） → 多源下载 → sha256 校验 → 原子安装 → 可随时删除
```

| 环节 | 实现要点 |
|---|---|
| 信任链 | 公钥编译进二进制 → 验签清单 → 清单内写死每个资产的 sha256 |
| 多源 | `urls` 数组按序尝试，失败自动切下一个 |
| 续传 | `Range` + 206/200/416 三分支，`.part` 保留 |
| 校验 | 分块 sha256，不符即删 |
| 解压 | zip-slip 三道闸（`..` / 绝对路径 / 符号链接） |
| 安装 | `<target>.tmp` → `rename`，断电不留半残 |
| 删除 | 幂等；缺关键文件时状态为 `Corrupted` |
| 手动导入 | 拖入 zip / gguf，走同一套校验与落盘 |

**发布工具**：

```powershell
.\backend\build\manifest_tool.exe genkey
.\backend\build\manifest_tool.exe sign payload.json <seedHex> out.json
.\backend\build\manifest_tool.exe verify out.json <pubkeyHex>
```

⚠️ 私钥（seed）**绝不入库**；当前 `TrustKeys.h` 里是开发用公钥，**发版前必须重新生成**。

⚠️ 仓库里的 `backend/asset/manifest.json` 中 sha256 与 URL 仍是占位，点「下载」会得到明确的本地化拒绝——这是预期行为。

---

## 配置项

持久化在 `QSettings("Studious","Studious")`，Windows 下落 `HKCU\Software\Studious\Studious`。

| 键 | 含义 |
|---|---|
| `settings/llm/{baseUrl,apiKey,model}` | LLM 连接 |
| `settings/llm/{timeoutMs,maxTokens,temperature,systemPrompt}` | 采样参数 |
| `settings/llm/reasoningEffort` | 思考等级（`none`/`minimal`/`low`/`medium`/`high`/空=默认） |
| `settings/web/providerId` | Web 来源（空=自动） |
| `settings/engine/id` | 引擎（空=自动） |
| `settings/lang/{src,dst}` | 语言对 |
| `settings/cache/enabled` | 翻译缓存开关 |
| `settings/ui/language` | 界面语言（空=跟随系统） |

运行时目录：

| 内容 | 路径 |
|---|---|
| 翻译缓存库 | `%APPDATA%\studious\cache.db` |
| 资产安装根 | `%APPDATA%\studious\assets\{runtime,models,downloads}` |

---

## 文档

- [`docs/HANDOFF.md`](docs/HANDOFF.md) —— **交接文档**（下一个接手的人先读这个）
- [`docs/test-report-2026-11-05.md`](docs/test-report-2026-11-05.md) —— 后端接口测试报告（63 条断言）
- [`docs/progress-report.md`](docs/progress-report.md) —— 进度报告（当前状态、实测数据、问题清单）
- [`docs/backend-architecture.md`](docs/backend-architecture.md) —— 架构、契约、20 条 ADR、全部踩坑记录
- [`backend/README.md`](backend/README.md) —— 后端模块与自测说明
- [`frontend/README.md`](frontend/README.md) —— 前端界面、线程模型、冒烟参数
- [`CONTRIBUTORS.md`](CONTRIBUTORS.md)

---

## 路线图

| 优先级 | 事项 | 说明 |
|---|---|---|
| **P0** | **截图链路** | 热键 → 抓屏 → 框选 → OCR → 翻译 → 浮层（产品主线） |
| P1 | 前端界面完整 i18n | 目前只有资产对话框双语 |
| P1 | MT 的 UX 补丁 | 显示「经英文中转」、失败时给「去下载」按钮 |
| P2 | 失败原因分级展示 | 区分「换模型 / 换网络 / 内容不可译」 |
| P3 | 预翻译 + 缓存导入导出 | galgame 感知延迟趋零 |
| P4 | 抽 ONNX control-flow 子图 | 吃满 KV cache 收益（需 python onnx） |

---

## 许可

待定（倾向 Apache-2.0）。

## 贡献者

见 [`CONTRIBUTORS.md`](CONTRIBUTORS.md)。
