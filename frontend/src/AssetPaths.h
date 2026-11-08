#pragma once

// 资产目录与清单的唯一入口。
//
// 以前「安装根在哪」「清单从哪读」分散在 AppBackend / LocalEngine / ScreenFlow /
// LiveSession 各处，各拼一遍 %APPDATA%\studious\assets —— 只有 AppBackend 那份做了
// cleanPath，日志里显示的路径和实际用的路径因此可能不一致。现在都走这里。

#include <QString>

#include <studious/asset/Manifest.h>

namespace studious::ui::paths {

// 数据根：所有"会长大的东西"（模型 / 运行时 / 日志 / 记录 / 缓存）都放进一个目录。
//
// 默认是应用目录下的 data/ —— 开发期 exe 在 frontend/build，往上找 version.json
// 命中仓库根；打包后 version.json 通常就在 exe 旁边。以前这些散在
// %APPDATA%\studious 下（assets/、logs/、records/、cache.db 各一处），
// 备份、迁移、卸载都得记四个位置，所以统一收进来。
QString dataRoot();

// 资产安装根：设置里指定了就用它（cleanPath），否则 <dataRoot>/assets。
QString assetsRoot();

// 模型目录按用途分三格 —— 一个 models/ 里平铺 16 个条目，分不清哪些是语言包、
// 哪些是 OCR、哪些能用 llama-server 打开：
//   <assets>/models/llm/<assetId>   本地 LLM 的 GGUF
//   <assets>/models/mt/<assetId>    本地 MT 的 opus-mt 语言包（目录）
//   <assets>/models/ocr/<assetId>   OCR 三件套的原件；运行时副本在 ocr/run/
// 运行时（llama-server / onnxruntime）仍在 <assets>/runtime/<assetId>。
QString modelsLlmDir();
QString modelsMtDir();
QString modelsOcrDir();

// 应用日志目录（studious.log 与轮转文件）。本地引擎的 llama-server.log 仍在
// 资产根下的 logs/（它由后端按运行时目录倒推，不依赖界面层）。
QString logsDir();

// 翻译记录（capture.db / mini.db / live.db）。
QString recordsDir();

// 翻译缓存库。
QString cacheDbPath();

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
