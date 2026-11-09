#pragma once

#include <QString>

namespace studious::asset {

// 应用数据根的唯一实现。放在后端是为了让「不知道 UI 存在」的组件
// （本地 MT 的默认模型目录、自测程序、CLI）也能算出同一份路径，
// 不再各自抄一份 QStandardPaths 或各写一遍向上查找。
//
// 规则：从 exe 目录往上最多四层找 version.json，命中就用 <应用目录>/data；
// 找不到（打包时把 exe 单独拎出来、跑自测程序）就用 <exe 目录>/data。
// 只做路径推导，不创建目录。
QString dataRoot();

// <dataRoot>/assets。注意 UI 层还多一层「设置里可以改安装根」的覆盖，
// 所以前端不要直接拿它当最终答案。
QString assetsRoot();

// <assetsRoot>/models/<kind>，kind 取 "llm" / "mt" / "ocr"。
// 空 kind 直接返回 <assetsRoot>/models。
QString modelsRoot(const QString &kind);

}   // namespace studious::asset
