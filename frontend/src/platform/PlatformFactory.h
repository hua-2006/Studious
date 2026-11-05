// 平台工厂 — 调用方（UI）唯一允许出现的平台入口。
//
// 约定：UI 永远不 new 具体平台类，只拿 createPlatform() 返回的 unique_ptr<IPlatform>。
// 这样以后把 NullPlatform 换成 WinPlatform / X11Platform 时，UI 一行都不用改。
#pragma once

#include "platform/IPlatform.h"

#include <memory>

namespace studious::platform {

// 本次一律返回 NullPlatform。
// 以后按 #ifdef Q_OS_WIN / Q_OS_LINUX 在这里分支（见 PlatformFactory.cpp）。
std::unique_ptr<IPlatform> createPlatform();

} // namespace studious::platform
