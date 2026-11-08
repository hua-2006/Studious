#include "platform/SecretStore.h"

#include <QByteArray>
#include <QDebug>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dpapi.h>
#endif

namespace studious::platform::secret {
namespace {

// 前缀决定"这是谁加的密"：看到 dpapi: 就去解开，看到 b64: 就知道只是编码过。
// 没有前缀 = 旧版本写进去的明文，原样交给调用方。
constexpr char kDpapiPrefix[] = "dpapi:";
constexpr char kBase64Prefix[] = "b64:";

// 附加熵：同一账户下不同用途的密文互不可解，防止把别处的 blob 塞进来。
QByteArray entropyBytes()
{
    return QByteArrayLiteral("studious.settings.v1");
}

}   // namespace

bool isProtected(const QString &stored)
{
    return stored.startsWith(QLatin1String(kDpapiPrefix))
        || stored.startsWith(QLatin1String(kBase64Prefix));
}

QString protect(const QString &plain)
{
    if (plain.isEmpty()) {
        return QString();
    }

#ifdef Q_OS_WIN
    const QByteArray utf8 = plain.toUtf8();
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(utf8.constData()));
    in.cbData = static_cast<DWORD>(utf8.size());

    QByteArray entropy = entropyBytes();
    DATA_BLOB ent{};
    ent.pbData = reinterpret_cast<BYTE *>(entropy.data());
    ent.cbData = static_cast<DWORD>(entropy.size());

    DATA_BLOB out{};
    // 不传 CRYPTPROTECT_LOCAL_MACHINE：只给当前用户解，别的账户拿到密文也没用。
    if (::CryptProtectData(&in, L"studious", &ent, nullptr, nullptr, 0, &out) != FALSE) {
        const QByteArray blob(reinterpret_cast<const char *>(out.pbData),
                              static_cast<int>(out.cbData));
        ::LocalFree(out.pbData);
        return QLatin1String(kDpapiPrefix) + QString::fromLatin1(blob.toBase64());
    }
    qWarning("[secret] CryptProtectData 失败（错误码 %lu），退回 base64",
             static_cast<unsigned long>(::GetLastError()));
#endif

    return QLatin1String(kBase64Prefix)
         + QString::fromLatin1(plain.toUtf8().toBase64());
}

QString unprotect(const QString &stored)
{
    if (stored.isEmpty()) {
        return QString();
    }

    if (stored.startsWith(QLatin1String(kDpapiPrefix))) {
        const QByteArray blob =
            QByteArray::fromBase64(stored.mid(static_cast<int>(qstrlen(kDpapiPrefix))).toLatin1());
#ifdef Q_OS_WIN
        DATA_BLOB in{};
        in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(blob.constData()));
        in.cbData = static_cast<DWORD>(blob.size());

        QByteArray entropy = entropyBytes();
        DATA_BLOB ent{};
        ent.pbData = reinterpret_cast<BYTE *>(entropy.data());
        ent.cbData = static_cast<DWORD>(entropy.size());

        DATA_BLOB out{};
        if (::CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, 0, &out) != FALSE) {
            const QString plain =
                QString::fromUtf8(reinterpret_cast<const char *>(out.pbData),
                                  static_cast<int>(out.cbData));
            ::LocalFree(out.pbData);
            return plain;
        }
        qWarning("[secret] CryptUnprotectData 失败（错误码 %lu）：密文可能来自别的账户或机器，"
                 "按「未配置」处理",
                 static_cast<unsigned long>(::GetLastError()));
#else
        qWarning("[secret] 这份配置是在 Windows 上加密的，当前平台解不开，按「未配置」处理");
#endif
        return QString();
    }

    if (stored.startsWith(QLatin1String(kBase64Prefix))) {
        return QString::fromUtf8(
            QByteArray::fromBase64(stored.mid(static_cast<int>(qstrlen(kBase64Prefix))).toLatin1()));
    }

    // 没有前缀：旧版本的明文，原样返回（下次保存时会被加密）。
    return stored;
}

}   // namespace studious::platform::secret
