#pragma once

// 引擎 id 的唯一换算层。
//
// 为什么单独一个文件：界面 id（web / llm-cloud / llm-local / local-mt）与 Router 的
// 后端 id（web / llm / local-mt）是两套命名，以前 AppBackend 与 QuickTranslate 各自
// 抄了一份 if 链，结果两边的别名表与「auto 怎么表示」不一致——同一份设置换个入口
// 翻译就可能挑到不同的引擎。现在只有这一份实现，谁要换算都调这里。
//
// 约定：
//   · 界面 id 空串 = 自动（不指定引擎，交给优先级挑）。
//   · "auto" 只是界面上的一个选项值，换算后同样是空串。
//   · llm-local 由 LocalEngine 单独承担，不进 Router 的降级链，换算成 Router id 时返回空。

#include <QString>
#include <QStringList>

namespace studious::ui::engine {

// 界面 id 空间的四个真实成员（顺序无关，用于校验与兜底）。
QStringList known();
// 默认降级顺序：本地优先（离线、不烧额度），云端次之，免费在线接口垫底。
QStringList defaultPriority();

// 任意写法（含老 id "llm"、别名 "localmt" / "llamacpp" / "local-llm"、大小写与空格）
// → 规范界面 id；无法识别 / "auto" / 空 → 空串（= 自动）。
QString toUi(const QString &raw);
// 界面 id → Router 引擎 id。llm-local 与无效 id → 空串。
QString toRouter(const QString &uiId);

// 只含已知 id、无重复、不缺项（缺的按默认顺序补在后面）。
QStringList sanitizePriority(const QStringList &input);
// 界面优先级 → Router 优先级（去掉 llm-local、去重、翻成后端 id）。
QStringList toRouterPriority(const QStringList &uiOrder);

// 界面显示名（中文源串，走 EngineIds 上下文，跟界面语言一起翻译）。
QString displayName(const QString &uiId);

}   // namespace studious::ui::engine
