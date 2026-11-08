#pragma once

// 资产目录与清单的唯一入口。
//
// 以前「安装根在哪」「清单从哪读」分散在 AppBackend / LocalEngine / ScreenFlow /
// LiveSession 各处，各拼一遍 %APPDATA%\studious\assets —— 只有 AppBackend 那份做了
// cleanPath，日志里显示的路径和实际用的路径因此可能不一致。现在都走这里。

#include <QString>

#include <studious/asset/Manifest.h>

namespace studious::ui::paths {

// 资产安装根：设置里指定了就用它（cleanPath），否则 <AppData>/assets。
QString assetsRoot();

// 清单路径。生产只认编译进二进制的 :/assets/manifest.json（改它等于改 exe）；
// 开发期设 STUDIOUS_DEV_MANIFEST=1 才允许退回磁盘上的示例清单。
QString manifestPath();

// 解析随二进制发布的清单。仓库里这份是"待发布"状态（sig 与部分 sha256 为空），
// 所以这里放宽签名校验；下载路径仍会逐条拒绝"有地址却没 sha256"的条目。
bool loadManifest(studious::asset::Manifest *out);

// 安装后复查：单文件资产整份重算 sha256；目录型资产核对清单点名的文件与服务端
// 声明的可执行文件。**不会被自动调用**，调用方（本地引擎启动路径）要自己用。
bool verifyInstalled(const QString &assetId, QString *error);

}   // namespace studious::ui::paths
