pragma Singleton

import QtQuick

// 设计令牌单例。整套调色板由强调色（seed）在 OKLab 空间动态混出来，
// 配方逐项对应 docs/TempUI/index.html 的 color-mix(in oklab, A p%, B)：
//   primary          = seed 原色
//   surface          = color-mix(in oklab, seed 3%, #FDF8FF)
//   primaryContainer = color-mix(in oklab, seed 24%, #FFFFFF)
// 换 seed 之后所有令牌自动重算，不需要重建界面。
//
// 用法：
//   Theme.applyMode("system" | "light" | "dark")
//   Theme.applySeed("#00658F")
//
// 所有令牌都是 `dark ? 暗色 : 浅色` 的三元表达式，不是死值。
QtObject {
    id: theme

    // ---- 主题模式 ----
    property string mode: "system"
    property bool dark: false

    // 系统是否深色。由 main.cpp 注入（QStyleHints 在 QML 侧拿不稳）。
    property bool systemDark: false

    function applyMode(next) {
        theme.mode = next
        theme.dark = next === "dark" ? true
                                     : next === "light" ? false
                                                        : theme.systemDark
    }

    // ---- 强调色（seed）----
    // 非法值一律回落到默认紫 #6750A4（与模板 data-seed 的初始值一致）。
    property color seed: "#6750A4"
    property color defaultSeed: "#6750A4"

    function applySeed(value) {
        var c = Qt.color(value)
        theme.seed = c.valid ? c : theme.defaultSeed
    }

    // ---- OKLab 混色（Björn Ottosson 标准公式）----
    // color-mix(in oklab, A p%, B) 的语义：两色各自转 OKLab，
    // 按 A 的占比 p 做线性插值，再转回 sRGB。
    // sRGB 分量进出一律 0..1，最终 clamp。

    function _clamp01(v) {
        return v < 0 ? 0 : (v > 1 ? 1 : v)
    }

    function _srgbToLinear(c) {
        return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4)
    }

    function _linearToSrgb(c) {
        return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1.0 / 2.4) - 0.055
    }

    // 取 sRGB 分量：参数可能是 QML color，也可能是 "#RRGGBB" 字符串。
    function _rgbOf(c) {
        var col = (typeof c === "string") ? Qt.color(c) : c
        return [col.r, col.g, col.b]
    }

    // 输入 sRGB 0..1，返回 [L, a, b]。
    function _rgbToOklab(r, g, b) {
        var lr = _srgbToLinear(r)
        var lg = _srgbToLinear(g)
        var lb = _srgbToLinear(b)

        var l = 0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb
        var m = 0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb
        var s = 0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb

        var l_ = Math.cbrt(l)
        var m_ = Math.cbrt(m)
        var s_ = Math.cbrt(s)

        return [
            0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
        ]
    }

    // 输入 OKLab，返回 [r, g, b]（sRGB 0..1，已 clamp）。
    function _oklabToRgb(L, A, B) {
        var l_ = L + 0.3963377774 * A + 0.2158037573 * B
        var m_ = L - 0.1055613458 * A - 0.0638541728 * B
        var s_ = L - 0.0894841775 * A - 1.2914855480 * B

        var l = l_ * l_ * l_
        var m = m_ * m_ * m_
        var s = s_ * s_ * s_

        var r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s
        var g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s
        var b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s

        return [
            _linearToSrgb(_clamp01(r)),
            _linearToSrgb(_clamp01(g)),
            _linearToSrgb(_clamp01(b))
        ]
    }

    // weight = 第一色（seed）的占比 0..1，等价于 CSS 的 `a weight*100%, b`。
    function mixOklab(a, b, weight) {
        var w = _clamp01(weight)
        var ca = _rgbOf(a)
        var cb = _rgbOf(b)
        var la = _rgbToOklab(ca[0], ca[1], ca[2])
        var lb = _rgbToOklab(cb[0], cb[1], cb[2])

        var mix = [
            la[0] * w + lb[0] * (1 - w),
            la[1] * w + lb[1] * (1 - w),
            la[2] * w + lb[2] * (1 - w)
        ]
        var rgb = _oklabToRgb(mix[0], mix[1], mix[2])
        return Qt.rgba(rgb[0], rgb[1], rgb[2], 1)
    }

    // ---- 主色 ----
    readonly property color primary: dark ? mixOklab(seed, "#FFFFFF", 0.64) : seed
    readonly property color primaryText: dark ? mixOklab(seed, "#000000", 0.46) : "#FFFFFF"
    readonly property color primaryContainer: dark ? mixOklab(seed, "#0E0D12", 0.46)
                                                   : mixOklab(seed, "#FFFFFF", 0.24)
    readonly property color primaryContainerText: dark ? mixOklab(seed, "#FFFFFF", 0.16)
                                                       : mixOklab(seed, "#000000", 0.74)

    // ---- 表面 ----
    readonly property color surface: dark ? mixOklab(seed, "#100F14", 0.05)
                                          : mixOklab(seed, "#FDF8FF", 0.03)
    readonly property color surfaceDim: dark ? mixOklab(seed, "#0A0910", 0.03)
                                             : mixOklab(seed, "#DED8E6", 0.10)
    readonly property color surfaceLow: dark ? mixOklab(seed, "#17161C", 0.07)
                                             : mixOklab(seed, "#F8F2FC", 0.05)
    readonly property color surfaceMid: dark ? mixOklab(seed, "#1C1B22", 0.09)
                                             : mixOklab(seed, "#F3EDF7", 0.07)
    readonly property color surfaceHigh: dark ? mixOklab(seed, "#232129", 0.12)
                                              : mixOklab(seed, "#ECE6F0", 0.09)
    readonly property color surfaceHighest: dark ? mixOklab(seed, "#2A2831", 0.15)
                                                 : mixOklab(seed, "#E6E0E9", 0.12)

    // 导航栏：背景保持深色（跟内容区同色系），**图标/文字**在深色主题下变亮。
    // 之前理解反了，做成浅底深图标；现在改回深底亮图标。
    readonly property color railSurface: surfaceLow
    readonly property color railIconIdle: dark ? mixOklab(seed, "#E9E4EC", 0.10)
                                               : inkVariant
    readonly property color railIconActive: dark ? mixOklab(seed, "#FFFFFF", 0.16)
                                                 : primaryContainerText

    // ---- 文字与描边 ----
    readonly property color ink: dark ? mixOklab(seed, "#E9E4EC", 0.05)
                                      : mixOklab(seed, "#191A1F", 0.24)
    readonly property color inkVariant: dark ? mixOklab(seed, "#C7C4CE", 0.12)
                                             : mixOklab(seed, "#44464F", 0.15)
    readonly property color outline: dark ? mixOklab(seed, "#8F8B96", 0.22)
                                          : mixOklab(seed, "#767680", 0.18)
    readonly property color outlineVariant: dark ? mixOklab(seed, "#46434D", 0.16)
                                                 : mixOklab(seed, "#C7C5D0", 0.10)

    // ---- 语义色 ----
    readonly property color danger: dark ? "#FFB4AB" : "#B3261E"
    readonly property color dangerText: dark ? "#690005" : "#FFFFFF"
    // 深色错误容器叠在 surface 上：color-mix(in oklab, #93000A 48%, surface)。
    readonly property color dangerContainer: dark ? mixOklab("#93000A", surface, 0.48)
                                                  : "#F9DEDC"
    readonly property color dangerContainerText: dark ? "#FFDAD6" : "#410E0B"
    readonly property color caution: dark ? "#FFC46B" : "#B26A00"
    readonly property color cautionContainer: dark ? mixOklab("#7A4E00", "#17130C", 0.62)
                                                   : "#FFF0D4"
    readonly property color good: dark ? "#7FDCA8" : "#1E6B45"
    readonly property color goodContainer: dark ? mixOklab("#14512F", "#0D1411", 0.70)
                                                : "#DDF2E6"

    // ---- 间距 ----
    readonly property int s1: 4
    readonly property int s2: 8
    readonly property int s3: 12
    readonly property int s4: 16
    readonly property int s5: 20
    readonly property int s6: 24
    readonly property int s7: 32
    readonly property int s8: 40
    readonly property int s9: 56

    // ---- 圆角 ----
    readonly property int rXs: 4
    readonly property int rSm: 8
    readonly property int rMd: 12
    readonly property int rLg: 16
    readonly property int rXl: 24
    readonly property int rFull: 999

    // 是否显示「未实现」标记（components/Stub.qml）。
    // 关掉它所有标记一起消失，不用逐个删。
    property bool showStubs: true

    // ---- 字体 ----
    // 实际族名由 main.cpp 装载后通过 bundledFont* 注入（Main.qml 里 Binding）。
    // 这里的默认值只是字体加载失败时的兜底。
    property string fontUi: "Segoe UI Variable"
    property string fontContent: "Source Han Sans SC"
    property string fontMono: "Cascadia Code"

    // 字号缩放（外观页的滑块）。1.0 = 100%。
    // 下面五个字号都是 `基准 × 缩放`，改这一个值整页字都会跟着变。
    property real fontScale: 1.0

    readonly property int fontSizeDisplay: Math.round(34 * fontScale)
    readonly property int fontSizeSection: Math.round(15 * fontScale)
    readonly property int fontSizeBody: Math.round(13 * fontScale)
    readonly property int fontSizeSmall: Math.round(12 * fontScale)
    readonly property int fontSizeTiny: Math.round(11 * fontScale)

    // ---- 动效 ----
    readonly property int durFast: 150
    readonly property int durBase: 250
}
