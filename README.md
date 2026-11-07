<p align="center">
  <img src="frontend/resources/brand.png" alt="Studious" width="112">
</p>

# Studious

跨平台截图翻译工具（Windows 优先）。

> **状态：基础设施已全部落地（当前版本 0.9.9 Erstling）—— 四类翻译引擎、六页桌面界面、
> 截屏 / 小窗 / 实时三种翻译形态、OCR、三分库记录、可取消翻译、平台层与审计加固都已完成。
> 接下来是测试与打磨。**

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
| **桌面界面** | Qt Quick（QML）六页：翻译 / 快捷键 / 浮层 / 配置 / 设置 / 外观 |
| **截屏翻译** | 全局热键框选 → OCR → 译文贴纸浮在原文上；放大镜、八向手柄、方向键微调、选完即译、自动复制、复制后延迟关闭 |
| **小窗翻译** | 置顶浮动小窗：源/目标语言（多选）、回车翻译、逐段复制、记录页签、译文带原文，与其它入口共用同一份语言与引擎 |
| **实时翻译** | 锁定屏幕区域按间隔轮询：抓屏 → OCR → 翻译 → 逐行贴回；控制条可暂停 / 继续 / 锁定选区 / 换引擎 / 换语言，选区可拖拽缩放，慢引擎与连续无字会提示并中断 |
| **OCR** | PP-OCRv4（DBNet 检测 + CRNN/CTC 识别 + 字符表），按空白列切片处理宽行，词级坐标还原成行供贴纸对齐 |
| **翻译记录** | 截屏 / 小窗 / 实时三个库（`capture.db` / `mini.db` / `live.db`），历史开启时自动存档，界面按时间合并展示 |
| **翻译取消** | 真正可打断：随时取消，LLM 逐段补救与重试路径也会立刻停 |
| **引擎配置** | 分段控件分 Web / 云端 LLM / 本地 LLM / 本地 MT，带状态点，与引擎列表联动 |
| **主题** | 由强调色（seed）按 OKLab 混色推导整套配色，明暗两套，可持久化 |
| **桌面集成** | 开机自启（Windows 注册表 / Linux XDG autostart）、最小化到托盘、关闭到托盘、单实例 |
| **平台层** | `frontend/src/platform/` 是唯一的平台接缝：能力接口 + 空实现 + 工厂，`windows.h` 只允许出现在该目录的 `.cpp`（见该目录 README） |
| **数据管理** | 安装根目录迁移、按文件导入资产、清理旧日志、重置配置与缓存 |
| **健壮性** | 失败重试、目标语种校验、复读检测、输入预检、全局错误通知条 |

### 下一步：测试与打磨

基础设施已经齐了，剩下的都是"把边角磨平"和"补测试"，不再是缺功能。

| 项 | 现状 |
|---|---|
| **端到端测试** | 三种翻译形态 + 四类引擎的组合目前靠手动冒烟，缺自动化用例与回归脚本（主线） |
| 主界面翻译入口 | 翻译页双语栏目前只能看结果，原文框旁还没有「开始翻译」按钮 |
| 翻译字号 | 译文栏字号写死，只有全局缩放倍率（`TODO.md` 第 10 节） |
| 快捷键页 | 三组热键已能注册，页面上仍是占位（`HotkeysPage.qml` 1 处「未实现」） |
| 外观页 | 强调色 / 明暗 / 字体 / 缩放已通，还差 6 处（`AppearancePage.qml`） |
| Linux 抓屏 | X11 无 `WDA_EXCLUDEFROMCAPTURE` 等价物，Wayland 三者皆无；实现走 `NullPlatform` |
| 清单验签 | 校验逻辑与工具链已就绪，端到端仍 `requireSignature=false`，待接公钥分发 |
| API Key 存储 | 目前明文存注册表，待换 DPAPI / 本地密钥包装 |

界面上没有接后端的控件会显示橙色「未实现」标记，方便逐项补齐；逐条状态见 [`TODO.md`](TODO.md)。

---

## 架构

前后端分离，为兄弟目录：

```
frontend/   界面层（Qt Quick / QML + 少量 C++ 桥）
            qml/          页面、组件、浮层（overlay/）
            src/          AppBackend（QML 与后端的唯一桥）、主题、设置、抓屏编排
            src/platform/ 平台接缝：抓屏 / 浮层排除 / 全局热键 / 托盘 / 单实例 / 进程兜底
backend/    翻译引擎 + OCR（C++20 + Qt Core，平台无关）
            contract / router / translators / mt / ocr / asset / cache / i18n
```

**边界约定：**

- `backend/` 不含任何平台相关头文件：`windows.h` 只允许出现在 `frontend/src/platform/` 的 `.cpp` 里。
  OCR 在 `backend/src/ocr/`，输入是内存里的 `QImage`，不认识窗口、句柄与显示器。
- QML 只通过 `AppBackend` 访问后端，不直接引用 `studious::*` 类型。
- 平台差异（抓屏 / 热键 / 浮层排除）全部收在 `frontend/src/platform/` 后面，
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

六个页面，默认 1120×720 逻辑像素（1680×1080 @1.5x 缩放），最小 1040×660：

| 页面 | 内容 |
|---|---|
| **翻译（起点页）** | 源/目标语言、引擎切换、三张入口卡片（小窗 / 截图 / 实时，都能用）、引擎状态条、错误通知条 |
| **快捷键** | 全局热键列表（平台层已注册，页面上仍是占位，见 `HotkeysPage.qml`） |
| **浮层** | 三个浮层共用项（放大镜、遮罩压暗、方向键微调）+ 按功能分段（小窗口 / 截图翻译 / 实时翻译）各自的开关 |
| **配置** | 引擎优先级与强制引擎、四类引擎的分段式配置（Web / 云端 LLM / 本地 LLM / 本地 MT）、高级设置折叠区 |
| **设置** | 通用 / 翻译行为 / 缓存 / 网络 / 资产 / 数据 / 截图 / 关于 |
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

模型与运行时通过「设置 → 资产」按需下载（LLM / 本地 MT / 视觉·OCR 三类，支持带进度的下载、断点续传与卸载）。

### 命令行

```powershell
# 文本翻译（不开 GUI）
studious.exe --smoke "Hello, world!" --to zh --engine llm-cloud
#   --engine  自动 | web | llm-cloud | llm-local | local-mt（默认走设置里的优先级）
#   --from <lang> --to <lang>        不给 --to 就用设置里的目标语言
#   --provider <google|mymemory>     Web 来源
#   --cache on|off
#   --local --local-gpu on|off --local-model <file>    本地 LLM

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

### 截屏翻译

默认热键 `Ctrl+Alt+S`（可在「设置 → 通用」改）：框选区域 → 松手后点工具栏的「翻译」（或在浮层「设置」小窗里打开「选完直接翻译」）→ 译文贴纸盖在原文上。
第一次用要先在「设置 → 资产 → 视觉 / OCR」下载 PP-OCRv4 的三件套（约 15 MB）。

### 小窗翻译

翻译页的「小窗口」卡片或热键 `mini` 唤起：粘/输入文本 → `Ctrl+Enter` 翻译，结果按目标语言分页签、可逐段复制；「记录」页签里是三库合并的历史。

### 实时翻译

翻译页的「实时翻译」卡片或热键唤起 → 框选区域 → 点「开始实时翻译」。控制条上能换引擎、换语言、锁定选区、暂停/继续；
「浮层 → 按功能设置 → 实时翻译」里可以调刷新间隔、是否只在内容变化时翻译、区域底色与译文字号。

三个入口共用「配置」页里那一个引擎，以及翻译页那一套源/目标语言（本地 MT 会自动把目标语言收成单选，因为它按语言对装模型）。

---

## 版本信息

版本号 / 渠道名读仓库根的 `version.json`；提交号在构建期从 git 取，Qt 版本与架构运行期取。
发版只需改 `version.json`，再打一个 `vX.Y.Z` 标签。

当前：`0.9.9`，版名 **Erstling**（channel `separate` / 分离版）。

---

## 路线图

三步主线：**小窗翻译 → 截图翻译 → 实时翻译**，均已落地。三者共用同一套平台层、OCR 与语言/引擎设置。

| 阶段 | 事项 | 状态 |
|---|---|---|
| **M1** | 平台层落地：整屏/区域抓图、交互式框选遮罩、结果浮层、全局热键、多屏与高 DPI | ✅ 已做（Windows） |
| **M2** | 小窗翻译：置顶浮动小窗、热键唤起、回车翻译、多目标语言、记录页签 | ✅ 已做 |
| **M3** | OCR：截图 → 文本，返回词级坐标供浮层对齐 | ✅ 已做 |
| **M4** | 截图翻译：热键框选 → OCR → 浮层出译文，可复制、可重识别 | ✅ 已做 |
| **M5** | 实时翻译：锁定屏幕区域轮询 OCR + 翻译，逐行贴回，控制条可暂停/锁定选区 | ✅ 已做 |
| P0 | 测试与打磨：端到端用例、回归脚本、当前 🚧 项 | 进行中 |
| P1 | 快捷键页热键自定义、外观页剩余项、翻译字号、主界面翻译入口 | 待做 |
| P2 | Linux 平台实现（X11 抓图 / xdg-desktop-portal） | 待做 |
| P3 | 预翻译与缓存导入导出 | 待做 |

---

## 许可

[MIT](LICENSE)

第三方组件与字体的许可证见程序内「设置 → 关于 → 第三方声明」。

## 贡献者

见 [`CONTRIBUTORS.md`](CONTRIBUTORS.md)。
