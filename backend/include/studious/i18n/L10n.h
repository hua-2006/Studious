#pragma once

#include <QString>
#include <QVariantList>

namespace studious::i18n {

// 后端文案的两种语言。v1 只做中英，其它语言的界面文字由前端负责。
enum class Language { Chinese, English };

// 当前生效语言。
//
// 判定规则：**只看系统语言**，不读任何配置文件——
//   QLocale::system().language() == QLocale::Chinese → 中文，其余 → 英文。
// setLanguage() 只用于测试和前端设置里的显式覆盖（覆盖后仍然不碰配置文件，
// 进程退出即失效，前端想持久化就自己存）。
Language current();

// 显式覆盖语言。覆盖后 current() 不再跟随系统。
void setLanguage(Language lang);

// 丢掉显式覆盖，重新跟随系统语言。
void resetToSystemLanguage();

// 双语取词：zh / en 都必须是 UTF-8 字面量（源码本身是 UTF-8 无 BOM）。
//
// 用法：
//   out.error = i18n::s("清单签名校验失败", "Manifest signature verification failed");
//   前端拿到的是**已经本地化好的最终文本**，直接显示即可，不要再拼前缀。
inline QString s(const char *zh, const char *en)
{
    return current() == Language::Chinese ? QString::fromUtf8(zh) : QString::fromUtf8(en);
}

// 带格式化的双语取词：zh / en 里用 %1 %2 %3 … 占位，与 QString::arg 的序号语义一致。
//
// 实现是**单遍扫描**：替换进去的参数内容不会被再次当作占位符扫描，
// 所以传 URL / 路径（含 "%20" 之类百分号编码）不会串位。
QString sf(const char *zh, const char *en, const QVariantList &args);

} // namespace studious::i18n
