#pragma once

// 进程级唯一的翻译缓存入口。
//
// 以前有两个：main.cpp 的 sharedCache()（GUI / --smoke）与 QuickTranslate.cpp 的
// processCache()（截屏 / 小窗 / 实时三页）。两个都指向 paths::cacheDbPath()，但各自
// 持有自己的实例 —— TranslationCache 的连接名是「路径哈希 + 线程 id」，同线程里两个
// 实例会**共用同一条 QSqlDatabase 连接**，看着没事；一旦哪天连接名规则改成按实例
// 区分（或者有人给其中一处传了不同的 dbPath），就会变成两条连接写同一个 db 文件，
// 缓存统计（命中 / 未命中）也跟着各算各的。
//
// 所以只留这一个入口：谁要缓存都来这里拿引用。

#include <studious/cache/TranslationCache.h>

namespace studious::ui {

// 首次调用时按 AssetPaths::cacheDbPath() 建实例，故意不 delete：
//   · 路径口径依赖 QCoreApplication 的 applicationName，不能做成命名空间级静态
//     （那样会在 QApplication 构造之前执行，拿到空路径）；
//   · TranslationCache 的析构要碰 QSqlDatabase 的驱动管理器，而进程退出时谁先析构
//     是未定义的 —— 让它在进程结束时随操作系统回收，最省心。
studious::cache::TranslationCache &sharedCache();

}   // namespace studious::ui
