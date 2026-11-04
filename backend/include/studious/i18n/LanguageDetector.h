#pragma once

#include <QString>

namespace studious::i18n {

// 轻量语言识别结果。
struct DetectedLanguage {
    QString code;              // 内部语言码；判断不出时为空
    double  confidence = 0.0;  // 0~1，越低越不可靠
};

// 识别一段文本的语言。
//
// 设计取舍：
//   · 不引入 fastText/CLD3 等模型文件 —— 那是 1MB 起步的额外资产，
//     而本项目必须支持"零资产也能跑"（云端引擎场景）。
//   · 改用「文字系统 + 停用词 + 特征字母」三重判据，纯 C++ 零依赖。
//   · 拉丁语系之间靠停用词区分，短句（<3 个词）会返回较低 confidence，
//     调用方应据此保守处理（例如不强行改写用户的源语言选择）。
//
// 覆盖：zh / zh-TW / ja / ko / ru / ar / th / hi / en / fr / de / es / pt / it
// 未覆盖的语言返回空 code（调用方当作 auto 处理）。
DetectedLanguage detectLanguage(const QString &text);

} // namespace studious::i18n
