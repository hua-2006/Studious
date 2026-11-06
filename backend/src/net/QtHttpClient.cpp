#include "net/QtHttpClient.h"

#include "i18n/L10n.h"

#include <QEventLoop>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <utility>

namespace studious::net {
namespace {

int statusOf(const QNetworkReply *reply)
{
    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    return statusVar.isValid() ? statusVar.toInt() : 0;
}

// 全局请求超时**下限**（环境变量 STUDIOUS_HTTP_TIMEOUT，毫秒）。
//
// 为什么是"下限"而不是"一把覆盖"：各调用点自己最清楚该等多久（LLM 网关 69 s、
// Web 适配器 5 s、本地 llama-server 探测 5 s），全局覆盖会把它们的判断全抹平。
// 用户的抱怨几乎总是"网慢，5 s 太早掐断"，所以这里只往上抬、不往下压。
//
// 回环地址不受影响：本地 llama-server / 本地网关慢是进程问题不是网络问题，
// 抬高超时只会让"启动失败"晚好几秒才报出来。
int effectiveTimeoutMs(const HttpRequest &req)
{
    if (req.timeoutMs <= 0) {
        return req.timeoutMs;   // 0 = 调用方明确要求不限时
    }

    bool ok = false;
    const int floorMs = qEnvironmentVariableIntValue("STUDIOUS_HTTP_TIMEOUT", &ok);
    if (!ok || floorMs <= 0) {
        return req.timeoutMs;
    }

    const QString host = req.url.host().toLower();
    if (host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost")
        || host == QStringLiteral("::1")) {
        return req.timeoutMs;
    }

    return qMax(req.timeoutMs, floorMs);
}

} // namespace

// 从环境变量读代理配置。
//
// 为什么需要：Qt 默认不走系统代理（而 Windows 的 WinINET 设置经常是关的），
// 于是 GitHub / HuggingFace 这类境外源在国内容易直接超时。
// curl 能过只是因为我们手动传了 --proxy —— 程序自己也得会读。
//
// 优先级：STUDIOUS_HTTP_PROXY（程序专用）> HTTPS_PROXY > HTTP_PROXY > ALL_PROXY。
QNetworkProxy proxyFromEnvironment()
{
    static const char *const names[] = {
        "STUDIOUS_HTTP_PROXY",
        "HTTPS_PROXY", "https_proxy",
        "HTTP_PROXY",  "http_proxy",
        "ALL_PROXY",   "all_proxy",
    };

    for (const char *name : names) {
        const QString value = qEnvironmentVariable(name).trimmed();
        if (value.isEmpty()) {
            continue;
        }

        const QUrl url(value.contains(QStringLiteral("://")) ? value
                                                             : QStringLiteral("http://") + value);
        if (!url.isValid() || url.host().isEmpty()) {
            continue;
        }

        QNetworkProxy proxy(QNetworkProxy::HttpProxy,
                            url.host(),
                            static_cast<quint16>(url.port(8080)));
        if (!url.userName().isEmpty()) {
            proxy.setUser(url.userName());
            proxy.setPassword(url.password());
        }
        return proxy;
    }

    return QNetworkProxy(QNetworkProxy::NoProxy);
}

QtHttpClient::QtHttpClient(QString userAgent, QObject *parent)
    : m_nam(parent)
    , m_userAgent(std::move(userAgent))
{
    if (m_userAgent.isEmpty()) {
        m_userAgent = QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    }

    const QNetworkProxy proxy = proxyFromEnvironment();
    if (proxy.type() != QNetworkProxy::NoProxy) {
        m_nam.setProxy(proxy);
    }
}

QtHttpClient::~QtHttpClient() = default;

QNetworkRequest QtHttpClient::buildRequest(const HttpRequest &req) const
{
    QNetworkRequest nr(req.url);
    // 免费翻译接口常有 301/302，这里跟随同类或更安全的跳转。
    nr.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                    QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));
    nr.setRawHeader("User-Agent", m_userAgent.toUtf8());
    nr.setRawHeader("Accept", "application/json, text/plain, */*");
    nr.setRawHeader("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");

    for (const auto &h : req.headers) {
        nr.setRawHeader(h.first, h.second);
    }
    return nr;
}

QNetworkReply *QtHttpClient::startReply(const QNetworkRequest &request, const HttpRequest &req)
{
    const QByteArray method = req.method.isEmpty() ? QByteArrayLiteral("GET") : req.method.toUpper();
    if (method == QByteArrayLiteral("GET")) {
        return m_nam.get(request);
    }
    if (method == QByteArrayLiteral("POST")) {
        return m_nam.post(request, req.body);
    }
    return m_nam.sendCustomRequest(request, method, req.body);
}

HttpResponse QtHttpClient::send(const HttpRequest &req)
{
    HttpResponse res;

    if (!req.url.isValid() || req.url.isEmpty()) {
        res.error = i18n::s("请求 URL 无效", "Invalid request URL");
        return res;
    }

    QNetworkRequest nr = buildRequest(req);

    const int timeoutMs = effectiveTimeoutMs(req);

    // 双保险：Qt 自带的传输超时 + 下面的 QTimer（部分平台 abort 更可靠）。
    if (timeoutMs > 0) {
        nr.setTransferTimeout(timeoutMs);
    }

    QNetworkReply *reply = startReply(nr, req);
    if (reply == nullptr) {
        res.error = i18n::s("无法创建网络请求", "Unable to create a network request");
        return res;
    }

    QEventLoop loop;
    bool timedOut = false;

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&loop, &timedOut, reply]() {
        timedOut = true;
        reply->abort();   // abort 通常同步触发 finished，从而退出事件循环
        loop.quit();      // 兜底：确保一定退出，避免挂死
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    if (timeoutMs > 0) {
        timer.start(timeoutMs);
    }

    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    }

    res.status = statusOf(reply);
    res.body = reply->readAll();

    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorStr = reply->errorString();

    if (timedOut) {
        res.ok = false;
        res.error = i18n::sf("请求超时（%1 ms）：%2",
                             "Request timed out after %1 ms: %2",
                             {timeoutMs, req.url.toString()});
    } else if (netError != QNetworkReply::NoError) {
        res.ok = false;
        res.error = i18n::sf("网络错误(%1)：%2",
                             "Network error (%1): %2",
                             {static_cast<int>(netError), netErrorStr});
    } else if (res.status < 200 || res.status >= 300) {
        res.ok = false;
        res.error = i18n::sf("HTTP 状态异常：%1", "Unexpected HTTP status: %1", {res.status});
    } else {
        res.ok = true;
    }

    reply->deleteLater();
    return res;
}

HttpResponse QtHttpClient::sendStream(const HttpRequest &req, const HttpStreamCallbacks &cb)
{
    HttpResponse res;

    if (!req.url.isValid() || req.url.isEmpty()) {
        res.error = i18n::s("请求 URL 无效", "Invalid request URL");
        return res;
    }

    QNetworkRequest nr = buildRequest(req);
    // 下载场景不要 JSON 优先的 Accept，也不要 Qt 的整段传输超时：
    // 大文件靠下面的空闲超时兜底，否则几百 MB 的包必然被误杀。
    nr.setRawHeader("Accept", "*/*");

    QNetworkReply *reply = startReply(nr, req);
    if (reply == nullptr) {
        res.error = i18n::s("无法创建网络请求", "Unable to create a network request");
        return res;
    }

    QEventLoop loop;
    QTimer idle;
    idle.setSingleShot(true);

    bool timedOut = false;
    bool started = false;      // onStart 是否已经调过
    bool rejected = false;     // 调用方在 onStart 里拒收
    bool sinkAborted = false;  // 调用方在 onData 里中止
    qint64 received = 0;

    // 开始流：把状态码与 Content-Length 交给调用方决定收不收。
    const auto beginStream = [&](int status) -> bool {
        if (started) {
            return !rejected;
        }
        started = true;
        qint64 contentLength = -1;
        const QVariant lengthVar = reply->header(QNetworkRequest::ContentLengthHeader);
        if (lengthVar.isValid()) {
            contentLength = lengthVar.toLongLong();
        }
        if (cb.onStart && !cb.onStart(status, contentLength)) {
            rejected = true;
            return false;
        }
        return true;
    };

    QObject::connect(&idle, &QTimer::timeout, &loop, [&loop, &timedOut, reply]() {
        timedOut = true;
        reply->abort();
        loop.quit();
    });

    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
        if (timedOut || rejected || sinkAborted) {
            return;   // 已决定不收：剩下的字节丢弃即可，别写进文件
        }
        if (!beginStream(statusOf(reply))) {
            reply->abort();
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) {
            return;
        }
        received += chunk.size();
        if (cb.onData && !cb.onData(chunk, received)) {
            sinkAborted = true;
            reply->abort();
            return;
        }
        if (req.timeoutMs > 0) {
            idle.start(req.timeoutMs);   // 有数据进来就重置空闲计时
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    if (req.timeoutMs > 0) {
        idle.start(req.timeoutMs);
    }

    loop.exec();
    idle.stop();

    // finished 之后可能还有没读的尾巴（本地服务器一次性写完整份时很常见）。
    if (!rejected && !sinkAborted && !timedOut && beginStream(statusOf(reply))) {
        for (;;) {
            const QByteArray chunk = reply->readAll();
            if (chunk.isEmpty()) {
                break;
            }
            received += chunk.size();
            if (cb.onData && !cb.onData(chunk, received)) {
                sinkAborted = true;
                break;
            }
        }
    }

    res.status = statusOf(reply);

    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorStr = reply->errorString();

    if (timedOut) {
        res.ok = false;
        res.error = i18n::sf("请求超时（%1 ms 内没有任何数据）：%2",
                             "Request timed out (no data for %1 ms): %2",
                             {req.timeoutMs, req.url.toString()});
    } else if (rejected || sinkAborted) {
        res.ok = false;
        res.aborted = true;
        res.error = i18n::s("传输被调用方中止", "The transfer was aborted by the caller");
    } else if (netError != QNetworkReply::NoError) {
        res.ok = false;
        res.error = i18n::sf("网络错误(%1)：%2",
                             "Network error (%1): %2",
                             {static_cast<int>(netError), netErrorStr});
    } else if (res.status < 200 || res.status >= 300) {
        res.ok = false;
        res.error = i18n::sf("HTTP 状态异常：%1", "Unexpected HTTP status: %1", {res.status});
    } else {
        res.ok = true;
    }

    reply->deleteLater();
    return res;
}

} // namespace studious::net
