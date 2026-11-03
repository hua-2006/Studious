#include "net/QtHttpClient.h"

#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QVariant>

#include <utility>

namespace studious::net {

QtHttpClient::QtHttpClient(QString userAgent, QObject *parent)
    : m_nam(parent)
    , m_userAgent(std::move(userAgent))
{
    if (m_userAgent.isEmpty()) {
        m_userAgent = QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    }
}

QtHttpClient::~QtHttpClient() = default;

HttpResponse QtHttpClient::send(const HttpRequest &req)
{
    HttpResponse res;

    if (!req.url.isValid() || req.url.isEmpty()) {
        res.error = QStringLiteral("请求 URL 无效");
        return res;
    }

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

    // 双保险：Qt 自带的传输超时 + 下面的 QTimer（部分平台 abort 更可靠）。
    if (req.timeoutMs > 0) {
        nr.setTransferTimeout(req.timeoutMs);
    }

    const QByteArray method = req.method.isEmpty() ? QByteArrayLiteral("GET") : req.method.toUpper();
    QNetworkReply *reply = nullptr;
    if (method == QByteArrayLiteral("GET")) {
        reply = m_nam.get(nr);
    } else if (method == QByteArrayLiteral("POST")) {
        reply = m_nam.post(nr, req.body);
    } else {
        reply = m_nam.sendCustomRequest(nr, method, req.body);
    }

    if (reply == nullptr) {
        res.error = QStringLiteral("无法创建网络请求");
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

    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    res.status = statusVar.isValid() ? statusVar.toInt() : 0;
    res.body = reply->readAll();

    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorStr = reply->errorString();

    if (timedOut) {
        res.ok = false;
        res.error = QStringLiteral("请求超时（%1 ms）：%2").arg(req.timeoutMs).arg(req.url.toString());
    } else if (netError != QNetworkReply::NoError) {
        res.ok = false;
        res.error = QStringLiteral("网络错误(%1)：%2").arg(static_cast<int>(netError)).arg(netErrorStr);
    } else if (res.status < 200 || res.status >= 300) {
        res.ok = false;
        res.error = QStringLiteral("HTTP 状态异常：%1").arg(res.status);
    } else {
        res.ok = true;
    }

    reply->deleteLater();
    return res;
}

} // namespace studious::net
