#pragma once

#include <QUrl>
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

namespace studious::net {

// 一次 HTTP 请求的完整描述。与具体传输实现解耦，便于单测和替换。
struct HttpRequest {
    QUrl url;
    QByteArray method = "GET";
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    int timeoutMs = 15000;
};

struct HttpResponse {
    bool ok = false;      // 网络层成功且 status 2xx
    int status = 0;
    QByteArray body;
    QString error;
};

// 所有网络访问必须走这个抽象。业务代码只依赖它，不直接碰 QNetworkAccessManager。
class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual HttpResponse send(const HttpRequest &req) = 0;
};

} // namespace studious::net
