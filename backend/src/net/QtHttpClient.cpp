#include "net/QtHttpClient.h"

#include "i18n/L10n.h"

#include <QEventLoop>
#include <QList>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
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

// 回环判定。127.0.0.0/8 整段都是回环（不止 127.0.0.1），localhost 与 ::1 一并认。
//
// 这类地址只可能指向本机（本地 llama-server / 内置网关）。让它们走用户配的 HTTP 代理
// 必然失败：实测 STUDIOUS_HTTP_PROXY=http://127.0.0.1:1 时本地探测报
// "网络错误(101)"（ProxyConnectionRefusedError），也就是一开代理本地 LLM 就 100% 不可用。
bool isLoopbackHost(QString host)
{
    host = host.trimmed().toLower();
    if (host.isEmpty()) {
        return false;
    }
    if (host == QLatin1String("localhost") || host == QLatin1String("::1")
        || host == QLatin1String("0:0:0:0:0:0:0:1") || host == QLatin1String("[::1]")) {
        return true;
    }
    return host.startsWith(QLatin1String("127."));
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

    if (isLoopbackHost(req.url.host())) {
        return req.timeoutMs;
    }

    return qMax(req.timeoutMs, floorMs);
}

// 逐请求决定代理：回环直连，其余走配置好的代理。
//
// 为什么不用 QNetworkAccessManager::setProxy：它只能给整个 NAM 设一把代理，
// 没有"按 host 区分"的入口，一旦设上，127.0.0.1 的本地 llama-server 也会被塞进代理。
//
// 生命周期：QNetworkProxyFactory 不是 QObject，setProxyFactory() 也不接管所有权，
// 所以这里让它**同时派生 QObject**，以 NAM 为 parent —— NAM 析构时一并 delete，
// 既不裸 new 泄漏，也不会在 ~QNetworkAccessManager 期间留下悬垂指针
// （QObject 只在自己的 ~QObject 里删子对象，那时 NAM 的析构体早已跑完）。
class LoopbackAwareProxyFactory final : public QObject, public QNetworkProxyFactory
{
public:
    LoopbackAwareProxyFactory(QNetworkProxy envProxy, QObject *parent)
        : QObject(parent)
        , m_envProxy(std::move(envProxy))
    {
    }

    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override
    {
        // 回环地址一律直连，绝不交给代理。前一个字段给 URL 查询，后一个给 peerHost 查询，
        // 两个都判是因为不同调用路径填的字段不一样。
        if (isLoopbackHost(query.peerHostName()) || isLoopbackHost(query.url().host())) {
            return {QNetworkProxy(QNetworkProxy::NoProxy)};
        }

        if (m_envProxy.type() != QNetworkProxy::NoProxy) {
            return {m_envProxy};
        }

        // 没配环境变量：保持原来的"跟随系统代理"语义（Qt 默认走的就是系统配置）。
        // Windows 上这次查询要读注册表，只做一次并缓存，别让每段译文都付一次开销。
        if (!m_systemQueried) {
            m_systemQueried = true;
            m_systemProxies = QNetworkProxyFactory::systemProxyForQuery(query);
            if (m_systemProxies.isEmpty()) {
                m_systemProxies = {QNetworkProxy(QNetworkProxy::NoProxy)};
            }
        }
        return m_systemProxies;
    }

private:
    QNetworkProxy m_envProxy;
    QList<QNetworkProxy> m_systemProxies;
    bool m_systemQueried = false;
};

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

    // 全局关掉"系统配置即默认工厂"那条路：本 NAM 的代理决策只由下面这个工厂负责，
    // 免得两套机制打架。进程级设置，重复调用无副作用。
    QNetworkProxyFactory::setUseSystemConfiguration(false);

    // 不再无条件 m_nam.setProxy(...)：改成安装按 host 判定的工厂，
    // 回环地址（本地 llama-server）永远直连，其余请求仍走配置的代理。
    m_nam.setProxyFactory(new LoopbackAwareProxyFactory(proxyFromEnvironment(), &m_nam));
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
    // 超时路径里上面调过 abort()，Qt 会把 reply 的设备关掉；
    // 此时再 readAll() 只会打出 "QIODevice::read (QNetworkReplyHttpImpl): device not open"
    // 这类告警，且读不到东西。先判可读性再读。
    if (reply->isOpen() && reply->isReadable()) {
        res.body = reply->readAll();
    }

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
    // abort() 过的 reply 设备已关闭，读之前先判可读性，避免 "device not open" 告警。
    if (!rejected && !sinkAborted && !timedOut && reply->isOpen() && reply->isReadable()
        && beginStream(statusOf(reply))) {
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
