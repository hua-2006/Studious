// 平台层 — DXGI Output Duplication 抓屏（GDI BitBlt 读不到硬件平面时的兜底）。
//
// 背景：游戏/CEF 外壳把画面扫到独立的硬件平面（MPO / independent flip）时，
// BitBlt 与 PrintWindow 都只能拿到黑；屏幕复制（DuplicateOutput）复制的正是扫描
// 输出，所以能看到。
//
// 三种常见格式都要能转：
//   · B8G8R8A8_UNORM          —— 普通 SDR 桌面，直接按 BGRA 拷
//   · R16G16B16A16_FLOAT      —— HDR 开着的桌面，scRGB 线性
//   · R10G10B10A2_UNORM       —— HDR + PQ 传输函数（部分驱动走这条）
// 后两种都要先转成 sRGB 8 bit，否则贴纸/OCR 看到的是发灰或全白。
//
// 只用 Win32/DXGI，不依赖 Qt 平台插件；本文件之外不出现任何 d3d 头。

#include "ScreenCaptureDxgi.h"

#include <QDebug>
#include <QScopeGuard>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace studious::platform {
namespace {

template <typename T>
void safeRelease(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// 半精度浮点（IEEE 754 binary16）→ float。scRGB 桌面就存在这种纹理里。
float halfToFloat(quint16 h)
{
    const quint32 sign = (h >> 15) & 0x1u;
    const quint32 exponent = (h >> 10) & 0x1fu;
    const quint32 mantissa = h & 0x3ffu;
    if (exponent == 0)
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 0x1fu)
        return mantissa ? std::nanf("") : (sign ? -INFINITY : INFINITY);
    return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa | 0x400u),
                                               static_cast<int>(exponent) - 25);
}

// 线性光 → sRGB 8 bit。
quint8 encodeSrgb(float linear)
{
    const float v = std::clamp(linear, 0.0f, 1.0f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<quint8>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

// SMPTE ST 2084（PQ）→ 绝对亮度（nits）。
float pqToNits(float pq)
{
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float p = std::pow(std::clamp(pq, 0.0f, 1.0f), 1.0f / m2);
    const float num = std::max(p - c1, 0.0f);
    const float den = c2 - c3 * p;
    if (den <= 0.0f)
        return 0.0f;
    return 10000.0f * std::pow(num / den, 1.0f / m1);
}

// SDR 参考白取 203 nits：桌面 UI 常见的纸白亮度，超过它的部分直接削平（不做过曝保留）。
constexpr float kSdrWhiteNits = 203.0f;

QImage convertFrame(const D3D11_MAPPED_SUBRESOURCE &map, const D3D11_TEXTURE2D_DESC &desc,
                    const QRect &crop)
{
    QImage image(crop.width(), crop.height(), QImage::Format_RGB32);
    if (image.isNull())
        return image;

    const auto *base = static_cast<const uchar *>(map.pData);
    for (int y = 0; y < crop.height(); ++y) {
        const auto *row = base + static_cast<qsizetype>(crop.y() + y) * map.RowPitch;
        auto *out = reinterpret_cast<QRgb *>(image.scanLine(y));

        switch (desc.Format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
            const auto *src = reinterpret_cast<const uchar *>(row) + static_cast<qsizetype>(crop.x()) * 4;
            std::memcpy(out, src, static_cast<size_t>(crop.width()) * 4);
            for (int x = 0; x < crop.width(); ++x)
                out[x] |= 0xff000000u; // DXGI 的 alpha 不可靠，统一按不透明处理
            break;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            const auto *src = reinterpret_cast<const quint16 *>(row) + static_cast<qsizetype>(crop.x()) * 4;
            for (int x = 0; x < crop.width(); ++x) {
                // scRGB 线性，直接映射到 sRGB（超过 1.0 的部分削平）。
                const quint8 r = encodeSrgb(halfToFloat(src[x * 4 + 0]));
                const quint8 g = encodeSrgb(halfToFloat(src[x * 4 + 1]));
                const quint8 b = encodeSrgb(halfToFloat(src[x * 4 + 2]));
                out[x] = qRgb(r, g, b);
            }
            break;
        }
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            const auto *src = reinterpret_cast<const quint32 *>(row) + crop.x();
            for (int x = 0; x < crop.width(); ++x) {
                const quint32 v = src[x];
                const float r = pqToNits(static_cast<float>(v & 0x3ffu) / 1023.0f) / kSdrWhiteNits;
                const float g = pqToNits(static_cast<float>((v >> 10) & 0x3ffu) / 1023.0f) / kSdrWhiteNits;
                const float b = pqToNits(static_cast<float>((v >> 20) & 0x3ffu) / 1023.0f) / kSdrWhiteNits;
                out[x] = qRgb(encodeSrgb(r), encodeSrgb(g), encodeSrgb(b));
            }
            break;
        }
        default:
            return QImage(); // 不认识的格式：交给上层报错，别输出错色
        }
    }
    return image;
}

} // namespace

QImage captureWithDxgi(const QRect &physicalRect, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QImage();
    };

    if (physicalRect.isEmpty())
        return fail(QStringLiteral("DXGI 抓屏：选区为空"));

    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const HRESULT createHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                               D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(createHr) || !device)
        return fail(QStringLiteral("DXGI 抓屏：D3D11 设备创建失败（0x%1）")
                        .arg(static_cast<quint32>(createHr), 8, 16, QLatin1Char('0')));
    auto deviceGuard = qScopeGuard([&]() {
        safeRelease(context);
        safeRelease(device);
    });

    IDXGIDevice *dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgiDevice))))
        return fail(QStringLiteral("DXGI 抓屏：拿不到 IDXGIDevice"));
    auto dxgiDeviceGuard = qScopeGuard([&]() { safeRelease(dxgiDevice); });

    IDXGIAdapter *adapter = nullptr;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter)
        return fail(QStringLiteral("DXGI 抓屏：拿不到显示适配器"));
    auto adapterGuard = qScopeGuard([&]() { safeRelease(adapter); });

    // 找出覆盖选区的那个输出（多显示器时选区可能落在副屏上）。
    IDXGIOutput *matched = nullptr;
    RECT matchedRect {};
    for (UINT i = 0;; ++i) {
        IDXGIOutput *output = nullptr;
        if (adapter->EnumOutputs(i, &output) != S_OK)
            break;
        DXGI_OUTPUT_DESC desc {};
        if (SUCCEEDED(output->GetDesc(&desc))) {
            const RECT &r = desc.DesktopCoordinates;
            if (physicalRect.left() >= r.left && physicalRect.top() >= r.top
                && physicalRect.right() <= r.right && physicalRect.bottom() <= r.bottom) {
                matched = output;
                matchedRect = r;
                break;
            }
        }
        output->Release();
    }
    if (!matched)
        return fail(QStringLiteral("DXGI 抓屏：找不到覆盖选区的显示器"));
    auto outputGuard = qScopeGuard([&]() { safeRelease(matched); });

    IDXGIOutput1 *output1 = nullptr;
    if (FAILED(matched->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&output1))))
        return fail(QStringLiteral("DXGI 抓屏：这块屏幕不支持桌面复制（系统版本太旧？）"));
    auto output1Guard = qScopeGuard([&]() { safeRelease(output1); });

    IDXGIOutputDuplication *duplication = nullptr;
    const HRESULT dupHr = output1->DuplicateOutput(device, &duplication);
    if (FAILED(dupHr) || !duplication) {
        // 常见：DXGI_ERROR_NOT_CURRENTLY_AVAILABLE（别的抓屏程序占满 4 个名额）、
        //       E_ACCESSDENIED（锁屏 / 安全桌面）。
        return fail(QStringLiteral("DXGI 抓屏：桌面复制不可用（0x%1），可能有别的录屏/截图程序正在占用")
                        .arg(static_cast<quint32>(dupHr), 8, 16, QLatin1Char('0')));
    }
    auto dupGuard = qScopeGuard([&]() { safeRelease(duplication); });

    // 旋转在副本描述里（不在每帧的 frameInfo 里）：旋转要额外做像素重排，
    // 选区坐标也会跟着翻，先明确报错，别输出一张方向错的图。
    DXGI_OUTDUPL_DESC duplicationDesc {};
    duplication->GetDesc(&duplicationDesc);
    if (duplicationDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY
        && duplicationDesc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        return fail(QStringLiteral("DXGI 抓屏：显示器旋转方向暂不支持"));

    // 桌面没有变化时 AcquireNextFrame 会超时，多试几次（游戏在动的话第一帧就有）。
    QImage result;
    QString lastError;
    for (int attempt = 0; attempt < 12 && result.isNull(); ++attempt) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo {};
        IDXGIResource *resource = nullptr;
        const HRESULT hr = duplication->AcquireNextFrame(300, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            lastError = QStringLiteral("DXGI 抓屏：画面一直没变化，取不到新帧");
            continue;
        }
        if (FAILED(hr)) {
            lastError = QStringLiteral("DXGI 抓屏：取帧失败（0x%1）")
                            .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
            break;
        }
        auto releaseFrameGuard = qScopeGuard([&]() { duplication->ReleaseFrame(); });
        auto resourceGuard = qScopeGuard([&]() { safeRelease(resource); });

        // LastPresentTime == 0 表示这一帧只是鼠标动了、桌面内容没更新：
        // 这种情况下帧纹理的内容是未定义的（常见表现就是全黑），必须丢掉重取。
        if (frameInfo.LastPresentTime.QuadPart == 0) {
            lastError = QStringLiteral("DXGI 抓屏：只有鼠标更新，还没有新画面");
            continue;
        }

        ID3D11Texture2D *texture = nullptr;
        if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void **>(&texture)))) {
            lastError = QStringLiteral("DXGI 抓屏：拿不到帧的纹理");
            break;
        }
        auto textureGuard = qScopeGuard([&]() { safeRelease(texture); });

        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.ArraySize = 1;
        stagingDesc.MipLevels = 1;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;

        ID3D11Texture2D *staging = nullptr;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging) {
            lastError = QStringLiteral("DXGI 抓屏：建不了回读纹理");
            break;
        }
        auto stagingGuard = qScopeGuard([&]() { safeRelease(staging); });

        context->CopyResource(staging, texture);

        D3D11_MAPPED_SUBRESOURCE map {};
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            lastError = QStringLiteral("DXGI 抓屏：纹理回读失败");
            break;
        }

        // 选区换算成输出内的相对坐标，再和帧尺寸取交集。
        const QRect frameRect(0, 0, static_cast<int>(desc.Width), static_cast<int>(desc.Height));
        const QRect crop = physicalRect.translated(-matchedRect.left, -matchedRect.top)
                              .intersected(frameRect);
        if (crop.isEmpty()) {
            context->Unmap(staging, 0);
            lastError = QStringLiteral("DXGI 抓屏：选区不在这一帧里");
            break;
        }

        result = convertFrame(map, desc, crop);
        context->Unmap(staging, 0);

        if (result.isNull())
            lastError = QStringLiteral("DXGI 抓屏：不支持的像素格式（%1）").arg(static_cast<int>(desc.Format));
    }

    if (result.isNull())
        return fail(lastError.isEmpty() ? QStringLiteral("DXGI 抓屏：没有拿到画面") : lastError);

    if (error)
        error->clear();
    return result;
}

} // namespace studious::platform
