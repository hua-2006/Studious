#pragma once

#include <QUrl>
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

#include <functional>

namespace studious::net {

// 一次 HTTP 请求的完整描述。与具体传输实现解耦，便于单测和替换。
struct HttpRequest {
    QUrl url;
    QByteArray method = "GET";
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    int timeoutMs = 15000;
    // 「多久连一个字都没收到就算失败」。0 = 不单独判。
    // 大模型不是流式返回时，timeoutMs 管的是整段生成时间（几十秒很正常），
    // 而用户真正会以为「卡死了」的是：连服务都没连上 / 对面一声不响。
    int firstByteTimeoutMs = 0;
};

struct HttpResponse {
    bool ok = false;      // 网络层成功且 status 2xx
    int status = 0;
    QByteArray body;
    QString error;
    // 传输是不是被调用方（onStart / onData 返回 false）主动中止的。
    // 中止不等于网络故障：下载器据此区分"用户取消 / 服务端 416"与"真的连不上"。
    bool aborted = false;
    // 是「首字节超时」（连上了但对面一直不出声），不是整段请求超时。
    // 上层据此给用户一句人话，而不是把毫秒数甩给他。
    bool firstByteTimedOut = false;
    // 连不上（拒绝连接 / 域名解析失败 / 网络不可达）—— 这类失败重试没有意义。
    bool connectionFailed = false;
};

// 流式接收回调。大文件（模型包动辄几个 GB）绝不能整份读进内存再落盘，
// 所以下载走这一套：先报头（状态码 + 长度），再按块报数据。
struct HttpStreamCallbacks {
    // 收到响应头、正文开始之前回调一次。
    // 返回 false = 拒收（连接会被中止，正文一个字节也不会交给 onData）。
    // 典型用途：状态码不是 200/206/416 时立刻掐断，别把 404 的 HTML 错误页写进 .part。
    std::function<bool(int status, qint64 contentLength)> onStart;

    // 每收到一块数据回调一次，receivedTotal 是**本次传输**的累计字节数。
    // 返回 false = 中止传输。
    std::function<bool(const QByteArray &chunk, qint64 receivedTotal)> onData;
};

// 所有网络访问必须走这个抽象。业务代码只依赖它，不直接碰 QNetworkAccessManager。
class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual HttpResponse send(const HttpRequest &req) = 0;

    // 流式发送：语义与 send() 一致（同步、返回时请求已结束），但正文按块回调，不驻留内存。
    //
    // 默认实现退化成 send() + 按 64 KiB 切块回调，够测试替身用；
    // 真实传输实现应覆盖它，做到边收边回调（QtHttpClient 已覆盖）。
    virtual HttpResponse sendStream(const HttpRequest &req, const HttpStreamCallbacks &cb);
};

} // namespace studious::net
