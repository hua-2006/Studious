#pragma once

#include "net/IHttpClient.h"

#include <QNetworkAccessManager>
#include <QString>

class QNetworkReply;
class QNetworkRequest;

namespace studious::net {

// IHttpClient 的 Qt 实现：QNetworkAccessManager + QEventLoop + QTimer 做同步语义。
//
// 线程约束（重要）：QNetworkAccessManager 是线程亲和的，一个实例只能在其所属线程使用。
// 因此本类不可跨线程共享；工作线程里请各自构造一个 QtHttpClient。
class QtHttpClient final : public IHttpClient {
public:
    // 没有 parent 形参：m_nam 是值成员，把它交给某个 QObject 父子关系里
    // 会变成「父对象 delete 一个栈/成员对象」= invalid free + double free
    // （本类自身也不是 QObject，没有父子机制）。
    explicit QtHttpClient(QString userAgent = QStringLiteral(
                              "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"));
    ~QtHttpClient() override;

    // 同步发送：内部起局部事件循环，返回时请求已结束（成功、失败或超时）。
    HttpResponse send(const HttpRequest &req) override;

    // 流式发送：正文边收边回调，绝不整份读进内存（模型包上 GB）。
    // 超时用**空闲超时**语义：连续 timeoutMs 没有任何数据才中止，
    // 这样"慢但一直在传"的大文件不会被误杀。
    HttpResponse sendStream(const HttpRequest &req, const HttpStreamCallbacks &cb) override;

private:
    QNetworkRequest buildRequest(const HttpRequest &req) const;
    QNetworkReply *startReply(const QNetworkRequest &request, const HttpRequest &req);

    QNetworkAccessManager m_nam;
    QString m_userAgent;
};

} // namespace studious::net
