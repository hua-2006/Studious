#include "net/IHttpClient.h"

#include "i18n/L10n.h"

namespace studious::net {

HttpResponse IHttpClient::sendStream(const HttpRequest &req, const HttpStreamCallbacks &cb)
{
    // 兜底实现：先整份收下来，再模拟流式回调。
    // 只用于没有真流式能力的替身实现；QtHttpClient 覆盖了它。
    HttpResponse res = send(req);

    if (cb.onStart) {
        const qint64 contentLength = static_cast<qint64>(res.body.size());
        if (!cb.onStart(res.status, res.ok ? contentLength : 0)) {
            res.ok = false;
            res.aborted = true;
            res.error = i18n::s("传输被调用方中止", "The transfer was aborted by the caller");
            return res;
        }
    }

    if (!res.ok) {
        return res;
    }

    constexpr qsizetype kChunk = 64 * 1024;
    qint64 received = 0;
    for (qsizetype offset = 0; offset < res.body.size(); offset += kChunk) {
        const QByteArray chunk = res.body.mid(offset, kChunk);
        received += chunk.size();
        if (cb.onData && !cb.onData(chunk, received)) {
            res.ok = false;
            res.aborted = true;
            res.error = i18n::s("传输被调用方中止", "The transfer was aborted by the caller");
            break;
        }
    }
    return res;
}

} // namespace studious::net
