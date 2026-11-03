#pragma once

#include <QString>
#include <QtGlobal>

#include <studious/asset/AssetRegistry.h>
#include <studious/i18n/L10n.h>

// 资产界面 / --assets-cli 共用的纯格式化工具（header-only，无状态）。
//
// 单独拆一个头的原因：对话框和命令行打印必须**字节级一致**，
// 否则验收时两边对不上（"250.0 MB" 与 "250 MB" 这种）。
namespace studious::frontend {

// 人类可读体积：B / KB / MB / GB，KB 以上保留 1 位小数。
// 0 字节固定显示 "0 B"（不显示 "0.0 B"），因为空安装目录就是这个值。
inline QString humanBytes(qint64 bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }

    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    }

    const double mb = kb / 1024.0;
    if (mb < 1024.0) {
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    }

    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
}

// 安装状态文案。走 i18n（跟随「界面语言」设置，重启生效）。
inline QString assetStateText(studious::asset::AssetState state)
{
    switch (state) {
    case studious::asset::AssetState::Installed:
        return studious::i18n::s("已安装", "Installed");
    case studious::asset::AssetState::Corrupted:
        return studious::i18n::s("损坏", "Corrupted");
    case studious::asset::AssetState::NotInstalled:
        break;
    }
    return studious::i18n::s("未安装", "Not installed");
}

} // namespace studious::frontend
