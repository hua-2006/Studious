#pragma once

#include "net/IHttpClient.h"

#include <QNetworkAccessManager>
#include <QString>

namespace studious::net {

// IHttpClient 的 Qt 实现：QNetworkAccessManager + QEventLoop + QTimer 做同步语义。
//
// 线程约束（重要）：QNetworkAccessManager 是线程亲和的，一个实例只能在其所属线程使用。
// 因此本类不可跨线程共享；工作线程里请各自构造一个 QtHttpClient。
class QtHttpClient final : public IHttpClient {
public:
    explicit QtHttpClient(QString userAgent = QStringLiteral(
                              "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"),
                          QObject *parent = nullptr);
    ~QtHttpClient() override;

    // 同步发送：内部起局部事件循环，返回时请求已结束（成功、失败或超时）。
    HttpResponse send(const HttpRequest &req) override;

private:
    QNetworkAccessManager m_nam;
    QString m_userAgent;
};

} // namespace studious::net
