# Studious 待做清单（自定义接口 / 可配置项）

> 这份清单只记「还没做成用户可调设置」的东西。已经做完的写在 README 与 `_log.md` 里。
> 状态列：`已实现` = 代码在跑但入口还没做；`待接线` = 后端/平台有接口、界面没用上；`未开始` = 两边都没有。

## 1. 截屏翻译 · 放大镜

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 放大倍率 | 已实现 | `AppSettings::overlayZoom`，键 `settings/overlay/zoom`，范围 2–8，默认 3；`AppBackend::overlayZoom` 已暴露给 QML | 在「设置 → 截图」分区加档位选择（2× / 3× / 4× / 6× / 8×），或滑杆 |
| 放大镜位置 | 未开始 | 目前写死「光标右下角，贴边自动翻折」 | 加四项枚举 `bottomRight` / `bottomLeft` / `topRight` / `topLeft`，键 `settings/overlay/magnifierCorner`；QML 里 `magnifier.x/y` 按枚举算，贴边仍要翻折 |
| 放大镜开关 | 未开始 | 始终开启（拖框时出现） | 加 `bool overlayMagnifier = true`，键 `settings/overlay/magnifier`；关掉时选区只显示尺寸标签 |
| 放大镜尺寸 | 未开始 | `zoomSize` 写死 104px | 若要跟字号联动可并入上一项；不建议单独暴露 |
| 取样说明 | 未开始 | 只显示 X/Y、色块、`#RRGGBB`、`R G B` | 可选加 `HSL` / 复制色值按钮 |

## 2. 截屏翻译 · 浮层外观

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 底色不透明度 | 已实现 | `AppSettings::overlayOpacity`，键 `settings/overlay/opacity`，40–100，默认 96 | 设置页滑杆 |
| 译文字号 | 已实现 | `AppSettings::overlayFontSize`，键 `settings/overlay/fontSize`，10–20，默认 13 | 设置页滑杆 |
| 保留原文标注 | 已实现 | `AppSettings::overlayKeepOriginal`，键 `settings/overlay/keepOriginal` | 设置页开关 |
| 选完直接翻译 | 已实现 | `AppSettings::captureAutoRun`，键 `settings/capture/autoRunOnSelect` | 设置页开关（当前只在注册表里改） |
| 自动复制译文 | 已实现 | `AppSettings::captureAutoCopy`，键 `settings/capture/autoCopy` | 设置页开关 |
| 翻译完自动关闭 | 待接线 | `AppSettings::captureAutoHide` 只落盘，`ScreenFlow` 没读 | 翻译成功后延迟 N 秒收起浮层 |
| 遮罩压暗程度 | 未开始 | 写死 `Qt.rgba(0,0,0,0.45)` | 加 `overlayDim`（20–80） |
| 选区边框粗细 / 颜色 | 未开始 | 2px + `Theme.primary` | 一般不需要暴露，跟随强调色即可 |

## 3. 截屏翻译 · 操作与热键

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 截图热键 | 已实现 | `AppSettings::hotkeyCapture`，默认 `Ctrl+Alt+S` | 「快捷键」页做成录制控件（现在整页是占位） |
| 小窗热键 | 已实现 | `hotkeyMini`，默认 `Ctrl+Alt+W` | 同上 |
| 实时热键 | 已实现 | `hotkeyLive`，默认 `Ctrl+Alt+L` | 同上 |
| 二次调整最小尺寸 | 未开始 | 写死 12px | 一般不需要暴露 |
| 方向键微调 | 待接线 | 设计稿 A2 有「方向键微调 / Shift 快移」 | `CaptureOverlay` 加 `Keys.onPressed` 处理四周方向键 |

## 4. 小窗翻译（B 页，未开工）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 窗口尺寸 | 未开始 | 设计稿 `min(560px, 93%)`、`max-height 558` | `AppSettings` 存 `miniWidth/miniHeight` |
| 置顶 | 已实现 | `AppSettings::miniAlwaysOnTop`，默认 true | 小窗做好后接线 |
| 目标语言多选 | 已实现（后端） | `dstLangs` 本身是数组；`QuickTranslate::translate(text, src, dsts)` 支持多目标 | 小窗里做带复选框的菜单 |
| 注音（ruby） | 未开始 | 设计稿 B4 的「显示注音」 | 需要后端判断语言（日语注音）；先留开关位 |

## 5. 实时翻译（C 页，未开工）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 刷新间隔 | 已实现 | `AppSettings::liveIntervalMs`，400–1500，默认 600 | 设置页滑杆 |
| 锁定本地引擎 | 已实现 | `AppSettings::liveEngineLocked`，默认 true | 设置页开关 |
| 控制条位置 | 未开始 | 设计稿里可拖动 | 记住最后一次位置到 `settings/live/ctrlPos` |
| 译文行数上限 | 未开始 | 设计稿 C8「译文过长」内滚 | 加 `liveMaxLines` |

## 6. 记录（B7）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 保存内容 | 已实现 | `AppSettings::recordSaveMode` = `original` / `current` | 二级设置里单选 |
| 条数上限 | 未开始 | `TranslatorRecords` 写死 2000 条 | 加 `recordsMax`（500–10000） |
| 缩略图 | 未开始 | 只存 `imagePath`，删记录不删图 | 加「同时删除图片」开关 + 定期清理 |

## 7. 引擎与网络

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 引擎优先级 | 已实现 | `AppSettings::enginePriority` + 配置页拖动排序 | 无 |
| Web 来源 | 已实现 | `webProviderId` 单值 | 若要「多来源排序」需改后端 `TranslateRequest.webProviders` 语义 |
| 全局 HTTP 超时 | 已实现 | `httpTimeoutMs` → `STUDIOUS_HTTP_TIMEOUT`（只抬下限） | 设置页已有入口 |
| 代理 | 已实现 | `proxyUrl` → `STUDIOUS_HTTP_PROXY`，回环自动直连 | 设置页已有入口 |

## 8. 资产与 OCR

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| OCR 模型下载 | 待接线 | `manifest` 里 `ocr.det/rec/dict.ppocrv4` 三条 `sha256` 与 `urls` 故意留空，`Downloader` 会拒装 | 找到可发布的 PaddleOCR-ONNX 权重后补地址与哈希 |
| OCR 语言 | 未开始 | `OcrOptions::languages` 有字段，界面没入口 | 加 `ocr/languages`（中文 / 英文 / 中英） |
| OCR 检测阈值 | 未开始 | `detThreshold=0.3`、`boxThreshold=0.5`、`unclipRatio=1.6` 写死默认 | 只在排错时需要，可放「高级」里 |

## 9. 平台层（Linux 预留）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 抓屏 | Windows 已实现 / Linux 未开始 | `ScreenCaptureWin` GDI；X11 需 XComposite，Wayland 只能走 portal | 先做 X11 的 `XGetImage` + 兜底提示 |
| 全局热键 | Windows 已实现 / Linux 未开始 | `HotkeyWin`；X11 用 `XGrabKey`，Wayland 无解 | 同上 |
| 浮层定位 / 截图排除 | Windows 已实现 / Linux 未开始 | `OverlayWin`；X11 无 `WDA_EXCLUDEFROMCAPTURE` 等价物 | 只做「不被自己拍到」的近似方案 |
| 孤儿进程兜底 | Windows 已实现 / Linux 未开始 | `ProcessGuard` 用 Job Object | 换 `prctl(PR_SET_PDEATHSIG)` |
