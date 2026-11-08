// 平台层 — Windows 屏幕抓取实现（GDI / BitBlt 主路径）。
//
// ── 为什么是 GDI ──────────────────────────────────────────────────────────────
// · BitBlt(SRCCOPY | CAPTUREBLT) 抓的是 DWM 合成后的结果，能拍到分层窗口（透明/圆角
//   /阴影那类）；CAPTUREBLT 不能省，否则抓到的是「没有分层窗口」的残缺画面。
// · 一次调用只建一个 DC + 一个 DIB，一次 BitBlt 拿一整块，再逐行 memcpy 成 QImage：
//   4K 全屏（3840×2160×4B ≈ 33MB）在 30ms 量级。绝不逐像素 setPixel。
// · 光标天然不进图（BitBlt 不含光标）。不要再补 DrawIconEx —— 设计稿没有光标。
//
// ── GDI 覆盖不了的两类场景（已知缺口，不要假装能抓）──────────────────────────
// ① 独占全屏：游戏/DirectX flip 独占时桌面合成被绕过，BitBlt 常拿到全黑。
// ② HDR 输出：DXGI 输出是 scRGB / PQ，GDI 拿到的往往全黑或过曝。
// 目前策略：BitBlt 拿到全黑时先用 DXGI Output Duplication 兜底重抓一帧
// （见 ScreenCaptureDxgi.cpp），还拿不到才返回 Failed + 中文提示。
// 不做静默降级 —— 宁可让上层提示用户，也不要把一张黑图当成功。
//
// TODO(dxgi): 在这两个场景加 DXGI Output Duplication 兜底：
//     IDXGIFactory1::EnumAdapters → IDXGIOutput1::DuplicateOutput
//     → AcquireNextFrame → CopyResource 到 staging texture（必须 GPU 侧拷贝，
//       CPU 直接 Map 全屏纹理太慢）→ Map → 逐行拷进 QImage。
//   需要 d3d11 + dxgi1_2（Qt 6 的 mingw 构建不一定带 d3d11 导入库，建议单独 TU，
//   或全部 GetProcAddress 运行时加载）。本文件现在只留这个锚点，不链任何 D3D 头。
//
// ── DPI ─────────────────────────────────────────────────────────────────────
// 全程物理像素。调用线程临时切到 PER_MONITOR_AWARE_V2（SetThreadDpiAwarenessContext，
// Win10 1607+ 动态解析），保证即使宿主没在 QApplication 之前调 initProcessDpi()，
// GetSystemMetrics / EnumDisplayMonitors / BitBlt 也在同一套物理坐标里；调用结束复原。
// 这样「虚拟桌面原点」在三种进程 DPI 感知模式下都一致，不会出现主屏和副屏错位。

#include "platform/ScreenCaptureDxgi.h"
#include "platform/ScreenCaptureWin.h"

#include <QtGlobal>

#if defined(Q_OS_WIN)

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

#  include <cstring>

namespace studious::platform {
namespace {

// ---------------------------------------------------------------------------
// 错误文案（中文、可直接显示）。Win32 错误码一律走 GetLastError()。
// ---------------------------------------------------------------------------
QString errText(const QString &what, DWORD code)
{
    return QStringLiteral("%1（错误码 %2）").arg(what, QString::number(static_cast<qulonglong>(code)));
}

CaptureResult failedCapture(const QString &why)
{
    CaptureResult r;
    r.status = PlatformStatus::Failed;
    r.error = why;
    r.devicePixelRatio = 1.0;
    return r;
}

// ---------------------------------------------------------------------------
// 线程级 DPI 感知作用域。
//
// 只改调用线程、出作用域还原；不碰进程默认值（那是 IPlatform::initProcessDpi 的地盘）。
// 用 HANDLE 而不是 DPI_AWARENESS_CONTEXT 声明函数指针，避免老 SDK / 老 mingw 头里
// 该 typedef 不存在导致编译不过 —— DPI_AWARENESS_CONTEXT 本身就是个 HANDLE 句柄。
// ---------------------------------------------------------------------------
class ThreadDpiScope
{
public:
    ThreadDpiScope()
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32)
            return;
        m_set = reinterpret_cast<SetFn>(reinterpret_cast<void *>(
            GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
        if (!m_set)
            return; // Win10 1607 以前：没有线程级 API，保持进程默认行为

        auto get = reinterpret_cast<GetFn>(reinterpret_cast<void *>(
            GetProcAddress(user32, "GetThreadDpiAwarenessContext")));
        m_previous = get ? get() : nullptr;

        // PER_MONITOR_AWARE_V2 == (HANDLE)-4，PER_MONITOR_AWARE == (HANDLE)-3
        if (!m_set(dpiContext(-4)) && !m_set(dpiContext(-3)))
            m_previous = nullptr; // 一次都没切成功：没什么可还原的
    }

    ~ThreadDpiScope()
    {
        if (m_set && m_previous)
            m_set(m_previous);
    }

    ThreadDpiScope(const ThreadDpiScope &) = delete;
    ThreadDpiScope &operator=(const ThreadDpiScope &) = delete;

private:
    using SetFn = HANDLE(WINAPI *)(HANDLE);
    using GetFn = HANDLE(WINAPI *)();

    static HANDLE dpiContext(LONG_PTR raw) { return reinterpret_cast<HANDLE>(raw); }

    SetFn m_set = nullptr;
    HANDLE m_previous = nullptr;
};

// ---------------------------------------------------------------------------
// 每显示器 DPI：shcore.dll 的 GetDpiForMonitor 是 Win8.1 之后才有的导出，
// 直接静态链接会让老系统加载失败，所以 LoadLibraryW + GetProcAddress 动态取。
// 取不到（或调用失败）一律回落 96 == devicePixelRatio 1.0。
// ---------------------------------------------------------------------------
constexpr int kMdtEffectiveDpi = 0; // MONITOR_DPI_TYPE::MDT_EFFECTIVE_DPI
constexpr UINT kFallbackDpi = 96;

using GetDpiForMonitorFn = LONG(WINAPI *)(HMONITOR, int, UINT *, UINT *);

GetDpiForMonitorFn getDpiForMonitorFn()
{
    // 函数局部静态：C++11 起线程安全，只解析一次。
    // 故意不 FreeLibrary：进程活多久模块就留多久，避免在别的线程调用中途被卸载。
    static const GetDpiForMonitorFn fn = []() -> GetDpiForMonitorFn {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (!shcore)
            return nullptr;
        return reinterpret_cast<GetDpiForMonitorFn>(
            reinterpret_cast<void *>(GetProcAddress(shcore, "GetDpiForMonitor")));
    }();
    return fn;
}

UINT dpiForMonitor(HMONITOR mon)
{
    const GetDpiForMonitorFn fn = getDpiForMonitorFn();
    if (!fn)
        return kFallbackDpi;
    UINT dpiX = kFallbackDpi;
    UINT dpiY = kFallbackDpi;
    if (FAILED(fn(mon, kMdtEffectiveDpi, &dpiX, &dpiY)) || dpiX == 0)
        return kFallbackDpi;
    return dpiX;
}

// ---------------------------------------------------------------------------
// 显示器枚举（一次枚举同时服务 winMonitors() 与 devicePixelRatioForRect()）。
// ---------------------------------------------------------------------------
struct MonitorRaw {
    QString device;
    QRect geometry;   // rcMonitor，物理像素
    QRect work;       // rcWork：扣掉任务栏与停靠栏后的可用区，物理像素
    UINT dpi = kFallbackDpi;
    bool primary = false;
};

struct EnumContext {
    QVector<MonitorRaw> *out = nullptr;
};

BOOL CALLBACK monitorProc(HMONITOR mon, HDC, LPRECT, LPARAM param)
{
    auto *ctx = reinterpret_cast<EnumContext *>(param);
    MONITORINFOEXW info {};
    info.cbSize = sizeof(MONITORINFOEXW); // 注意是 EX 结构体的尺寸，否则 szDevice 拿不到
    if (!GetMonitorInfoW(mon, &info))
        return TRUE; // 单个屏读失败就跳过，别让整次枚举失败

    // rcWork（可用区，去掉任务栏）照样读出来，填进 MonitorInfo::workRect。
    const RECT work = info.rcWork;

    MonitorRaw m;
    m.device = QString::fromWCharArray(info.szDevice);
    m.geometry = QRect(info.rcMonitor.left,
                       info.rcMonitor.top,
                       info.rcMonitor.right - info.rcMonitor.left,
                       info.rcMonitor.bottom - info.rcMonitor.top);
    // rcWork 是「可用区」：任务栏、被停靠的工具栏都扣掉了。浮层定位要用它，否则
    // 译文会压到任务栏底下（物理像素，与 geometry 同一坐标系）。
    m.work = QRect(work.left,
                   work.top,
                   work.right - work.left,
                   work.bottom - work.top);
    m.dpi = dpiForMonitor(mon);
    m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    ctx->out->push_back(m);
    return TRUE;
}

QVector<MonitorRaw> enumerateMonitors()
{
    QVector<MonitorRaw> result;
    EnumContext ctx { &result };
    // 传 nullptr 的 HDC：枚举全系统显示器，不需要 DC；矩形参数对我们无用。
    EnumDisplayMonitors(nullptr, nullptr, &monitorProc, reinterpret_cast<LPARAM>(&ctx));
    return result;
}

QRect virtualDesktopRectRaw()
{
    // 四个 metric 必须在同一线程、同一 DPI 感知上下文中取，否则原点与尺寸会来自不同坐标系。
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0)
        return {};
    return QRect(x, y, w, h);
}

// 该区域所在屏幕的 devicePixelRatio。跨了不同 DPI 的屏幕 → 1.0（没有单一缩放）。
qreal devicePixelRatioForRect(const QRect &rect)
{
    const QVector<MonitorRaw> monitors = enumerateMonitors();
    if (monitors.isEmpty())
        return 1.0;

    // 取交集面积最大的那块屏作为「这张图属于谁」。
    qreal dominant = 1.0;
    qint64 bestArea = -1;
    for (const MonitorRaw &m : monitors) {
        const QRect inter = m.geometry.intersected(rect);
        if (inter.isEmpty())
            continue;
        const qint64 area = static_cast<qint64>(inter.width()) * inter.height();
        if (area > bestArea) {
            bestArea = area;
            dominant = m.dpi / 96.0;
        }
    }
    if (bestArea < 0)
        return 1.0;

    for (const MonitorRaw &m : monitors) {
        if (m.geometry.intersected(rect).isEmpty())
            continue;
        if (qAbs(m.dpi / 96.0 - dominant) > 0.01)
            return 1.0; // 混合 DPI：交给调用方按 winMonitors() 逐屏处理
    }
    return dominant;
}

// 全黑判定：抽样检查（不逐像素扫，4K 也就几十微秒）。
// 判据是「所有采样点的 RGB 精确等于 0」——正常桌面/窗口即使很暗也几乎不可能全零。
// 命中就说明这是 GDI 抓不到的独占全屏 / HDR / 锁屏会话，走 Failed 让上层提示用户。
bool looksAllBlack(const QImage &image)
{
    if (image.isNull())
        return false;
    const int stepX = qMax(1, image.width() / 64);
    const int stepY = qMax(1, image.height() / 64);
    for (int y = 0; y < image.height(); y += stepY) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += stepX) {
            if ((line[x] & 0x00ffffffu) != 0u)
                return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// GDI 句柄的 RAII。顺序有讲究：
//   DIB 必须先 SelectObject 还原（deselect）才能 DeleteObject，DC 才能 DeleteDC。
// ---------------------------------------------------------------------------
struct GdiCapture
{
    HDC screenDc = nullptr;   // GetDC(nullptr) 拿的，用 ReleaseDC 还
    HDC memDc = nullptr;      // CreateCompatibleDC 建的，用 DeleteDC 销毁
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;

    ~GdiCapture()
    {
        // HGDI_ERROR 不是可用的旧句柄（SelectObject 失败时就是它），拿它回 Select 只会再错一次。
        if (memDc && oldBitmap && oldBitmap != HGDI_ERROR)
            SelectObject(memDc, oldBitmap);
        if (dib)
            DeleteObject(dib);
        if (memDc)
            DeleteDC(memDc);
        if (screenDc)
            ReleaseDC(nullptr, screenDc);
    }

    GdiCapture() = default;
    GdiCapture(const GdiCapture &) = delete;
    GdiCapture &operator=(const GdiCapture &) = delete;
};

// 核心抓取。rect 必须已经是「与虚拟桌面求交后」的非空矩形。
CaptureResult capturePhysical(const QRect &target)
{
    const int width = target.width();
    const int height = target.height();

    GdiCapture g;

    // 一次调用只建一套 DC/DIB；screenDc 覆盖整个虚拟桌面（所以副屏可以传负坐标）。
    g.screenDc = GetDC(nullptr);
    if (!g.screenDc)
        return failedCapture(errText(QStringLiteral("抓屏失败：GetDC 返回空"), GetLastError()));

    g.memDc = CreateCompatibleDC(g.screenDc);
    if (!g.memDc)
        return failedCapture(errText(QStringLiteral("抓屏失败：CreateCompatibleDC 失败"), GetLastError()));

    // 32bpp top-down DIB：负高度 = 行序从顶到底，拷进 QImage 时不用翻转。
    // BI_RGB + 32bpp 的字节序在内存里就是 B,G,R,X（小端 0x00RRGGBB），
    // 正好对上 QImage::Format_RGB32 的 0xffRRGGBB（alpha 字节无效，后面有说明）。
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    g.dib = CreateDIBSection(g.screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g.dib || !bits)
        return failedCapture(errText(QStringLiteral("抓屏失败：CreateDIBSection 失败"), GetLastError()));

    // SelectObject 失败时返回 NULL 或 HGDI_ERROR((HGDIOBJ)-1)，两者都不是可用的旧句柄：
    // 记到 g.oldBitmap 后立刻退出，析构里会跳过回 Select（否则等于拿 -1 再错一次）。
    // 注意 GDI 不保证设置 LastError，错误码只作参考。
    g.oldBitmap = SelectObject(g.memDc, g.dib);
    if (!g.oldBitmap || g.oldBitmap == HGDI_ERROR)
        return failedCapture(errText(QStringLiteral("抓屏失败：SelectObject 失败"), GetLastError()));

    // CAPTUREBLT 必须有：没有它，分层窗口（透明/圆角/带阴影的现代窗口）会整块缺内容。
    const BOOL bltOk = BitBlt(g.memDc, 0, 0, width, height,
                              g.screenDc, target.x(), target.y(),
                              SRCCOPY | CAPTUREBLT);
    if (!bltOk)
        return failedCapture(errText(QStringLiteral("抓屏失败：BitBlt 返回 0"), GetLastError()));

    QImage image(width, height, QImage::Format_RGB32);
    if (image.isNull())
        return failedCapture(QStringLiteral("抓屏失败：内存不足，分配不出 %1×%2 的图").arg(width).arg(height));

    // 逐行 memcpy：QImage 每行可能有 padding（bytesPerLine >= width*4），所以按行来，
    // 每行只拷 width*4 字节。DIB 侧 32bpp 的行距恒为 width*4（本身已 4 字节对齐）。
    const auto *src = static_cast<const uchar *>(bits);
    const qsizetype srcStride = static_cast<qsizetype>(width) * 4;
    const qsizetype rowBytes = srcStride;
    for (int y = 0; y < height; ++y)
        std::memcpy(image.scanLine(y), src + static_cast<qsizetype>(y) * srcStride, static_cast<size_t>(rowBytes));

    // 注意：这里刻意用 Format_RGB32 而不是 ARGB32。GDI 不写 alpha 字节（全 0），
    // 但 RGB32 在 Qt 里就是「不透明 0xffRRGGBB」语义，绘制与 RGB32→ARGB32 转换都会强制
    // alpha=0xff；换成 ARGB32 反而会让整张图变透明。别改。

    // 诊断用：设了 STUDIOUS_FORCE_DXGI 就跳过 BitBlt，直接用屏幕复制抓。
    // 用来确认「黑图是 GDI 读不到硬件平面」而不是别的原因（见 _log.md 的排查记录）。
    if (qEnvironmentVariableIsSet("STUDIOUS_FORCE_DXGI")) {
        QString forcedError;
        const QImage forced = captureWithDxgi(target, &forcedError);
        CaptureResult forcedResult;
        if (!forced.isNull()) {
            forcedResult.status = PlatformStatus::Ok;
            forcedResult.image = forced;
            forcedResult.rect = CaptureRect { target.x(), target.y(), forced.width(), forced.height() };
            forcedResult.devicePixelRatio = devicePixelRatioForRect(target);
            forcedResult.error.clear();
            return forcedResult;
        }
        return failedCapture(QStringLiteral("抓屏失败（强制 DXGI）：%1").arg(forcedError));
    }

    if (looksAllBlack(image)) {
        // GDI 读不到硬件平面（MPO / independent flip 的游戏窗口）时整块是黑的。
        // 换成屏幕复制（DXGI Output Duplication）再抓一次：它复制的是扫描输出。
        QString dxgiError;
        const QImage dxgiImage = captureWithDxgi(target, &dxgiError);
        if (!dxgiImage.isNull()) {
            CaptureResult fallback;
            fallback.status = PlatformStatus::Ok;
            fallback.image = dxgiImage;
            fallback.rect = CaptureRect { target.x(), target.y(), dxgiImage.width(), dxgiImage.height() };
            fallback.devicePixelRatio = devicePixelRatioForRect(target);
            fallback.error.clear();
            return fallback;
        }
        return failedCapture(QStringLiteral(
                                 "抓屏失败：抓到的画面全黑（游戏窗口的硬件平面 / HDR 输出 / 锁屏会话），"
                                 "DXGI 兜底也没成功：%1")
                                 .arg(dxgiError));
    }

    CaptureResult result;
    result.status = PlatformStatus::Ok;
    result.image = image;
    result.rect = CaptureRect { target.x(), target.y(), width, height };
    result.devicePixelRatio = devicePixelRatioForRect(target);
    result.error.clear();
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------

bool winCaptureAvailable()
{
    // 真的试一次屏幕 DC：远程会话 / 服务会话 / 无交互桌面时这里就会失败。
    HDC dc = GetDC(nullptr);
    if (!dc)
        return false;
    ReleaseDC(nullptr, dc);
    return true;
}

QVector<MonitorInfo> winMonitors()
{
    const ThreadDpiScope dpiScope; // 保证 rcMonitor 与 GetSystemMetrics 同一套物理坐标

    QVector<MonitorInfo> result;
    const QVector<MonitorRaw> raw = enumerateMonitors();
    result.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const MonitorRaw &src = raw.at(i);
        MonitorInfo info;
        info.id = static_cast<int>(i);     // 枚举序号；跨调用/插拔后会变，别当稳定 id 存盘
        info.label = src.device;           // "\\.\DISPLAY1"
        info.geometry = src.geometry;      // rcMonitor，物理像素
        info.workRect = src.work;          // rcWork，物理像素
        info.devicePixelRatio = src.dpi / 96.0;
        info.primary = src.primary;
        result.push_back(info);
    }
    return result;
}

QRect winVirtualDesktopRect()
{
    const ThreadDpiScope dpiScope;
    return virtualDesktopRectRaw();
}

CaptureResult winCaptureRect(const QRect &physicalRect)
{
    if (physicalRect.width() <= 0 || physicalRect.height() <= 0) {
        return failedCapture(QStringLiteral("抓屏失败：选区为空（宽高必须大于 0）"));
    }

    const ThreadDpiScope dpiScope;

    const QRect desktop = virtualDesktopRectRaw();
    if (desktop.isEmpty())
        return failedCapture(QStringLiteral("抓屏失败：拿不到虚拟桌面尺寸（可能没有可用显示器）"));

    // 与虚拟桌面求交：跨屏、负坐标、超出边界都在这里被夹到合法范围。
    const QRect target = physicalRect.intersected(desktop);
    if (target.isEmpty())
        return failedCapture(QStringLiteral("选区不在任何屏幕内"));

    return capturePhysical(target);
}

CaptureResult winCaptureVirtualDesktop()
{
    const ThreadDpiScope dpiScope;

    const QRect desktop = virtualDesktopRectRaw();
    if (desktop.isEmpty())
        return failedCapture(QStringLiteral("抓屏失败：拿不到虚拟桌面尺寸（可能没有可用显示器）"));

    // 注意：多屏拼接图的 devicePixelRatio 在混合 DPI 下会是 1.0（没有单一缩放），
    // 见 devicePixelRatioForRect() 的注释。
    return capturePhysical(desktop);
}

} // namespace studious::platform

#else // !Q_OS_WIN

// ---------------------------------------------------------------------------
// 非 Windows：全部 Unsupported。语义与 NullPlatform 一致，调用方据此置灰。
// ---------------------------------------------------------------------------

namespace studious::platform {
namespace {

QString notImplementedReason()
{
    return QStringLiteral("当前平台尚未实现");
}

CaptureResult unsupportedCapture()
{
    CaptureResult r;
    r.status = PlatformStatus::Unsupported;
    r.error = notImplementedReason();
    r.devicePixelRatio = 1.0;
    return r;
}

} // namespace

bool winCaptureAvailable()
{
    return false;
}

QVector<MonitorInfo> winMonitors()
{
    return {};
}

QRect winVirtualDesktopRect()
{
    // 空 QRect = 无法得知，调用方必须当成事实而不是错误（与 NullPlatform 一致）。
    return {};
}

CaptureResult winCaptureRect(const QRect &)
{
    return unsupportedCapture();
}

CaptureResult winCaptureVirtualDesktop()
{
    return unsupportedCapture();
}

} // namespace studious::platform

#endif // Q_OS_WIN
