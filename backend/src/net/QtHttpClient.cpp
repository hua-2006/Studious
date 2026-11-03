#include "net/QtHttpClient.h"

#include "i18n/L10n.h"

#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QVariant>

#include <utility>

namespace studious::net {
namespace {

int statusOf(const QNetworkReply *reply)
{
    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    return statusVar.isValid() ? statusVar.toInt() : 0;
}

} // namespace

QtHttpClient::QtHttpClient(QString userAgent, QObject *parent)
    : m_nam(parent)
    , m_userAgent(std::move(userAgent))
{
    if (m_userAgent.isEmpty()) {
        m_userAgent = QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
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

    // 双保险：Qt 自带的传输超时 + 下面的 QTimer（部分平台 abort 更可靠）。
    if (req.timeoutMs > 0) {
        nr.setTransferTimeout(req.timeoutMs);
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

    if (req.timeoutMs > 0) {
        timer.start(req.timeoutMs);
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
                             {req.timeoutMs, req.url.toString()});
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
