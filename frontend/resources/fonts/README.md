# 内置字体（软件默认-*）

界面和译文都自带字体，不依赖用户机器上装了什么。五个文件都做了**子集化**，
原始字重共 97.2 MB，现在是 **22.4 MB**（界面 2.0 + 译文 6.5 + 等宽 6.3）。

界面字体（思源黑体三份）按**实际用字**裁剪：2,835 字（含 1,308 汉字），三份合计
0.7 MB×3。译文/等宽字体保持宽口径（GB2312 + JIS 汉字 + 韩语音节 + 假名），
因为译文可能是任意文本。

> **加/改界面文案后要重跑一次**：`python frontend/resources/fonts/subset.py --ui`，
> 再把 `_fontbuild/StudiousSans-*.otf` 覆盖回本目录。漏跑不会出方块（Qt 会回落到
> 系统字体），但那几个新字会跟界面其余部分字体不一致。

| 文件 | 字重 | 用途 | 来源（`font/`） | 字符集 |
| --- | --- | --- | --- | --- |
| `StudiousSans-Regular.otf` | Regular | 界面正文 | `UI_OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf` | 实际用字 2,835 |
| `StudiousSans-Medium.otf` | Medium | 分组标题、按钮 | `UI_OTF/SimplifiedChinese/SourceHanSansSC-Medium.otf` | 同上 |
| `StudiousSans-Bold.otf` | Bold | 标题、强调 | `UI_OTF/SimplifiedChinese/SourceHanSansSC-Bold.otf` | 同上 |
| `StudiousWenKai-Regular.ttf` | Regular | 译文正文（贴纸、小窗、实时） | `centent-v1.530/LXGWWenKai-Regular.ttf` | 宽口径 ~14,500 |
| `StudiousWenKaiMono-Regular.ttf` | Regular | 日志正文、浮层尺寸标、热键框、路径 | `centent-v1.530/LXGWWenKaiMono-Regular.ttf` | 宽口径 |

`font/` 里还有这些没进包：思源 ExtraLight / Light / Normal / Heavy，霞鹜 Light / Medium
和 Mono 的 Light / Medium —— 全项目一处都没用到。

PostScript 名（`name` 表 6 号）必须是 ASCII，所以文件里叫 `StudiousSans-Regular` 这类；
用户看到的族名是中文的「软件默认-思源黑体」「软件默认-霞鹜文楷」「软件默认-霞鹜文楷等宽」。

## 子集保留了什么字

脚本：`subset.py`（入库，可重跑）。

**界面字体（`--ui`）**——只看"界面上真的会出现的字"：

- `frontend/qml`、`frontend/src`、`backend/src`、`backend/include`、`frontend/translations`
  里出现过的全部非 ASCII 字符（含后端 `i18n::s/sf` 里的中文、语言名、状态文案）
- 版面整段保留：ASCII、Latin-1、通用标点、箭头/数学/几何/杂项符号、CJK 标点、全角半角
- 当前结果：**2,835 字**（汉字 1,308）

**译文/等宽字体（默认模式）**——要能显示任意语言：

- 拉丁：ASCII、Latin-1、Latin Ext-A
- 符号：General Punctuation、Currency、Arrows、Math Operators、Geometric Shapes、
  Miscellaneous Symbols / Dingbats（`✓ ✕ × ▾ ≈` 这些都在里面）
- 中日韩标点、平假名/片假名、全角半角
- 汉字：**GB2312 全部 6763 字** + **JIS X 0208 的汉字**（约 6000，日文 OCR 原文要用）
- 韩语：**KS X 1001 的 2350 个常用音节**

覆盖自测（子集后逐字查 cmap）：界面用字 0 缺失（2093 个原本存在的字全部保留）；
宽口径下日文样例、繁体样例、韩语样例均 0 缺字。包里没有的字（生僻字、GB18030
扩展、全量谚文）由 Windows 的字体回退兜住 —— 显示成系统字体，不会出现豆腐块。

## 为什么要子集化

五个完整字重是 97.2 MB，编进 qrc 后 exe 从 29 MB 涨到 146 MB。第一次宽口径子集化后
30.4 MB（exe 73 MB），界面字体再按实际用字裁到 2.0 MB 后 exe 约 **57 MB**。
界面截图与子集前逐像素比对差异为 0。

## 许可证

- 思源黑体（Source Han Sans SC）：SIL Open Font License 1.1
- 霞鹜文楷（LXGW WenKai / WenKai Mono）：SIL Open Font License 1.1

`OFL.txt` 是霞鹜文楷随包附带的许可证原文；思源黑体的许可证见
<https://github.com/adobe-fonts/source-han-sans> 的 `LICENSE.txt`。
按 OFL 要求：不得单独售卖字体，改名后再分发时要保留本说明与许可证。

## 加字重 / 换字体

1. 把原文件放进 `font/`（该目录不入库）。
2. 用 fontTools 改写 `name` 表（1/4/16 族名、2/17 字重名、6 PostScript 名必须是 ASCII），
   再用同一个字符集子集化，输出到本目录。
3. 在 `frontend/CMakeLists.txt` 的 `fonts` 资源列表里加一行。
4. 在 `frontend/src/main.cpp` 的 `loadBundledFonts()` 里 `loadFontFamily(":/fonts/…")`。
5. 要出现在外观页下拉里，还得加进 `AppBackend::fontChoices()` 与 `knownFontChoice()`。

> 注意：不要把这些字体挂回 `studious` 目标自身的资源里（`qt_add_resources(studious …)`），
> 否则 `update_translations` 时 lupdate 会去解析这几个几十 MB 的字体文件，直接卡死。
> 现在它们挂在独立静态库 `studious_fonts` 上，就是为了避开这一点。
