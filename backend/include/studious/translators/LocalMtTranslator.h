#pragma once

#include <atomic>

#include "contract/ITranslator.h"
#include "translators/ICancelControl.h"

#include <QSet>
#include <QString>

#include <map>
#include <memory>
#include <mutex>

namespace studious::mt {
class MtModel;
}

namespace studious::translate {

// 本地神经机器翻译组件（OPUS-MT / Marian，ONNX Runtime CPU 推理）。
//
// 资产约定：<modelsRoot>/mt.opus.<src>-<dst>/ 下放
//   encoder_model_quantized.onnx / decoder_model_quantized.onnx
//   source.spm / target.spm / vocab.json / config.json
//
// 语言对策略（按顺序找第一个可用的）：
//   1. 直连          mt.opus.<src>-<dst>                       质量最好；
//   2. 英文中转      mt.opus.<src>-en + mt.opus.en-<dst>        两次推理；
//   3. en-mul 兜底   mt.opus.<src>-en + mt.opus.en-mul          给目标加 >>token<<；
//   4. mul-en 兜底   mt.opus.mul-en  + mt.opus.en-<dst>         源侧自动识别，不带 token；
//   5. mul 全兜底    mt.opus.mul-en  + mt.opus.en-mul           目标加 >>token<<；
//   6. 都没有 → supports() 返回 false，Router 不会选中，UI 通过 resolve() 提示下载。
//
// 注意 "mul" 不是真语言码，只是 mt.opus.mul-en 目录名里的伪语言标签：
// 它绝不能被当成 en→mul / mul→zh 这类"直连"去匹配（否则 en-mul 会被误用）。
class LocalMtTranslator final : public core::ITranslator, public ICancelControl {
public:
    // modelsRoot 为空时用 %APPDATA%/studious/assets/models（与资产系统一致）。
    explicit LocalMtTranslator(const QString &modelsRoot = QString());
    ~LocalMtTranslator() override;

    QString id() const override;
    bool isReady() const override;
    core::ResolveResult resolve(const QString &src, const QString &dst) const override;
    QStringList supportedTargetLangs() const override;
    bool supports(const QString &src, const QString &dst) const override;
    core::TranslateOutcome translate(const QStringList &paragraphs,
                                     const QString &srcLang,
                                     const QStringList &dstLangs,
                                     const QJsonObject &options) override;
    void cancel() override;

    // 取消标志只由 Router 在每个请求开始时清零（见 ICancelControl.h 的说明）。
    void resetCancel() override;

    QString cacheTag() const override;

    // 当前已安装的语言对（"en-zh" 形式），供 UI 展示。
    QStringList installedPairs() const;

    // ---- 常驻 / 预热 ------------------------------------------------------
    // 这个组件默认按需加载：一个语言对第一次翻的时候才把 ONNX 会话建起来，
    // 组件析构时一起释放。界面上的「开机自启」用 warmPair() 提前建好会话，
    // 「空闲关闭」用 unloadAll() 把权重还给系统。
    bool warmPair(const QString &pair);      // pair = "en-zh"；已加载直接返回 true
    void unloadAll();                        // 释放所有已加载的语言对
    int loadedCount() const;                 // 当前常驻了几个语言对（UI 显示用）

    // 一个语言对目录里必须存在的文件（不含可选的 decoder_model_merged_quantized.onnx）。
    // 返回值与 modelFor() 构造 MtPaths 时用的文件名逐字一致，顺序即 UI/自测的展示顺序。
    static QStringList requiredFiles();

    // missingFiles: 该语言对的目录里缺了哪些必备文件（只列相对文件名）。
    // 目录不存在时返回全部必备文件 —— 因为"目录缺失"和"文件缺失"对调用方来说是同一件事：
    // 这个对不能用。想区分的话调用方自己 QDir 看一眼即可。
    QStringList missingFiles(const QString &pair) const;

    // src→dst 是否要走英文中转（直连模型可用时返回 false）。
    // 只是把 translate() 内部的选择结果提前暴露出来，供 UI/自测展示。
    bool pivotsThroughEnglish(const QString &src, const QString &dst) const;

    // src→dst 实际会走的路径，稳定 ASCII 标识（自测按它断言）：
    //   "direct" / "pivot-en" / "en-mul" / "mul-en" / "mul-en+en-mul"；不可用返回空串。
    QString routeId(const QString &src, const QString &dst) const;

    // 同上，但是给人看的一句话（含真在用的模型目录名），resolve() 也复用它。
    QString routeDescription(const QString &src, const QString &dst) const;

private:
    enum class RouteKind {
        None,
        Direct,
        EnglishPivot,
        EnMul,      // src→en + en-mul（目标加 >>token<<）
        MulEn,      // mul-en + en→dst（源侧自动识别）
        MulBoth,    // mul-en + en-mul（目标加 >>token<<）
    };

    struct Plan {
        RouteKind kind = RouteKind::None;
        QString targetToken;   // 仅 en-mul 侧需要：Marian token（如 "zho_Hans"）
        QString error;         // 非空 = 有明确不可用原因（如韩语目标没有 token）
    };

    Plan planFor(const QString &src, const QString &dst) const;

    // 多语言模型真正覆盖的语言集合：以 mt.opus.en-mul/vocab.json 里的 >>xx<< token 为准
    // （194 个，去掉尖括号就是 Marian token 名），外加 en（mul-en 的源端语言）。
    // 这是一次实测结论的固化：韩语这类没有 token 的语言，spm 切出来会落成 <unk>，
    // 硬翻必然输出垃圾，所以宁可提前拒绝并说清楚。
    const QSet<QString> &mulTokens() const;
    bool mulCoversLanguage(const QString &internal) const;
    QString mulUnsupportedReason(const QString &internal, bool asTarget) const;

    QString pairDir(const QString &src, const QString &dst) const;
    bool pairAvailable(const QString &src, const QString &dst) const;
    mt::MtModel *modelFor(const QString &src, const QString &dst);
    bool translateOne(const QString &text,
                      const QString &src,
                      const QString &dst,
                      QString &outText,
                      QString &error);

    QString m_modelsRoot;
    // 界面上的「开机自启」把本对象做成进程级共享实例之后，三个页面（截屏 / 小窗 /
    // 实时）可能在不同工作线程同时用到它 —— ONNX 会话和 m_models 都不是线程安全的，
    // 所以这里用一把粗锁把调用串起来。本地 MT 本来就是 CPU 满载的活，串行不损失什么。
    // mutable：loadedCount() 是 const，但读 m_models.size() 也要跟写方互斥。
    mutable std::mutex m_mutex;
    // 注意：不能用 QHash —— Qt 容器要求元素可拷贝，std::unique_ptr 不能拷贝（实测编译不过）。
    std::map<QString, std::unique_ptr<mt::MtModel>> m_models;   // key: "en-zh"
    // 多语言 token 表缓存（惰性读一次 vocab.json；mutable 因为 resolve()/supports() 是 const）。
    mutable bool m_mulTokensLoaded = false;
    mutable QSet<QString> m_mulTokens;
    std::atomic_bool m_cancelRequested{false};
};

} // namespace studious::translate
