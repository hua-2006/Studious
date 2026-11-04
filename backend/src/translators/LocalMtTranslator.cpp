#include "translators/LocalMtTranslator.h"

#include "contract/LanguageRegistry.h"
#include "i18n/L10n.h"
#include "i18n/LanguageDetector.h"
#include "mt/MtModel.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace studious::translate {
namespace {

// 一个语言对目录里必须具备的文件。缺任何一个都视为该对不可用。
const char *const kRequiredPairFiles[] = {
    "encoder_model_quantized.onnx",
    "decoder_model_quantized.onnx",
    "source.spm",
    "target.spm",
    "vocab.json",
};

QString defaultModelsRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("assets/models"));
}

} // namespace

LocalMtTranslator::LocalMtTranslator(const QString &modelsRoot)
    : m_modelsRoot(modelsRoot.isEmpty() ? defaultModelsRoot() : modelsRoot)
{
}

LocalMtTranslator::~LocalMtTranslator() = default;

QString LocalMtTranslator::id() const
{
    return QStringLiteral("local-mt");
}

QString LocalMtTranslator::cacheTag() const
{
    // 本地 MT 的译文取决于装了哪些语言对；把已装词组进 tag，
    // 这样"装了新对之后"不会误命中旧缓存。
    return QStringLiteral("local-mt|%1").arg(installedPairs().join(QLatin1Char(',')));
}

QString LocalMtTranslator::pairDir(const QString &src, const QString &dst) const
{
    return QDir(m_modelsRoot).filePath(QStringLiteral("mt.opus.%1-%2").arg(src, dst));
}

bool LocalMtTranslator::pairAvailable(const QString &src, const QString &dst) const
{
    const QString dir = pairDir(src, dst);
    for (const char *name : kRequiredPairFiles) {
        if (!QFileInfo::exists(QDir(dir).filePath(QString::fromUtf8(name)))) {
            return false;
        }
    }
    return true;
}

QStringList LocalMtTranslator::installedPairs() const
{
    QStringList pairs;
    const QDir root(m_modelsRoot);
    const QStringList dirs = root.entryList({QStringLiteral("mt.opus.*")}, QDir::Dirs, QDir::Name);
    for (const QString &name : dirs) {
        // mt.opus.en-zh -> en-zh
        const QString suffix = name.mid(QStringLiteral("mt.opus.").size());
        const int dash = suffix.indexOf(QLatin1Char('-'));
        if (dash <= 0) {
            continue;
        }
        const QString src = suffix.left(dash);
        const QString dst = suffix.mid(dash + 1);
        if (pairAvailable(src, dst)) {
            pairs << suffix;
        }
    }
    return pairs;
}

bool LocalMtTranslator::isReady() const
{
    return !installedPairs().isEmpty();
}

QStringList LocalMtTranslator::supportedTargetLangs() const
{
    QStringList targets;
    for (const QString &pair : installedPairs()) {
        const int dash = pair.indexOf(QLatin1Char('-'));
        if (dash > 0) {
            const QString dst = pair.mid(dash + 1);
            if (!targets.contains(dst)) {
                targets << dst;
            }
        }
    }
    return targets;
}

bool LocalMtTranslator::supports(const QString &src, const QString &dst) const
{
    const QString s = core::LanguageRegistry::normalize(src);
    const QString d = core::LanguageRegistry::normalize(dst);
    if (d.isEmpty() || s == d) {
        return false;
    }

    // src=auto：translate() 内部会先做语言检测再选模型，
    // 所以这里只判断"有没有任何能产出 d 的路径"，不能因为 s 是 auto 就否掉。
    if (s.isEmpty() || s == QLatin1String("auto")) {
        for (const QString &pair : installedPairs()) {
            if (pair.endsWith(QLatin1Char('-') + d)) {
                return true;
            }
        }
        return false;
    }

    if (pairAvailable(s, d)) {
        return true;
    }
    // 英文中转：A->en 和 en->B 都在才可行
    return s != QLatin1String("en") && d != QLatin1String("en")
           && pairAvailable(s, QStringLiteral("en"))
           && pairAvailable(QStringLiteral("en"), d);
}

core::ResolveResult LocalMtTranslator::resolve(const QString &src, const QString &dst) const
{
    core::ResolveResult r;
    const QString s = core::LanguageRegistry::normalize(src);
    const QString d = core::LanguageRegistry::normalize(dst);

    if (s.isEmpty() || d.isEmpty() || s == QLatin1String("auto")) {
        r.ready = false;
        r.reason = i18n::s("本地 MT 需要明确的源语言（不支持自动检测后再选模型）",
                           "Local MT needs an explicit source language (it cannot detect then pick a model)");
        return r;
    }

    if (pairAvailable(s, d)) {
        r.ready = true;
        r.reason = i18n::s("已安装该语言对", "The language pair is installed");
        return r;
    }

    // 直连缺失：检查能否走英文中转
    const bool canPivot = s != QLatin1String("en") && d != QLatin1String("en")
                          && pairAvailable(s, QStringLiteral("en"))
                          && pairAvailable(QStringLiteral("en"), d);
    if (canPivot) {
        r.ready = true;
        r.reason = i18n::sf("将经英文中转（%1→en→%2）",
                            "Will pivot through English (%1→en→%2)",
                            {s, d});
        return r;
    }

    r.ready = false;
    r.reason = i18n::sf("缺少本地翻译模型：需要 %1 或「%2→en + en→%3」",
                        "Missing local MT models: need %1, or %2→en plus en→%3",
                        {QStringLiteral("mt.opus.%1-%2").arg(s, d), s, d});
    r.missingAssets = {QStringLiteral("mt.opus.%1-%2").arg(s, d)};
    return r;
}

mt::MtModel *LocalMtTranslator::modelFor(const QString &src, const QString &dst)
{
    const QString key = QStringLiteral("%1-%2").arg(src, dst);
    const auto found = m_models.find(key);
    if (found != m_models.end()) {
        return found->second.get();
    }

    const QString dir = pairDir(src, dst);
    mt::MtPaths paths;
    paths.encoderOnnx = QDir(dir).filePath(QStringLiteral("encoder_model_quantized.onnx"));
    paths.decoderOnnx = QDir(dir).filePath(QStringLiteral("decoder_model_quantized.onnx"));
    paths.sourceSpm = QDir(dir).filePath(QStringLiteral("source.spm"));
    paths.targetSpm = QDir(dir).filePath(QStringLiteral("target.spm"));
    paths.vocabJson = QDir(dir).filePath(QStringLiteral("vocab.json"));
    paths.configJson = QDir(dir).filePath(QStringLiteral("config.json"));

    auto model = std::make_unique<mt::MtModel>();
    QString error;
    if (!model->load(paths, &error)) {
        // 不缓存失败结果：用户可能刚装好模型，下次应重试。
        return nullptr;
    }
    mt::MtModel *raw = model.get();
    m_models.emplace(key, std::move(model));
    return raw;
}

bool LocalMtTranslator::translateOne(const QString &text,
                                     const QString &src,
                                     const QString &dst,
                                     QString &outText,
                                     QString &error)
{
    if (mt::MtModel *direct = (pairAvailable(src, dst) ? modelFor(src, dst) : nullptr)) {
        return direct->translate(text, outText, error);
    }

    // 英文中转：A -> en -> B
    mt::MtModel *toEn = modelFor(src, QStringLiteral("en"));
    mt::MtModel *fromEn = modelFor(QStringLiteral("en"), dst);
    if (toEn == nullptr || fromEn == nullptr) {
        error = i18n::s("缺少英文中转所需的模型", "Models required for English pivoting are missing");
        return false;
    }

    QString middle;
    QString firstError;
    if (!toEn->translate(text, middle, firstError)) {
        error = firstError;
        return false;
    }
    return fromEn->translate(middle, outText, error);
}

core::TranslateOutcome LocalMtTranslator::translate(const QStringList &paragraphs,
                                                    const QString &srcLang,
                                                    const QStringList &dstLangs,
                                                    const QJsonObject &options)
{
    Q_UNUSED(options)

    core::TranslateOutcome out;
    m_cancelRequested = false;

    // 源语言：用户显式指定就信用户；auto 时用语言检测（MT 没有自纠能力，
    // 选错语言对必然输出垃圾，所以这里必须检测而不是猜）。
    QString src = core::LanguageRegistry::normalize(srcLang);
    if (src.isEmpty() || src == QLatin1String("auto")) {
        const QString sample = paragraphs.isEmpty() ? QString() : paragraphs.first().left(300);
        const i18n::DetectedLanguage detected = i18n::detectLanguage(sample);
        src = detected.code;
        if (src.isEmpty() || detected.confidence < 0.5) {
            out.error = i18n::s("无法确定源语言：本地 MT 需要明确的语言对，请手动指定源语言。",
                                "Could not determine the source language. Local MT needs an explicit "
                                "language pair; please select the source language manually.");
            return out;
        }
        if (src == QLatin1String("zh-TW")) {
            src = QStringLiteral("zh");
        }
    }

    if (dstLangs.isEmpty()) {
        out.error = i18n::s("未指定目标语言", "No target language specified");
        return out;
    }

    QStringList errors;
    for (const QString &rawDst : dstLangs) {
        QString dst = core::LanguageRegistry::normalize(rawDst);
        if (dst == QLatin1String("zh-TW")) {
            dst = QStringLiteral("zh");
        }

        if (!supports(src, dst)) {
            const core::ResolveResult r = resolve(src, dst);
            errors << (r.reason.isEmpty() ? i18n::s("该语言对不可用", "This language pair is unavailable")
                                          : r.reason);
            continue;
        }

        QStringList translated;
        translated.reserve(paragraphs.size());
        QString firstError;

        for (const QString &paragraph : paragraphs) {
            if (m_cancelRequested) {
                out.error = i18n::s("本地 MT 已取消", "Local MT was cancelled");
                return out;
            }

            const QString trimmed = paragraph.trimmed();
            if (trimmed.isEmpty()) {
                translated << QString();
                continue;
            }

            QString one;
            QString err;
            if (!translateOne(trimmed, src, dst, one, err)) {
                translated << QString();
                if (firstError.isEmpty()) {
                    firstError = err;
                }
                continue;
            }
            translated << one;
        }

        out.byLang.insert(rawDst, translated);
        if (!firstError.isEmpty()) {
            errors << firstError;
        }
    }

    out.ok = errors.isEmpty();
    if (!out.ok) {
        out.error = errors.join(QStringLiteral("；"));
    }
    return out;
}

void LocalMtTranslator::cancel()
{
    m_cancelRequested = true;
}

} // namespace studious::translate
