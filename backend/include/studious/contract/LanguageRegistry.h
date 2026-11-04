#pragma once

#include <QString>
#include <QStringList>

namespace studious::core {

// 内部语言码 ⇄ 各组件语言码的集中映射表。
// 内部码采用简化 BCP-47：auto / zh / zh-TW / en / ja / ko / fr / de / es / ru / pt / pt-BR / it
//
// 设计要点：所有跨组件的语言码转换只能走这里，禁止在别处硬编码 "zh-CN" 之类的字符串。
class LanguageRegistry {
public:
    // 各种写法归一化到内部码；未知输入返回小写后的原值（不抛错，交给调用方判断）。
    // 处理别名：zh-cn / zh_CN / zh-Hans / zh_Hans -> zh，zh-tw / zh-hant / zh_TW -> zh-TW，
    //           pt-br -> pt-BR，jp -> ja，kr -> ko
    static QString normalize(const QString &code);

    // 中文显示名，UI 下拉框用：zh -> "中文(简体)"
    static QString displayName(const QString &internal);

    // 映射到 Google 免费接口的语言码：zh -> zh-CN，zh-TW -> zh-TW，en -> en ...
    static QString toGoogle(const QString &internal);

    // 映射到 MyMemory 的语言码：zh -> zh-CN
    static QString toMyMemory(const QString &internal);

    // 内部语言码 -> Marian 语言 token（**不含**尖括号）。例：zh -> "zho_Hans"、ja -> "jpn"。
    // 这些 token 只存在于多语言模型（mt.opus.en-mul）的 vocab.json 里，spm 切不出来，
    // 所以调用方要拿这个名字去 vocab.json 查 id 再手工拼到 input_ids 最前面。
    // 多语言模型覆盖不了的语言（例如 ko：en-mul 里没有任何韩语 token）返回空串。
    static QString toMarianToken(const QString &internal);

    // 全部内部语言码（含 auto），按 UI 展示顺序。
    static QStringList allInternal();

    // 是否为已知内部码（不含 auto）。
    static bool isKnown(const QString &internal);
};

} // namespace studious::core
