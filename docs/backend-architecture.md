# Studious 后端架构设计（讨论稿 v0.1）

> 状态：架构共识已达成，尚未编码。
> 范围：**仅后端**。前端（Qt 截图 / 选区 / 浮层 / 热键 / OCR）另行设计。

---

## 1. 定位与边界

后端是一个**纯粹的翻译引擎**，不碰图像、不碰坐标、不碰渲染。

| 归属 | 内容 |
|---|---|
| **前端**（Qt，平台相关） | 截图、选区交互、OCR、结果浮层、全局热键 |
| **后端**（本文件，平台无关） | 翻译调度、翻译组件、资产包管理、缓存 |

**边界铁律**：
- `backend/` 下不得出现任何平台相关头文件（`windows.h` / `X11/*`）。
- 后端**不接收图像**，只接收文字。
- 后端**不返回坐标**，只返回文本。

**这样切分的理由**：
1. OCR 本质是平台能力（Win 用 WinRT OcrEngine、Linux 用 RapidOCR），天然属于平台层。
2. 后端保持纯粹后可被复用（CLI、浏览器插件、未来移动端）。
3. 数据契约最干净：`string[] → string[]`。

---

## 2. 分层架构

```
┌──────────────────────────────────────────────────────────────┐
│  前端平台层（Windows 已实现；Linux 仅预留接口）                 │
│    截图 / 选区 / 浮层 / 热键 / OCR                            │
└──────────────────────┬───────────────────────────────────────┘
                       │ 输入：段落数组 + 语言对
                       ▼
┌──────────────────────────────────────────────────────────────┐
│  后端（平台无关，一份代码）                                    │
│                                                              │
│   TranslatorRouter ── 选组件 / 降级链 / 按目标语言拆分         │
│        │                                                     │
│        ├── WebTranslator        （默认，零配置，无需资产）      │
│        ├── LocalMtTranslator    （离线，依赖外部资产包）        │
│        └── LlmTranslator        （高质量，依赖外部资产包）      │
│                                                              │
│   LanguageRegistry ── 内部语言码 ⇄ 各组件语言码                │
│   TranslationCache ── 原文 hash → 译文（SQLite）              │
│                                                              │
│   asset/  AssetRegistry · Downloader · LocalInstaller        │
│   runtime/ LlmRuntimeManager（子进程拉起 llama-server）        │
└──────────────────────┬───────────────────────────────────────┘
                       │ 输出：段落数组
                       ▼
                  前端浮层渲染
```

---

## 3. 后端模块划分

```
backend/
├─ contract/
│   ├─ ITranslator.h            # 三组件统一接口
│   ├─ Types.h                  # TranslateRequest / TranslateResponse / ResolveResult
│   └─ LanguageRegistry.{h,cpp} # 内部语言码与各组件语言码映射
├─ router/
│   └─ TranslatorRouter.{h,cpp} # 选组件、降级、按语言拆分
├─ translators/
│   ├─ web/
│   │   ├─ WebTranslator.{h,cpp}
│   │   └─ adapters/{DeepL,GoogleFree,Baidu,...}
│   ├─ local/
│   │   └─ LocalMtTranslator.{h,cpp}
│   └─ llm/
│       ├─ LlmTranslator.{h,cpp}
│       ├─ OpenAICompatClient.{h,cpp}  # HTTP + SSE 流式
│       ├─ PromptBuilder.{h,cpp}       # 上下文窗口 + 术语表注入
│       ├─ ResponseParser.{h,cpp}      # JSON 提取 + 校验
│       └─ Chunker.{h,cpp}             # 长文本切块
├─ runtime/
│   ├─ LlmRuntimeManager.{h,cpp}       # 探测外部 Ollama / 拉起内置 llama-server
│   ├─ BackendDetector.{h,cpp}         # CPU 指令集 / Vulkan 可用性探测
│   └─ ServerLauncher.{h,cpp}          # 子进程生命周期 + 端口就绪等待
├─ asset/
│   ├─ AssetRegistry.{h,cpp}           # manifest 拉取 + ed25519 验签 + 本地状态
│   ├─ AssetResolver.{h,cpp}           # 给定 (src,dst) 算出缺失资产
│   ├─ Downloader.{h,cpp}              # 多源 + Range 续传 + sha256 + 原子落盘
│   ├─ LocalInstaller.{h,cpp}          # 手动导入本地文件
│   └─ manifest.json                   # 资产清单（自身经 ed25519 签名）
├─ cache/
│   └─ TranslationCache.{h,cpp}
└─ config/
    └─ ConfigStore.{h,cpp}
```

---

## 4. 核心接口契约

### 4.1 数据契约

```cpp
struct TranslateRequest {
    QStringList paragraphs;   // 每项一个段落（前端已按行合并成段）
    QString     srcLang;      // "auto" 或内部语言码
    QStringList dstLangs;     // 复数：为多目标预留，v1 UI 只暴露单选
    QString     preferredEngine; // 可选："web" | "local-mt" | "llm"
    QJsonObject options;      // 温度、术语表、语气等
};

struct TranslateResponse {
    bool ok;
    QHash<QString, QStringList> translated;  // dstLang -> 段落译文
    QString engineUsed;
    QString error;            // 失败原因，供 UI 提示与降级说明
};
```

**为什么 `dstLangs` 从第一天就是复数**：接口一旦定成单数，后续改多目标要改动所有组件与 Router。**接口按复数设计，v1 UI 只暴露单选**——这是最省成本的路径。

### 4.2 组件统一接口

```cpp
class ITranslator {
public:
    virtual ~ITranslator() = default;

    virtual QString id() const = 0;               // "web" | "local-mt" | "llm"

    // 是否可立即使用（服务在跑 / key 已配 / 模型已装）
    virtual bool isReady() const = 0;

    // 资产就绪检查：缺什么告诉前端（asset 系统的接入点）
    virtual ResolveResult resolve(const QString& src, const QString& dst) const = 0;

    // 能力声明
    virtual QStringList supportedTargetLangs() const = 0;
    virtual bool supports(const QString& src, const QString& dst) const = 0;

    // 可选能力：不支持返回空
    virtual QString detectLanguage(const QStringList& texts) const { return {}; }

    virtual QVector<QStringList> translate(
        const QStringList& paragraphs,
        const QString& srcLang,
        const QStringList& dstLangs,
        const QJsonObject& options) = 0;

    virtual void cancel() = 0;
};

struct ResolveResult {
    bool ready;
    QVector<QString> missingAssets;  // UI 展示"需下载 X (1.9GB)"
    QString reason;                  // "缺模型" / "Ollama 未启动" / "硬件不支持"
};
```

`web` 恒返回 `ready=true`（无资产依赖）；`local-mt` / `llm` 查询 `AssetRegistry`。

### 4.3 语言码体系

内部统一使用**简化 BCP-47**：

| 内部码 | 含义 |
|---|---|
| `zh` | 简体中文 |
| `zh-TW` | 繁体中文 |
| `en` | 英文 |
| `ja` | 日文 |
| `pt-BR` / `pt-PT` | 巴西 / 欧洲葡语 |

各组件语言码映射（差异极大，必须集中管理）：

| 内部码 | Google | DeepL | NLLB | LLM |
|---|---|---|---|---|
| `zh` | `zh-CN` | `ZH-HANS` | `zho_Hans` | `中文` |
| `en` | `en` | `EN` | `eng_Latn` | `English` |
| `ja` | `ja` | `JA` | `jpn_Jpan` | `日本語` |

`LanguageRegistry` 提供双向映射 + 各语言的中文名/原生名（供下拉框显示）。
**这是后端必须最先完成的模块，优先级高于任何翻译逻辑。**

---

## 5. 三个翻译组件

### 5.1 WebTranslator（默认，零配置）

- **多 adapter 可插拔**，统一注册表管理。
- **默认策略**：无用户 key 时走免费源；有 key 时优先用户 key。
- **语言策略**：`dsts` 里某个目标语言是当前 adapter 不支持的，触发切换而不是报错。
- **各家能力差异**（需联网核实，见 §11）：源语言 `auto` 支持情况、语言码写法、是否支持单请求多目标（基本都不支持 → 内部循环）。

| adapter | 需要 key | 多目标 | 备注 |
|---|---|---|---|
| Google 免费接口 | 否 | 循环 | 语言覆盖最广，稳定性无保障 |
| DeepL | 是（免费额度） | 循环 | 质量最好 |
| 百度 | 是 | 循环 | 国内最快 |

### 5.2 LocalMtTranslator（离线，精度要求低）

**定位：离线/断网时的快速保底，不追求精度。**

- **模型形态**：按语言对分模型（体积可拆，用户选什么下什么），冷门语言对走**英文中转**（A → en → B，两次推理）。
- **必需接口**：模型可动态加载，缺模型时通过 `resolve()` 返回缺失清单。
- **语言检测**：本地 MT 引擎不自带检测，需外挂轻量检测器（fastText lid / CLD3，约 1~2MB）或由前端提供源语言。

**备选方案对比**：

| 方案 | 一个模型覆盖 | 体积 | 下载可拆 | 结论 |
|---|---|---|---|---|
| 分语言对小模型（OPUS-MT 转换 CT2） | 一对 | 100~300MB/对 | ✅ | **采用** |
| NLLB-200 distilled | 200 语言互译 | ~600MB 起 | ❌ | 备选 |

**代价**：冷门语言对需中转，两次翻译会损失语序自然度（中↔日等差异大的语言尤其明显）。

### 5.3 LlmTranslator（高质量，galgame 场景核心）

**定位：翻译质量与语境适配最好，但延迟与资源开销最大。用于 galgame 等需要"人名一致、语气准确"的场景。**

#### 接入方式
统一走 **OpenAI 兼容协议**（`POST /v1/chat/completions`），一处实现覆盖三种后端：

| 后端 | baseUrl | 说明 |
|---|---|---|
| 内置 llama-server（默认） | `http://127.0.0.1:<随机端口>/v1` | 子进程拉起 |
| 用户已装 Ollama | `http://127.0.0.1:11434/v1` | 探测到就优先用 |
| 远程 OpenAI 兼容 API | 用户填写 | 可选 |

#### 输出约束（关键）
LLM 的失败模式必须用接口层兜住：

| 失败模式 | 处理 |
|---|---|
| 输出加前缀（"以下是翻译："） | 强制 JSON 输出 + 解析失败重试 |
| 段落数不符 | 校验 `size == expected`，重试 1 次，仍败则降级 |
| 长文本被截断 | 按段落切块（每块 5~8 段），块内并发 2 路 |
| 语言译错 | 抽查首段做语言检测，不符重试 |
| 超时 / OOM | 超时中止 + 降级到 local-mt |

#### 参数字典

| 参数 | 值 | 原因 |
|---|---|---|
| `temperature` | 0.1 ~ 0.3 | 翻译要稳定 |
| `max_tokens` | 2048 或按输入 ×2 | 防截断 |
| `num_ctx` | 4096 ~ 8192 | 上下文窗口 |
| `stream` | true | 首 token 即显示，感知延迟减半 |
| `format` | `"json"`（Ollama 专有） | 强制结构化输出 |

#### galgame 场景的三个必要增强

1. **上下文窗口**：把前 2~4 句原文+译文一并塞进 prompt，保证人名与语气一致。
2. **术语表（glossary）**：人名、地名、专有名词强制映射——这是 LLM 相对 Web/本地 MT 的唯一硬优势。
3. **预翻译 + 缓存**：galgame 文本可提前获取，后台预翻后续 N 句，用户推进时命中缓存 = 零感知延迟。**这是该场景能否可用的分水岭。**

#### 延迟参考（需实测复核，见 §11）

| 环境 | 7B | 3B | 1.5B |
|---|---|---|---|
| 8 核 CPU | 2.5~4s ❌ | 1.0~1.7s ⚠️ | 0.5~0.8s ✅ |
| Vulkan 核显 | 2~3s ❌ | 0.9~1.5s ⚠️ | 0.4~0.7s ✅ |
| RTX 3060 | 0.6~0.9s ✅ | 0.3~0.5s ✅ | 0.15~0.3s ✅ |

> 单句 30 字、输出 ~25 token 的估算。**结论：纯 CPU 跑 7B 不可用于实时；必须靠预翻译+缓存把延迟藏起来。**

---

## 6. 资产包系统（"插件"的落地形态）

**"插件"是抽象概念，不是第三方扩展系统。** 特指**官方资产包**，分两类，信任级别不同：

| 类型 | 例子 | 体积 | 可执行 | 校验 |
|---|---|---|---|---|
| `runtime` | llama-server、onnxruntime | 20~40MB | ✅ | **ed25519 签名 + sha256** |
| `model` | .gguf、CT2 模型包 | 100MB~5GB | ❌ | **sha256（写在已签名清单里）** |

**明确不做**：第三方插件加载、插件 API 版本兼容、沙箱权限模型。

### 6.1 信任链

```
内置公钥（编译进本体）
      │ 校验
      ▼
远程 manifest.json（ed25519 签名）
      │ 内含每个资产的 sha256
      ▼
下载的 zip / gguf → 校验 sha256 → runtime 额外验签
```

公钥硬编码在二进制中，不可从网络获取。
**只有 hash 没有签名不安全**：攻击者同时改文件和清单里的 hash 即可通过。

### 6.2 manifest 结构（示例）

```jsonc
{
  "schemaVersion": 2,
  "generatedAt": "2026-11-04T00:00:00Z",
  "channel": "stable",
  "assets": [
    {
      "id": "runtime.llama.vulkan.win-x64",
      "type": "runtime",
      "op": "windows", "arch": "x64", "backend": "vulkan",
      "version": "b<build>",
      "size": 38000000,
      "sha256": "…",
      "sig": "…",                       // ed25519
      "archive": "zip",
      "urls": ["https://主源/…zip", "https://备源/…zip"]
    },
    {
      "id": "model.qwen2.5-3b-instruct.q4km",
      "type": "model",
      "family": "qwen2.5", "quant": "Q4_K_M",
      "size": 1980000000,
      "sha256": "…",
      "requires": { "ramGB": 6, "vramGB": 0 },
      "urls": ["https://modelscope.cn/…", "https://hf-mirror.com/…"]
    },
    {
      "id": "runtime.llama.vulkan.linux-x64",
      "type": "runtime", "op": "linux", "arch": "x64",
      "urls": []                        // 占位：Linux 暂不下发
    }
  ]
}
```

**清单的四个必备设计**：`urls` 必须是数组（多源）；`sha256` 写死在清单里；`requires` 供 UI 预判硬件；`backend` 决定 vulkan / cpu 版本选择。

### 6.3 源策略

| 资产 | 主源 | 备源 | 理由 |
|---|---|---|---|
| manifest.json（~100KB） | jsDelivr | GitHub raw | 免费 CDN，零成本起步 |
| runtime zip（20~40MB） | 你的 OSS / GitHub Releases | 手动导入 | 体积小，自托管可承受 |
| GGUF 模型（GB 级） | ModelScope | hf-mirror | **模型绝不自托管**：1.9GB × 1000 用户 ≈ 950 元/月流量费 |
| 本地 MT 模型包 | 你的对象存储 | hf-mirror | 转换产物，公共仓库未必有 |

**注意**：jsDelivr 对 `@master` 有较长 CDN 缓存，更新清单需用 `@commit-sha` 或加查询串。

### 6.4 下载流水线（20 条注意事项摘要）

**网络层**
1. 多源 failover：主源超时/404 自动切镜像，记录可用源
2. HEAD 预检 + Range 断点续传
3. 分块并发（4~8 路），单块失败只重试该块
4. 仅允许 HTTPS，拒绝降级
5. 读系统代理 / 支持手动填代理（国内用户普遍走 Clash 等）
6. 限速 + 后台低优先级 + 可暂停/取消

**完整性 / 安全**
7. 校验顺序：体积 → sha256 →（runtime）签名 → 解压
8. 解压防 zip-slip / 路径穿越 / 符号链接 / 解压炸弹
9. 解压前检查磁盘剩余（需 2× 体积）
10. 原子安装：tmp → 校验 → 解压到 `version.tmp` → `rename` 到位

**平台 / 兼容**
11. CPU 指令集（AVX2）与 Vulkan 可用性探测
12. `packageSchemaVersion` 兼容检查（自家清单演进用）
13. 主机平台/架构匹配（win-x64 / linux-x64）

**运行期**
14. llama-server 只绑 `127.0.0.1` + 随机端口 + 一次性 token
15. 进程保活 + 崩溃重启上限 + 退出时清理残留
16. 首载预热（模型加载 5~15s），UI 必须显示"加载中"

**更新 / 卸载**
17. 版本并存（side-by-side），指针切换，绝不在运行中覆盖
18. 保留上一版用于回滚，只 GC 两版之前
19. 启动时完整性巡检（size + mtime，或抽查 hash）
20. 卸载/更新只标记，进程退出后再删

### 6.5 目录布局

```
%PROGRAMDATA%\Studious\          # 见 §9.2，避开 AppData
├─ runtime/
│   ├─ llama-vulkan/<version>/   { llama-server.exe, ggml-vulkan.dll, … }
│   ├─ llama-cpu/<version>/
│   └─ current.json              # {"llama-vulkan":"<version>", …} 版本指针
├─ models/
│   ├─ qwen2.5-3b-q4km.gguf
│   └─ opus-mt-en-zh/
├─ downloads/                    # 临时区，装完即删
└─ logs/

%LOCALAPPDATA%\Studious\
├─ cache/translations.sqlite
└─ config.json
```

### 6.6 手动导入（LocalInstaller）

**终极兜底，必做**。解决大文件下载失败、杀软拦截、局域网分发三类问题：

```
设置 → 模型管理
  ├─ 拖入 .gguf  → 算 sha256 → 匹配清单 → 注册
  ├─ 拖入 .zip   → 验签 + 校验 → 解压安装
  └─ 导出/导入   小 JSON（资产 id + hash），跨机器复用
```

实现上复用 `Downloader` 的校验与原子落盘逻辑，仅输入源从网络换成本地文件。

---

## 7. 运行时管理（LlmRuntimeManager）

**为什么用子进程而不是进程内链接**：

| | 进程内链接 | **子进程（采用）** |
|---|---|---|
| 模型 OOM / 驱动崩溃 | 带走整个 App | 主程序存活，可重启子进程 |
| llama.cpp 升级 | 需重编重发版 | 换二进制即可 |
| GPU 后端切换 | 需重新编译 | 换 DLL |
| 通信 | C API | HTTP（localhost 随机端口） |

**由于走 HTTP，`OpenAICompatClient` 代码一行不变**，只需切换 `baseUrl`。

启动流程：

```
启动 → probe(用户已装 Ollama)
         ├─ 可用  → 直接用，baseUrl = 127.0.0.1:11434
         └─ 不可用 → spawn(内置 llama-server, 随机端口, 一次性 token)
                      → 等待 /v1/models 就绪（超时 30s）
                      → 预热一次空翻译
退出 → kill 子进程 + 清理端口
```

**后端矩阵（v1 只发两项）**：

| 后端 | 覆盖 | 额外体积 | v1 |
|---|---|---|---|
| CPU（AVX2） | 所有机器兜底 | ~5MB | ✅ 必带 |
| Vulkan | NVIDIA / AMD / Intel | ~30MB | ✅ 必带 |
| CUDA + cuBLAS | 仅 NVIDIA，最快 | +400~800MB | ❌ 后置 |

> CUDA 那 800MB 是这套方案最重的固定成本，v1 不碰。

**模型分级**：

| 模型 | 量化体积 | 建议 RAM | 定位 |
|---|---|---|---|
| 1.5B | ~1.0GB | 4GB | 低配兜底 |
| **3B** | ~1.9GB | 6GB | **默认推荐（短句甜点区）** |
| 7B | ~4.7GB | 10GB | 高质量可选 |

---

## 8. 缓存与预翻译

### 8.1 TranslationCache

```
key:   sha256(engine_id + srcLang + dstLang + srcText)
value: translated text + 时间戳
存储:  SQLite
```

galgame 场景重复率高（`はい`、角色名、口头禅），**真实命中率可达 30~60%**。

### 8.2 预翻译（后续版本）

```
hook 到第 1 句 → 后台预翻后续 N 句 → 用户推进时命中缓存
```

配套需要：批量导入接口、进度回调、断点续翻、并发控制。
**这会改变 `TranslationCache` 的设计（需支持批量写入与任务状态），需在 v2 规划时一并考虑。**

---

## 9. 平台差异与注意事项

### 9.1 Linux：只留占位

1. manifest 中加 `"op": "linux"` 条目，`urls` 为空 → 客户端标记"暂不支持"。
2. `Platform` 枚举 + 工厂函数，Linux 分支返回 `NotSupported`。
3. `runtime/LinuxServerLauncher` 空实现（抛 `NotImplemented`）。

**首版只支持 X11 会话**（Wayland 的截图需 portal 授权、浮层需 compositor 支持 layer-shell）。

### 9.2 Windows：程序自拉 exe 的真实情况

**系统层面没有任何限制**，程序完全可以下载并运行 exe。真正的阻力是四层安全机制：

| 层 | 触发条件 | 表现 | 可否规避 |
|---|---|---|---|
| MOTW（Zone.Identifier ADS） | **下载器主动写入** | 文件被标记"来自互联网" | ✅ 能 |
| SmartScreen | 有 MOTW + 无签名/无信誉 | 全屏蓝框警告 | ✅ 能（无 MOTW 即不触发） |
| Defender 实时扫描 | 写入/执行时启发式命中 | 静默删除 | ⚠️ 部分 |
| 国产杀软 | 未签名 + 联网 + 监听端口 | 弹窗 / 删除 | ❌ 需用户加白 |

**关键事实**：MOTW 不是"下载就有"，而是下载器主动写入的 ADS。

| 下载方式 | 是否写 MOTW |
|---|---|
| 浏览器 / `URLDownloadToFile` / `IAttachmentExecute` | ✅ |
| **Qt `QNetworkAccessManager` + `QFile` 手动写盘** | ❌ |
| **zip 解压出的文件（自写解压）** | ❌（ADS 不随解压传播） |

**因此**：下载 `.zip` + 程序内自解压 → 产出 exe 无 MOTW → 不触发 SmartScreen。

**落地位置**：装到 `%PROGRAMDATA%\Studious\runtime`。
**理由**：从 `%LOCALAPPDATA%` 执行 exe 是典型恶意软件特征，杀软敏感度显著更高。

**文件缺失处理**：启动前校验存在性 + hash，失败时提示
"推理引擎被安全软件移除，请加入白名单或手动导入"，**不要只报"无法启动"**。

**代码签名证书**（见 ADR-09）：v1 不购买。OV 约 1000~3000 元/年，EV 需企业资质且更贵。

**反面做法（明确不做）**：下载后主动清除 MOTW（`Unblock-File` / 删 `Zone.Identifier`）。技术上可行，但这是恶意软件标准行为，会降低整程序信誉。

---

## 10. 决策记录（ADR）

| # | 决策 | 结论 | 理由 |
|---|---|---|---|
| 01 | 平台切分 | 前端 = 截图/选区/浮层/热键/OCR；后端 = 纯翻译 | OCR 是平台能力，后端保持可复用 |
| 02 | 前端框架 | Qt | 双端一套 UI，浮层能力齐全 |
| 03 | OCR 归属 | 前端 | 与平台强绑定 |
| 04 | 语言码 | 简化 BCP-47（`zh` / `zh-TW` / `en` / `ja` / `pt-BR`） | 兼容性与可读性平衡 |
| 05 | 多目标 | **接口复数、v1 UI 单选** | 接口改单数成本高，UI 可后开 |
| 06 | Web 默认策略 | 免费源 + 用户 key 双轨 | 开箱可用且不至于被封死 |
| 07 | 本地 MT | 分语言对模型（CT2/OPUS-MT）+ 英文中转 | 体积可控，用户按需下载 |
| 08 | LLM 后端 | **子进程 llama-server，CPU + Vulkan** | 稳定性优先，CUDA 后置 |
| 09 | 签名证书 | v1 不买 | 靠 zip 自解压 + 明确引导 |
| 10 | "插件"定位 | 官方资产包（runtime / model），非第三方扩展 | 不开放第三方，砍掉整套插件 API |
| 11 | 清单签名 | ed25519，公钥硬编码 | 信任链根 |
| 12 | 下载器 | Qt 自研 | 需深度集成 sha256 / 续传 / 原子装 |
| 13 | manifest 托管 | jsDelivr 主 + GitHub raw 备（仓库 public） | 零成本起步 |
| 14 | 模型托管 | ModelScope 主 + hf-mirror 备 | 自托管流量费不可持续 |
| 15 | 手动导入 | 进 v1 | 成本低，救三类用户 |
| 16 | Linux | 占位，仅 X11 规划 | 暂无设备测试 |
| 17 | 实现语言 / 框架 | **C++20 + Qt 6 Core**，编译为静态库、与前端同进程 | 零 IPC、零 FFI、单一工具链 |
| 18 | 项目授权 | **开源** | LGPLv3 静态链接的合法性由“公开源码”满足 |
| 19 | 本地 MT 推理后端 | **onnxruntime** | 跨端预编译二进制最省心 |
| 20 | 基础依赖 | Monocypher（ed25519）+ miniz（zip） | 均为单文件、无依赖 |

---

## 11. 待联网核实清单（当前网络不可达，未能验证）

> 本次会话中 `api.github.com` / `cdn.jsdelivr.net` / `raw.githubusercontent.com` 均解析到非公网 IP，搜索服务无 API key。以下条目**均未经实测验证**，编码前需逐条确认。

| # | 待核实 | 核实方式 |
|---|---|---|
| 1 | llama.cpp 官方 Windows **Vulkan** 预编译包是否存在、实际体积 | 查 GitHub Releases 资产列表 |
| 2 | `llama-server` 的 OpenAI 兼容端点路径与启动参数（`--port` / `--host` / `--api-key` / `-ngl`） | 拉取最新仓库文档实测 |
| 3 | `llama-server` 是否支持 `format: json` 类结构化输出（这是 Ollama 的参数） | 实测 `/v1/chat/completions` |
| 4 | Qwen2.5 各尺寸 GGUF **Q4_K_M 实际体积**（1.5B/3B/7B） | 查 ModelScope / HF 文件列表 |
| 5 | ModelScope 上 Qwen2.5 GGUF 的**直链 URL 格式**与 Range 支持 | 实测 curl -r |
| 6 | 上述延迟数据（CPU/Vulkan/CUDA 下各尺寸） | 本机实测 |
| 7 | OPUS-MT 转 CTranslate2 的**转换流程与产物体积**（每对） | Argos Translate 仓库 |
| 8 | jsDelivr 单文件上限（印象 20MB）、`@master` 缓存时长 | 官方文档 |
| 9 | Qt `QNetworkAccessManager` 是否确实不写 `Zone.Identifier` | Windows 实测特性验证 |
| 10 | 各 Web 翻译 adapter 的语言码与 `auto` 支持情况 | 逐个实测 |
| 11 | GitHub Releases 单文件上限（印象 2GB） | 官方文档 |
| 12 | 内部语言码与 NLLB / DeepL 语言码对照表完整性 | 各官方文档 |

**核实顺序建议**：1 → 2 → 4 → 5 → 9（前五项直接决定 v1 能否跑通）。

---

## 12. 未决事项

| # | 问题 | 待定 |
|---|---|---|
| 1 | 是否采用安装器可选组件内置 runtime | v1 不做，视用户反馈 |
| 2 | ~~本地 MT 引擎选型~~ | **已定：onnxruntime**（见 ADR-19） |
| 3 | 默认模型（3B vs 7B） | 暂定 3B |
| 4 | 预翻译与批量导入是否进 v2 | 待定 |
| 5 | 空翻译预热策略（是否首次启动就加载模型） | 待定 |
| 6 | 项目开源协议（MIT / Apache-2.0 / GPL-3.0） | 待定；若用 LGPL Qt 静态链接，MIT/Apache 亦可 |
| 7 | ed25519 **签名私钥**存放方式 | 待定；**绝不可提交进仓库**，应走 CI Secret |
| 8 | Qt 6.8 LTS 的开源版补丁可用性 | 待联网核实（历史经验：LTS 后期补丁对开源用户延后） |

---

## 13. 后续落地顺序

1. `contract/ITranslator.h` + `Types.h` + `LanguageRegistry`
2. `asset/AssetRegistry` + manifest 验签
3. `asset/Downloader`（工作量最大，独立测试）
4. `web/WebTranslator`（无资产依赖，最早可端到端验证）
5. `runtime/LlmRuntimeManager` + `llm/LlmTranslator`
6. `local/LocalMtTranslator`
7. `cache/TranslationCache`
8. `router/TranslatorRouter`（最后组装）

---

## 14. 技术栈与构建

### 14.1 定版

| 项 | 选型 | 说明 |
|---|---|---|
| 语言 | **C++20** | |
| 框架 | **Qt 6.8 LTS，仅 Core / Network / Sql** | 不引 Widgets / Quick，后端保持无 UI 依赖 |
| 形态 | **静态库，与前端同进程** | 整个程序仅 `llama-server` 为子进程 |
| 构建 | CMake 3.21+ + FetchContent | |
| 编译器 | MSVC 2022（Win）/ GCC 13+ 或 Clang 16+（Linux） | |
| 项目授权 | **开源** | LGPLv3 静态链接合法性由公开源码满足 |

**为什么后端不与前端拆成两个语言/两个进程**：零 IPC、零 FFI、单一工具链、低延迟回调；Qt Core 已覆盖 HTTP / JSON / SQLite / 哈希 / 子进程 / 线程池，无需另找语音运行时。

**备选方案被否决的理由**：

| 方案 | 否决原因 |
|---|---|
| Rust（axum/tokio） | 与 Qt 前端跨语言，需 C ABI + 两套构建与依赖管理 |
| Go | 同上，且嵌入 Qt 进程麻烦 |
| Python | Windows 发行打包是灾难（双运行时、体积 200MB 起） |
| C# / .NET | 开发环境无 SDK，与 C++ Qt 混编成本高 |
| 纯 C++ 标准库 | HTTP / JSON / SQLite / zip 全部自找库，成本大于收益 |

### 14.2 Qt Core 直接提供的能力

| 能力 | Qt 类 |
|---|---|
| HTTP 客户端 + Range 续传 | `QNetworkAccessManager` / `QNetworkRequest` / `QNetworkReply` |
| JSON | `QJsonDocument` / `QJsonObject` / `QJsonArray` |
| SQLite | `QSqlDatabase`（`QSQLITE` 驱动） |
| SHA-256 | `QCryptographicHash`（增量 `addData`） |
| 子进程 | `QProcess` |
| 线程池 / 异步 | `QThreadPool` / `QtConcurrent` / `QRunnable` |
| 配置 | `QSettings` 或自管 JSON |
| 日志 | `QLoggingCategory` + 自定义 `messageHandler` |
| SSE 流式解析 | `QNetworkReply::readyRead` 手写 |

### 14.3 第三方依赖（仅两个）

| 需求 | 选型 | 理由 |
|---|---|---|
| **ed25519 验签** | **Monocypher** | 单文件、公有领域、无依赖；libsodium 亦可但体积大 |
| **zip 解压** | **miniz** | 单文件；**不可用 Qt 私有 `QZipReader`** |
| 本地 MT 推理 | onnxruntime（C API） | 子进程加载，跨端预编译包齐全 |
| LLM 推理 | llama-server 子进程 | 见 §7 |

### 14.4 构建骨架

```cmake
cmake_minimum_required(VERSION 3.21)
project(studious_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql)

include(FetchContent)
FetchContent_Declare(monocypher GIT_REPOSITORY https://github.com/LoupVaillant/Monocypher.git GIT_TAG <待核实>)
FetchContent_Declare(miniz      GIT_REPOSITORY https://github.com/richgel999/miniz.git      GIT_TAG <待核实>)
FetchContent_MakeAvailable(monocypher miniz)

add_library(backend STATIC
    contract/LanguageRegistry.cpp
    asset/AssetRegistry.cpp asset/Downloader.cpp asset/LocalInstaller.cpp
    translators/web/WebTranslator.cpp
    translators/llm/LlmTranslator.cpp translators/llm/OpenAICompatClient.cpp
    runtime/LlmRuntimeManager.cpp
    router/TranslatorRouter.cpp
    cache/TranslationCache.cpp
)
target_include_directories(backend PUBLIC src)
target_link_libraries(backend
    PUBLIC  Qt6::Core Qt6::Network Qt6::Sql
    PRIVATE monocypher miniz
)
```

### 14.5 线程模型

| 操作 | 线程 | 说明 |
|---|---|---|
| 翻译请求 | 异步（`QThreadPool`） | 绝不阻塞 UI |
| HTTP 下载 | `QNetworkAccessManager` 异步信号槽 | 注意 NAM 的线程亲和性 |
| **SHA-256 校验（GB 级）** | 后台线程 + 分块 `addData` | **禁止在 UI 线程算 2GB 的 hash** |
| zip 解压 | 后台线程 | 同上 |
| SQLite 读写 | **每线程独立连接** | Qt 的 DB 连接**不可跨线程** |
| llama-server 通信 | 异步 HTTP | |
| 子进程生命周期 | 主线程 `QProcess` | 信号槽回调 |

### 14.6 三个必须提前知道的坑

**坑 1：`QSqlDatabase` 不能跨线程**
每个工作线程必须 `addDatabase` 建独立连接（连接名唯一），否则随机 `database is locked` 或崩溃。

**坑 2：`QZipReader` 是私有 API**
位于 `QtGui/private/` 下，跨版本会删改。用 miniz。

**坑 3：`QNetworkAccessManager` 是线程亲和的**
一个 NAM 只能在一个线程使用。要么全局单例（主线程 + 异步信号槽），要么每工作线程各建一个并在该线程内完成生命周期。

### 14.7 Qt 授权（开源场景）

- Qt 6.8 LTS 开源版为 **LGPLv3 / GPLv3**。
- **LGPLv3 允许静态链接**，前提是「提供重新链接的手段」——本项目公开源码即满足。
- 因此**项目自身协议可以是 MIT / Apache-2.0**（待定，见未决事项 6）。
- 注意：若使用 GPL-only 的 Qt 模块，则项目必须整体 GPL-3.0。当前选型（Core / Network / Sql）均在 LGPLv3 覆盖范围内。

---

## 15. 开源带来的额外注意事项

| # | 事项 | 说明 |
|---|---|---|
| 1 | **ed25519 私钥绝不可入库** | 放 CI Secret；一旦泄露，可伪造 manifest 向所有用户投毒 |
| 2 | 公钥可以公开 | 验签公钥本就应硬编码并公开，不构成风险 |
| 3 | 他人可自行编译并去掉验签 | 这是对方自己的 fork，不影响官方分发链路 |
| 4 | 社区可自制模型包 | manifest schema 应文档化，允许第三方按同一格式提交资产（仍由官方私钥签名后才被客户端接受） |
| 5 | CI 需双端构建 | GitHub Actions 矩阵：`windows-latest` / `ubuntu-latest`；Linux 仅出占位产物 |
| 6 | 架构守护 | `contract/` 的变更需评审；`backend/` 禁止引入平台头文件，可用 CI grep 检查 |
| 7 | manifest schema 版本化 | `schemaVersion` 单调递增，客户端拒绝过新的 schema 而非猜测 |
| 8 | 签名流程 | 建议独立仓库或 CI job 负责签名，主仓库只持有验签公钥 |

**CI 架构守护示例**：

```bash
# 禁止后端出现平台相关头文件
! grep -rnE '#include <(windows\.h|X11/|unistd\.h>)' backend/ src/contract src/asset
```

---

## 16. 实施记录（v0.1 demo）

### 16.1 实际环境

| 项 | 计划 | 实际 |
|---|---|---|
| Qt | 6.8 LTS | **6.12.0**（mingw_64） |
| 编译器 | GCC 13.1+ | GCC 13.1.0（Qt 自带 MinGW） |
| CMake / Ninja | 3.21+ / 任意 | 3.30.5 / 1.12.1 |

> 6.12.0 非 LTS，但本项目只用 Core / Network / Widgets，不受 LTS 补丁策略影响。

### 16.2 已实现

- `contract/`：`Types.h` / `ITranslator.h` / `LanguageRegistry`（14 个内部语言码 + 别名归一化）
- `net/`：`IHttpClient` 端口 + `QtHttpClient` 实现（QNAM + QEventLoop + QTimer 三重超时兜底）
- `translators/web/`：`WebTranslator` + `GoogleFreeAdapter` + `MyMemoryAdapter`（双适配器自动降级）
- `translators/mock/`：离线自测引擎
- `translators/llm|local-mt`：桩（`isReady()==false`，`resolve()` 返回缺失资产清单）
- `router/TranslatorRouter`：优先级选路 + 强制引擎 + 全失败时拼接降级原因
- `Factory`：组件装配入口（前端不接触内部适配器）
- `selftest`：离线/联网两套自测

### 16.3 实测结果

| 项 | 结果 |
|---|---|
| 构建 | 16/16 步通过，零警告 |
| `selftest --offline` | ALL CORE TESTS PASSED |
| 真实联网（Google） | `Hello, world!` → **`你好世界！`** |
| 真实联网（MyMemory） | `auto→zh` → `您好，世界！`；`en→ja` → `世界、こんにちは！` |
| 平台头文件自检 | `windows.h` / `_WIN32` / `X11` / `unistd.h` 均无 |

### 16.4 踩坑记录

**坑 1（架构级）：`QVector<std::unique_ptr<T>>` 在 Qt 6 编译不过**

Qt 6 的 `QList`（`QVector` 是其别名）要求元素可拷贝：`push_back` / `reserve` / `operator[]`→`data()`→`detach()` 都会实例化 `copyAppend` 路径。`std::unique_ptr` 删除了拷贝构造 → 编译失败。

`Q_DECLARE_TYPEINFO(..., Q_RELOCATABLE_TYPE)` 也救不了——`reallocateAndGrow` 里 copy 与 move 两个分支都会被实例化。

**结论**：所有权容器统一用 `std::vector<std::unique_ptr<ITranslator>>`，别名 `router::TranslatorList`。**已在文档与代码中统一，后续不要再写 QVector。**

**坑 2：`QtHttpClient` 线程亲和**

`QNetworkAccessManager` 只能在所属线程使用。工作线程翻译必须在该线程内构造 `QtHttpClient` 与 translator，不能跨线程共享实例。

**坑 3：`setTransferTimeout` 偶发不生效**

已加 `QTimer` + 手动 `abort()` + `loop.quit()` 三重兜底，同步 `send()` 不会挂死。

**坑 4：Google 免费接口限流返回 HTML**

限流时返回 HTML 而非 JSON。解析失败的错误信息里附带响应前 200 字节，便于排查。

**坑 5：`toLocal8Bit()` 在 UTF-8 终端产生乱码**

Windows 控制台默认 cp936，`toLocal8Bit()` 会把 UTF-8 内容转成 GBK 字节。后端统一改直接输出 UTF-8；老控制台需 `chcp 65001`。

**坑 6：Qt 6 已移除 `QTextCodec`**

`QTextCodec` 从 Core 移到 Core5Compat，不要再指望用它做转码。

**坑 7：`.ps1` 脚本必须存成 UTF-8 **带 BOM**

Windows PowerShell 5.1 对无 BOM 的 `.ps1` 按 ANSI(cp936) 解码，脚本里的中文字符串会被打乱，进而导致**语法解析失败**（报 `Unexpected token`）。

另外 `Set-Content -Encoding utf8` 在 PS 5.1 下写出的是**带 BOM**的，但很多编辑器/工具默认存无 BOM —— 用 git 的 `.gitattributes` 也管不了 BOM。

**结论**：交付给用户的 `.ps1` 一律写成 `UTF8Encoding($true)`（带 BOM）。

**坑 8：PATH 必须包含 `mingw_64\bin`，不是 `mingw_64`**

Qt 的 DLL 在 `Qt\6.12.0\mingw_64\bin`。只把 `mingw_64` 加进 PATH，程序启动会以 **`0xC0000135`（-1073741515, DLL_NOT_FOUND）** 静默退出，**并且不打印任何输出** —— 极易被误判为"程序没输出/脚本吞了输出"。

排查口诀：**看到 -1073741515，先查 Qt DLL 路径。**

**坑 9：管道吞输出**

`SomeFunction | Out-Null` 会把函数内部所有原生命令（cmake/ninja/子进程）的输出一并吞掉，只剩 `Write-Host` 能显示。构建脚本因此改为"捕获后逐行 Write-Host 输出"。

### 16.5 遗留

- `LocalMtTranslator` / `LlmTranslator` 仍为桩，待 v2
- 资产系统（`asset/`）、缓存（`cache/`）尚未实现
- 前端仅为纯文本测试窗口，截图 / OCR / 浮层均未开始

---

## 17. v0.2 增补：云端 LLM 与 Web 多来源

### 17.1 排期调整（重要）

原计划 §7 的本地 LLM（llama-server 子进程 + 资产系统）**降级到 v2**，改为先做云端 LLM。

| | 云端 LLM | 本地 LLM |
|---|---|---|
| 代码量 | ~400 行 | ~2000 行（资产系统 + 子进程 + 后端探测） |
| 依赖 | 一把 API key | 4.7GB 模型 + 显卡 |
| 单句延迟 | **1.2~2.3s**（实测） | CPU 上 2.5~4s（7B） |
| 质量 | 最好 | 取决于硬件 |
| 离线 | ❌ | ✅ |
| 隐私 | ❌ 文本出本机 | ✅ |

**关键洞察**：云端 LLM 与本地 llama-server、Ollama 走的是**同一套 OpenAI 兼容协议**，因此 `LlmTranslator` 一份实现覆盖三种后端，只换 `baseUrl` / `apiKey` / `model`。

### 17.2 已实现

**`LlmTranslator` 真实实现**（不再是桩）

| 项 | 做法 |
|---|---|
| 协议 | `POST {baseUrl}/chat/completions`，`Authorization: Bearer <key>` |
| 粒度 | **逐段落请求**（方案 A）：失败可精确定位到段，无需批量格式容错 |
| 取字段 | 只取 `choices[0].message.content`，**显式忽略 `reasoning_content`** |
| prompt | 强约束 system + `<<< >>>` 原文边界 |
| 后处理 | 剥除模型偶发的 ``` 代码块与成对引号 |
| 错误 | HTTP 401/402/429/5xx/超时/解析失败 → 中文可读原因 |

**踩坑 1：弱 prompt 会变成闲聊**

只写「翻译下面文本」时，模型会输出「翻译如下：」或直接回答原文中的问题。实测用疑问句、UI 菜单、剧情长句三组刁钻输入验证，强 system prompt + 边界符后全部只出译文。

**踩坑 2：推理模型的 `reasoning_content`**

`deepseek-v4.1-flash` 等推理模型会同时返回 `content` 与 `reasoning_content`（思考过程，实测 170~344 字符）。**只认 `content`**，否则用户看到的是一段自言自语。

**踩坑 3：非 2xx 时 Qt 只报「网络错误(203)」**

真实原因（如 `model_not_found`）在响应体里。4xx 分支必须把 body 前 200 字节贴进错误信息。

### 17.3 Web 多来源

`IWebAdapter` 扩展三个能力：`displayName()` / `requiresApiKey()` / `isConfigured()`。

由 `WebProvider` 注册表统一管理（`listWebProviders()` / `makeWebAdapters()`），前端按 id 有序装配，**未配置的来源直接跳过不发请求**，失败继续试下一家。

| 来源 | id | 需 key | 状态 |
|---|---|---|---|
| Google 在线翻译 | `google` | 否 | ✅ 可用（国内常需代理，高频会 429） |
| Microsoft Azure | `azure` | 是 | 🚧 **仅占位**（见 17.4） |
| MyMemory | `mymemory` | 否 | ✅ 可用（不支持 auto 源语言，额度有限） |

### 17.4 微软翻译为何只留占位

实测三家微软相关端点：

| 方案 | 端点 | 实测结果 | 结论 |
|---|---|---|---|
| Edge 免费接口 | `edge.microsoft.com/translate/auth` | **404** | 端点已下线/迁移 |
| Bing 网页版 | `www.bing.com/ttranslatev3` | 三种参数组合全部 `{"statusCode":205}` | 反爬已加强，不可依赖 |
| Azure 官方 | `api.cognitive.microsofttranslator.com/translate?api-version=3.0` | **401**（端点存活，仅缺 key） | ✅ 唯一正路 |

**结论**：微软只剩 Azure 官方一条路，需订阅 key（F0 免费层约 200 万字符/月）。v1 保留接口与错误提示，等用户提供 key 再接入。

接入契约已写在 `AzureTranslatorAdapter.h` 注释里：
```
POST https://api.cognitive.microsofttranslator.com/translate?api-version=3.0
Header: Ocp-Apim-Subscription-Key / Ocp-Apim-Subscription-Region
Body:   [{"Text": "..."}]
Resp:   [{"translations":[{"text":"..."}]}]
```

### 17.5 配置持久化约定- LLM 凭据（`baseUrl` / `apiKey` / `model`）由前端 `QSettings` 保存，**后端只接收注入的 `LlmConfig`，不读配置、不读环境变量**（保持无状态）。
- 开源仓库中 `AppSettings::defaults()` 带作者本地网关的默认值（内网地址，无泄露风险）。
- 未来需要加密时，按 §2 的端口-适配器原则加 `ISecretStore` 实现，不改 backend 契约。

### 17.6 思考等级（reasoning_effort）

翻译是确定性任务，推理模型"先想再答"只增加延迟、不提升译文质量。故 `LlmConfig` 新增 `reasoningEffort`，**默认 `"none"`**。

| 取值 | 含义 |
|---|---|
| `"none"`（默认） | 彻底关闭思考；同时补发 `thinking:{type:"disabled"}` 兼容 Anthropic 风格网关 |
| `"minimal"` / `"low"` / `"medium"` / `"high"` | 透传给服务端 |
| 空串 | 完全不发送该参数，跟随服务端默认（最慢） |

**本机网关实测（deepseek-v4.1-flash，同一句英文→中文）**：

| 请求参数 | reasoning_tokens | 说明 |
|---|---|---|
| 不传 | 24~81 | 默认会思考 |
| `reasoning_effort: "minimal"` | 73 | **无效**，思考照旧 |
| `enable_thinking: false` | 42 | **无效** |
| `chat_template_kwargs.enable_thinking: false` | 23 | 部分生效，仍不干净 |
| `reasoning_effort: "none"` | **0** | ✅ 生效 |
| `thinking: {type: "disabled"}` | **0** | ✅ 生效 |
| 两者同时发送 | **0** | ✅ 不报错，故实现里同时发 |

> 延迟方差较大（1.0~2.3s），但 `reasoning_tokens` 是最可靠的判据：**看 token 数，不看秒数**。

### 17.7 模型目录与能力探测（`llm/LlmCatalog`）

用户填完 Base URL + API Key 后，**由后端去问网关要模型列表**，前端只负责渲染下拉——前端不拼接口。

```cpp
// GET {baseUrl}/models
ModelListResult listModels(const config::LlmConfig &cfg, net::IHttpClient &http);

// 按模型能力推导可选思考等级
QVector<ReasoningOption> reasoningOptionsFor(const LlmModelInfo &model);

// 真实探测"关闭思考"是否生效（元数据可能是错的）
ReasoningProbeResult probeReasoning(const config::LlmConfig &cfg, net::IHttpClient &http);
```

**关键设计 1：网关不返回"思考等级列表"**

`/models` 只给 `supports_reasoning` 布尔值，没有等级枚举。所以等级列表是**推导**出来的：

| 模型能力 | 可选等级 |
|---|---|
| `supports_reasoning = true` | `none`(推荐) / `minimal` / `low` / `medium` / `high` / `<不发送>` |
| `supports_reasoning = false` | 只有一项「不适用」，且**不发送任何思考相关字段**（严格网关遇未知字段会 400） |

**关键设计 2：过滤非聊天模型**

网关返回 24 条，其中 3 条是图像模型（`kind=image`）。只保留 `kind` 为空或 `chat` 的，实测得到 **21 个聊天模型**。把图像模型列进翻译模型下拉只会干扰用户。

**关键设计 3：`reasoningOptionsFor` 是纯函数**

不依赖 HTTP、可在任意线程调用，前端切模型时实时刷新下拉，无需再发请求。

**关键设计 4：元数据可能撒谎，所以有 `probeReasoning`**

它真发一次 `reasoning_effort=none` 的短请求，读 `usage.completion_tokens_details.reasoning_tokens` 与 `message.reasoning_content`，返回三态：

| 结论 | 判据 |
|---|---|
| `Disabled` | 有 token 明细且 `reasoning_tokens == 0`，且无 `reasoning_content` |
| `StillThinking` | 有 `reasoning_content` 或 `reasoning_tokens > 0` |
| `Unknown` | 网关没回 usage 明细，无法判定 |

**实测输出（本机网关）**：

```
共 21 个聊天模型：
  deepseek-v4.1-flash   推理=是 输入上限=1000000 输出上限=128000
  default-model         推理=否 ...
  ...

模型 deepseek-v4.1-flash 支持的思考等级（由能力推导）：
  [none] 关闭（推荐，最快）  ← 推荐
  [minimal] 极低 / [low] 低 / [medium] 中 / [high] 高 / [<不发送>] 跟随模型默认

真实探测：
  reasoning_tokens  = 0
  reasoning_content = 无
  探测译文           = 你好，世界！
  结论：思考已成功关闭
```

**注意**：`default-model`（Auto）的 `supports_reasoning` 是 `false`，但它内部可能路由到推理模型——这种"聚合模型"的元数据不可全信，正是 `probeReasoning` 存在的理由。

### 17.8 延迟剖析与批量合并（性能关键）

**问题**：关掉思考后仍然"慢"，每次约 1.1~2.3s。

**逐层拆解实测**：

| 测试 | 耗时 | 说明 |
|---|---|---|
| `GET /models`（纯到网关开销） | **6~62 ms** | 本地链路可忽略 |
| 极短请求：输入 32 token / 输出 4 token | **1031~1357 ms** | — |
| 完整请求：输入 69 token / 输出 5 token | **991~1431 ms** | **token 变多，耗时没变** |
| 流式 TTFT（首字） | **1278~1638 ms** | 比非流式总时长还长，流式救不了 |

**结论：延迟几乎 100% 来自"每次请求固定的上游首字延迟"，与输入长度、输出长度、思考开没开、本地代码都无关。**

于是唯一的杠杆是：**减少请求次数**。

| 策略 | 效果 |
|---|---|
| 逐段请求 N 次 | `N × 1.1s` |
| **合并成 1 次请求** | `1 × 1.1s` |

**实测（5 段英译中，`--llm-batch`）**：

```
[batchSize=1 逐段请求] 6375 ms
[batchSize=8 合并请求]  910 ms      ← 7.0 倍
译文完全一致
```

**实现要点（`LlmTranslator`）**：

1. `LlmConfig.batchSize = 8`（`<=1` 退回逐段）。
2. 按块请求，要求模型**只输出 JSON 字符串数组**，长度必须等于输入条数。
3. 解析失败/条数不符 → **重试 1 次**（温度降到 0、加重语气）；仍失败 → **整块回退到逐段**，把问题定位到具体段落。
4. 空段落不发请求，结果按下标回填，保证与原文位置一一对应。
5. 批量请求的 `max_tokens` 按段数动态给足（`clamp(段数 × 128, 1024, 8192)`），防截断。

**为什么批量只是"加速手段"而非"新失败点"**：批量失败一律回退逐段，最坏情况退化为原来的行为，不会比改造前更差。

**后续更大的杠杆**：

| 手段 | 预期 |
|---|---|
| **翻译缓存**（原文 hash → 译文） | 命中即 0ms；galgame 重复台词多 |
| **预翻译**（后台提前翻后续 N 句） | 玩家推进时命中缓存，感知延迟趋近 0 |
| 换更快的模型 | 实测各模型都在 1.0~1.3s，收益有限 |

---

## 18. 翻译缓存（Translation Memory）

### 18.1 定位

缓存不是"优化"，是**延迟结构上的第二个数量级**：

| 场景 | 无缓存 | 有缓存 |
|---|---|---|
| 首次遇到某句 | ~1.1s | ~1.1s（不变） |
| 再次遇到同一句 | ~1.1s | **0ms** |

galgame 的台词、菜单项、角色口头禅重复率极高，实测自测用例首次 1123ms → 命中 **0ms**。

### 18.2 缓存键设计

```cpp
key = SHA256( engineTag \x1f srcLang \x1f dstLang \x1f text )
```

三个要点：

1. **必须带 `engineTag`**。同一个原文换个模型/换家翻译源，译文可能完全不同，混用会让用户以为"翻译坏了"。
   默认 `engineTag = id()`，`LlmTranslator` 覆写为 `"llm|<model>|<baseUrl>"`。
2. **用 `\x1f` 分隔**，不用 `|` 之类能出现在正文里的字符，避免 `"a|b"` 与 `"a"` + `"|b"` 撞键。
3. **键里不含 uid/时间**，保证跨进程、跨会话可复用。

### 18.3 开关接口

```cpp
router.setTranslationCache(&cache);   // 注入（不注入 = 完全不用缓存）
router.setCacheEnabled(true/false);   // 开关
router.isCacheEnabled();
router.lastCacheReport();             // {used, hits, misses} 供 UI 显示
```

**两级开关**：不注入 = 根本没有缓存能力；注入了但 `enabled=false` = 完全绕过（不读不写）。自测已验证关闭后库内条数为 0。

### 18.4 执行流程

```
translate()
  ├─ 未开缓存 → 原样交给引擎（行为与改造前完全一致）
  └─ 开缓存
       ├─ 逐段查 key
       ├─ 命中 → 直接填进结果，不发请求
       └─ 未命中 → 收集起来，一次性交给引擎翻译（仍然走批量合并）
                     → 成功后逐条写回缓存
```

**注意**：未命中集合仍走 §17.8 的批量合并，所以"部分命中"也是 1 次请求，不是 N 次。

### 18.5 存储实现

- SQLite（`QSQLITE`），表 `translations(key, source, translated, engine, src_lang, dst_lang, created_at, hit_count)`
- `PRAGMA journal_mode=WAL` + `busy_timeout=3000`，允许并发读写
- **每线程一条连接**（连接名含线程 id）——`QSqlDatabase` 不可跨线程
- 空译文不入缓存（否则下次会命中一个空结果）
- 超过 `maxEntries`（默认 10 万条）时按 `created_at` 淘汰最旧的一批
- `hit_count` 记录命中次数，将来可按"常用度"做淘汰优先级

### 18.6 自测覆盖（`--cache`）

| 用例 | 断言 |
|---|---|
| `cacheOpen` | 数据库能打开 |
| `cacheDisabledIgnored` | `enabled=false` 时 `report.used == false` |
| `cacheFirstMiss` | 首次全部未命中（hits=0, misses=N） |
| `cacheSecondHit` | 第二次全部命中（hits=N, misses=0） |
| `cacheSameResult` | 两次译文完全一致（缓存不能改变结果） |
| `cacheOffNoWrite` | 关闭后不写入（库内条数 0） |

实测输出：

```
第一次：hits=0 misses=3  1123 ms
第二次：hits=3 misses=0     0 ms
耗时：首次 1123 ms -> 缓存命中 0 ms
```

### 18.7 后续可扩展

| 方向 | 说明 |
|---|---|
| **预翻译** | galgame 提前翻后续 N 句，玩家推进时全部命中 |
| 模糊命中 | 仅标点/空格差异时归一化后复用 |
| 缓存导入导出 | 社区共享译文库（注意版权与审核） |
| 淘汰策略 | 按 `hit_count` 而非纯时间，保留高频句 |

---

## 19. 资产清单与 ed25519 验签（v0.3）

### 19.1 为什么先做清单，而不是先做本地翻译

本地 MT / 本地 LLM 的代码量都在 ①② 之下：

| 积木 | 内容 | 谁需要 |
|---|---|---|
| **① 资产系统**（本节） | 清单 + 验签 + 下载 + 安装 | **local-mt 和本地 LLM 都要** |
| ② 推理运行时 | onnxruntime / llama-server 拉起 | 各自 |
| ③ 翻译逻辑 | 语言对路由、中转 | local-mt 独有 |

先把 ① 做完，两个引擎共用一套底座。

### 19.2 清单信封格式

```jsonc
{
  "schema": "studious-manifest/1",
  "keyId": "studious-dev",
  "payload": { "schemaVersion": 1, "generatedAt": "...", "channel": "stable", "assets": [...] },
  "sig": "<128 位 hex>"
}
```

**签名覆盖 `QJsonDocument(payload).toJson(Compact)` 的字节**。

为什么这样定：
- `QJsonObject` 内部按 key 排序 → 序列化确定性有保证
- 签名只覆盖 payload，信封字段（如 `keyId`）可自由扩展
- 但**清单里的数字一律写整数**，避免浮点格式化抖动导致签名对不上

### 19.3 信任链

```
编译进二进制的公钥（TrustKeys.h）
        │ 验证
        ▼
远程 manifest.json（ed25519 签名）
        │ 内含每个资产的 sha256
        ▼
下载的 zip / gguf → sha256 校验 → （runtime）独立签名校验
```

**只有 hash 没有签名是不安全的**：攻击者同时改文件与清单里的 hash 即可通过。

### 19.4 结构校验（8 条，全部中文 error）

| # | 规则 |
|---|---|
| 1 | 顶层 JSON 合法、`payload` 是 object、`sig` 是 128 位 hex |
| 2 | `requireSignature` 时必填 sig 并用受信公钥验签 |
| 3 | `schema == "studious-manifest/1"` |
| 4 | `schemaVersion` 在 `(0, maxSchemaVersion]`，否则提示"客户端版本过旧" |
| 5 | 资产 `id` 非空且**全局唯一**；`type ∈ {runtime, model}` |
| 6 | **有 urls 的资产 sha256 必须是 64 位小写 hex**（无 urls = 占位，可为空） |
| 7 | urls 必须 `https://` 开头，拒绝明文 http |
| 8 | `size >= 0` |

### 19.5 加密选型

| 项 | 决定 |
|---|---|
| 库 | **Monocypher 4.0.2**（单文件、无依赖、BSD-2-Clause OR CC0-1.0） |
| 算法 | **monocypher-ed25519（RFC 8032 兼容）** |
| 为什么不用 `crypto_eddsa_*` | 那是 BLAKE2b 变体，**不与标准 Ed25519 互通**；用 RFC 变体才能让 openssl/python/libsodium 也能签 |
| 签名字节 | 64；公钥 32；私钥 seed 32 |

⚠️ **`crypto_ed25519_key_pair()` 会把传入的 seed 数组清零**（内部 `crypto_wipe`），所以要在调用前先把 seed hex 抄走 —— 这是踩过的坑。

### 19.6 开发工具 `manifest_tool`

```
manifest_tool genkey                                   # 生成 seed + pubkey
manifest_tool sign <payload.json> <seedHex> <out.json> # 签名，输出完整信封
manifest_tool verify <manifest.json> [pubkeyHex]       # 不带 pubkey 时只做结构校验
```

不随产品分发，仅发布/CI 用。

### 19.7 密钥管理铁律

| 项 | 要求 |
|---|---|
| 私钥（seed） | **离线保存，绝不进仓库**（已加检查：仓库内 grep seed 无命中） |
| 公钥 | 公开无妨，编译进 `TrustKeys.h` |
| 换钥 | 需发新版客户端，所以 `keyId` 支持将来多密钥轮换 |
| 当前密钥 | `studious-dev`，**发版前必须重新生成** |

### 19.8 自测覆盖（`--manifest`，11 个用例）

`manifestValid` / `manifestTampered` / `manifestMissingSig` / `manifestBadSchema` /
`manifestDupId` / `manifestHttpUrl` / `manifestBadSha` / `sha256FileOk` / `sha256FileTampered`
/ `manifestTestKey` / `manifestTrustKey`

篡改用例是核心：签名后改任一字段，验签必须失败。

### 19.9 已知取舍

`ManifestVerifyOptions` 有 `allowMissingSha256`（**默认 false**）。
原因：示例 `manifest.json` 里模型条目有 urls 但 sha256 待填。发布路径永远不要开这个开关——否则等于给占位哈希开后门。只有 `manifest_tool verify` 不带公钥的开发分支会开。

---

## 20. 资产下载 / 安装 / 删除（v0.4）

### 20.1 为什么要先做"流式 HTTP"

原来的 `IHttpClient::send()` 是一次性把整个响应读进内存。GB 级模型这样用会直接吃掉几 GB 内存，而且**没法做进度条、没法断点续传**。

所以先给端口加流式能力：

```cpp
struct HttpStreamCallbacks {
    std::function<bool(int status, qint64 contentLength)> onStart;   // false = 拒收
    std::function<bool(const QByteArray &chunk, qint64 receivedTotal)> onData;  // false = 中止
};
virtual HttpResponse sendStream(const HttpRequest &, const HttpStreamCallbacks &);
```

`IHttpClient` 提供默认实现（退化为整体读取），`QtHttpClient` 覆写为真流式。

### 20.2 下载流程

```
download(assetId)
  ├─ 清单里查资产；sha256 为空（占位）→ 直接报"尚未发布正式哈希"
  ├─ 目标 .part = <root>/downloads/<id>.part
  ├─ 按 urls 顺序尝试（多源 failover）
  │    ├─ .part 已有 N 字节 → 发 Range: bytes=N-
  │    ├─ 206 → 追加写
  │    ├─ 200 → 服务端不支持续传 → truncate 到 0 重写
  │    └─ 416 → .part 已完整 → 直接进校验
  ├─ 每收一块（64KiB）落盘 + 回调 progress
  ├─ 下载完 → sha256 比对
  │    └─ 不符 → 删 .part + 报错（带实际/期望哈希）
  └─ 安装：zip → 解压到 <target>.tmp → rename；单文件 → 直接 rename
       └─ 成功后 registry.markInstalled() + 删 .part
```

### 20.3 四个必须踩对的细节

| # | 细节 | 踩错的后果 |
|---|---|---|
| 1 | `onStart` 返回 false 后**一个字节都不能写文件** | 404 的 HTML 混进 .part，最后报"哈希不匹配"，排查方向全错 |
| 2 | 流式**不能用 `setTransferTimeout()`** | GB 级下载必被误杀；改成"每收一块重置的空闲超时" |
| 3 | 续传必须区分 206/200/416 三分支 | 200 分支不 truncate 会写出"旧+新"混合文件 |
| 4 | 请求带 `Accept-Encoding: identity` | 服务器 gzip 后长度与内容全错 |

### 20.4 zip-slip 三道闸

恶意 zip 能写到目标目录之外（也顺手防 zip bomb）：

1. 条目名含 `..` 路径段 → 拒绝
2. 绝对路径 / 带盘符（`C:\`）→ 拒绝
3. 符号链接条目（`m_external_attr` 高位 `S_IFLNK`）→ 拒绝

解压到 `<target>.tmp`，全部成功才 `rename`；失败把 `.tmp` 删干净。

实测：`../evil.txt` 被拒绝，无文件逃逸。

### 20.5 miniz 选型

| 项 | 说明 |
|---|---|
| 版本 | 3.0.2 **官方 release 的 amalgamation 单文件版** |
| 为什么不用 master | master 已拆成 4 个 .c + 若干头，且需要 CMake 生成 `miniz_export.h` / `miniz_common.h`，直接源码引入编译不过 |
| 用哪个 API | 回调式 `mz_zip_reader_init` + `m_pRead` + `mz_zip_reader_extract_to_callback` |
| 为什么 | `mz_zip_reader_init_file()` 吃 `char*` 本地编码，**中文用户名路径会打不开**；回调式还能避免整包进内存 |

### 20.6 删除（卸载）

```cpp
bool uninstall(const QString &id, QString *error = nullptr);   // 幂等：不存在也返回 true
```

要求：
- 递归删除 `<root>/{models,runtime}/<id>` 与 `.part`
- 删完 `refresh()` 状态回 `NotInstalled`
- **幂等**：重复调用不报错（UI 上连点两次不应该弹错误）

**为什么删除必须和下载一起做**：资产是 GB 级，用户装错了模型、或磁盘紧张时要能立刻清掉；没有删除的下载器等于只能装不能卸。

### 20.7 状态判定

| 状态 | 判据 |
|---|---|
| `Installed` | 目标路径存在且非空 |
| `Corrupted` | 路径存在，但缺失清单 `files` 里声明的关键文件 |
| `NotInstalled` | 其它 |

`Corrupted` 是必要的：用户手工删了目录里一个 dll，程序不能还认为"已安装"然后启动失败。

### 20.8 错误提示中英双语

新增 `i18n/L10n.h`：

```cpp
enum class Language { Chinese, English };
Language current();                       // 默认跟 QLocale::system()，不读配置
void setLanguage(Language);               // 前端设置页切语言
QString s(const char *zh, const char *en);
QString sf(const char *zh, const char *en, const QVariantList &args);  // %1 %2
```

**所有面向用户的 error / reason / note 都走 `i18n::s()`**，调用方直接显示，不再二次判断语言。

一个坑：`sf()` **不能**用 `QString::arg()` 逐个替换 —— 参数里的 `%20`（URL 编码）会被当成占位符二次替换。改成单遍扫描。

另一个坑：`WebProvider` 的来源表原来是 `static const`，加 i18n 后必须每次重建，否则语言会被"烤死"在第一次调用的结果里。

### 20.9 自测（`--assets`，11 个用例）

用 **进程内 QTcpServer 起临时 HTTP 服务**做确定性测试（不依赖外网）：

| 用例 | 断言 |
|---|---|
| `assetsDownloadOk` | 512KB 下载成功、9 次进度回调、sha256 一致 |
| `assetsShaMismatch` | 失败且 `.part` 被清理 |
| `assetsResume` | 预置半截 `.part`，服务器收到 1 次 `Range`，最终内容正确 |
| `assetsFailover` | urls = [坏端口, 本地服务] → 最终 `sourceIndex=1` |
| `assetsCancel` | 取消后 `.part` 保留 |
| `assetsZipInstall` / `assetsZipSlip` | 正常解压 / `../evil.txt` 被拒 |
| `assetsInstallFromFile` | 手动导入 |
| `assetsCorrupted` | 删关键文件后 `refresh()` → `Corrupted` |
| `assetsUninstall` | 删除后 `NotInstalled`，重复删除仍 true |
| `assetsStatusList` / `assetsI18n` | 列表与双语 |
