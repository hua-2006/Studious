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

// 一个语言对目录里必须具备的文件清单见 LocalMtTranslator::requiredFiles()（唯一来源），
// 缺任何一个都视为该对不可用。

QString defaultModelsRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("assets/models"));
}

// 多语言资产目录里的伪语言标签：mt.opus.mul-en / mt.opus.en-mul。
// 它不是用户能选的语言，只是目录名里表示"多语言"的占位：
// 必须挡在语言码解析之外，否则 mt.opus.en-mul 会被 pairAvailable("en","mul") 当成
// "en→mul 直连"匹配上，把任何 en→X 的请求都抢成 mul 路径。
bool isMulTag(const QString &code)
{
    return code == QLatin1String("mul");
}

// 韩语等"多语言模型没有 token"的语言，专门给一条能看懂的原因。
// en-mul 的 vocab.json 里 194 个 >>xx<< token 全查过，一个韩语 token 都没有；
// 而且 mul-en 的词表切不出完整韩文（实测 안녕/날씨/좋네 都落成 <unk>），
// 与其静默产出垃圾，不如明确说不支持。
QString mulTargetError(const QString &dst)
{
    if (dst == QLatin1String("ko")) {
        return i18n::s("多语言模型不支持韩语作为目标语言（mt.opus.en-mul 的 vocab.json 里没有任何韩语 token）",
                       "The multilingual model cannot use Korean as the target language "
                       "(mt.opus.en-mul's vocab.json has no Korean token at all)");
    }
    return i18n::sf("多语言模型不支持目标语言 %1（mt.opus.en-mul 的 vocab.json 里没有它的语言 token）",
                    "The multilingual model cannot use %1 as the target language "
                    "(mt.opus.en-mul's vocab.json has no language token for it)",
                    {dst});
}

// 源语言侧的同一套措辞（韩语单独点名，其余按语言码说）。
QString mulSourceError(const QString &src)
{
    if (src == QLatin1String("ko")) {
        return i18n::s("多语言模型不支持韩语作为源语言"
                       "（mt.opus.en-mul 的 vocab.json 里没有任何韩语 token，mul-en 也切不出完整韩文）",
                       "The multilingual model cannot use Korean as the source language "
                       "(mt.opus.en-mul's vocab.json has no Korean token, and mul-en cannot "
                       "tokenize Korean without <unk> holes)");
    }
    return i18n::sf("多语言模型不支持源语言 %1（mt.opus.en-mul 的 vocab.json 里没有它的语言 token）",
                    "The multilingual model cannot use %1 as the source language "
                    "(mt.opus.en-mul's vocab.json has no language token for it)",
                    {src});
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
    // 多语言模型（mul-en / en-mul）一旦参与，同一句话的走法和译文都会变，
    // 所以单独再打一个 mul 段（内容就是 en-mul / mul-en 的目录名），便于排障时一眼看出来。
    const QStringList pairs = installedPairs();
    QStringList mulPairs;
    for (const QString &pair : pairs) {
        if (pair.endsWith(QLatin1String("-mul")) || pair.startsWith(QLatin1String("mul-"))) {
            mulPairs << pair;
        }
    }
    return QStringLiteral("local-mt|%1|mul:%2")
        .arg(pairs.join(QLatin1Char(',')),
             mulPairs.isEmpty() ? QStringLiteral("-") : mulPairs.join(QLatin1Char(',')));
}

QString LocalMtTranslator::pairDir(const QString &src, const QString &dst) const
{
    return QDir(m_modelsRoot).filePath(QStringLiteral("mt.opus.%1-%2").arg(src, dst));
}

bool LocalMtTranslator::pairAvailable(const QString &src, const QString &dst) const
{
    const QString dir = pairDir(src, dst);
    // 判据与 missingFiles() 完全一致（同一张表 + isFile()），否则会出现"界面说缺文件、
    // 路由却认为可用"的错位：曾经 kRequiredPairFiles 少 config.json 且用 exists()，
    // 目录里同名子目录也能算"有文件"，最后在 MtModel::load 里才炸。
    for (const QString &name : requiredFiles()) {
        if (!QFileInfo(QDir(dir).filePath(name)).isFile()) {
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

QStringList LocalMtTranslator::requiredFiles()
{
    // 必须与 modelFor() 里拼 MtPaths 的文件名逐字一致（见 MtModel::load 的存在性检查）。
    // 这张表是唯一来源：pairAvailable()、installedPairs()、missingFiles() 都用它，
    // 界面上"缺哪些文件"和路由判断"这对能不能用"因此永远一致。
    // vocab.json 缺了会退回 spm 自己的 id 空间（译文是错的），config.json 缺了
    // MtModel::load 直接失败（decoder_start_token_id / num_beams 在里面），都算必备。
    return {
        QStringLiteral("encoder_model_quantized.onnx"),
        QStringLiteral("decoder_model_quantized.onnx"),
        QStringLiteral("source.spm"),
        QStringLiteral("target.spm"),
        QStringLiteral("vocab.json"),
        QStringLiteral("config.json"),
        // decoder_model_merged_quantized.onnx 是可选加速项（KV cache），不算必备。
    };
}

QStringList LocalMtTranslator::missingFiles(const QString &pair) const
{
    const QStringList required = requiredFiles();
    const QString suffix = pair.trimmed();
    if (suffix.isEmpty()) {
        return required;   // 没给 pair 等于目录不可用，按"全缺"处理
    }

    // 直接拼 "mt.opus.<pair>"，刻意不走 pairDir(src, dst)：pair 已经是拼好的目录后缀，
    // 再按 '-' 拆成两段只是在"能不能原样拼回来"上赌一把（mul 伪语言标签、zh-TW 这类带
    // 横线的目标语言段都在赌注里），而目录名本身就等于 "mt.opus." + pair，一步拼完最稳，
    // 也和 installedPairs() 反过来剥前缀的写法对称。
    const QDir dir(QDir(m_modelsRoot).filePath(QStringLiteral("mt.opus.") + suffix));
    QStringList missing;
    missing.reserve(required.size());
    for (const QString &name : required) {
        // isFile() 而不是 exists()：同名目录不算文件，缺了就是缺了。
        if (!QFileInfo(dir.filePath(name)).isFile()) {
            missing << name;
        }
    }
    return missing;
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
            // "mul" 是伪语言标签（en-mul 的目标侧），不是用户能选的目标语言，别列给 UI。
            if (isMulTag(dst)) {
                continue;
            }
            if (!targets.contains(dst)) {
                targets << dst;
            }
        }
    }
    return targets;
}

const QSet<QString> &LocalMtTranslator::mulTokens() const
{
    if (m_mulTokensLoaded) {
        return m_mulTokens;
    }
    m_mulTokensLoaded = true;

    // mt.opus.en-mul/vocab.json 是"多语言模型会哪些语言"的唯一权威名单：
    // 里面每个 >>xx<< 就是一个可指定的语言，去掉尖括号正好等于 toMarianToken() 的返回值。
    const QString path =
        QDir(pairDir(QStringLiteral("en"), QStringLiteral("mul"))).filePath(QStringLiteral("vocab.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return m_mulTokens;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return m_mulTokens;
    }
    const QJsonObject obj = doc.object();
    m_mulTokens.reserve(obj.size());
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QString key = it.key();
        if (key.size() > 4 && key.startsWith(QLatin1String(">>")) && key.endsWith(QLatin1String("<<"))) {
            m_mulTokens.insert(key.mid(2, key.size() - 4));
        }
    }
    return m_mulTokens;
}

bool LocalMtTranslator::mulCoversLanguage(const QString &internal) const
{
    const QString n = core::LanguageRegistry::normalize(internal);
    if (n == QLatin1String("en")) {
        return true;   // mul-en 的源端 / en-mul 的中转语言就是英语本身
    }

    const QString token = core::LanguageRegistry::toMarianToken(n);
    if (token.isEmpty()) {
        return false;   // 表里就没有（ko 之类），肯定不支持
    }

    const QSet<QString> &tokens = mulTokens();
    if (tokens.isEmpty()) {
        // 词表读不到（资产没装全）：不拦，退回旧行为，别因为读不到文件就误判"不支持"。
        return true;
    }
    return tokens.contains(token);
}

QString LocalMtTranslator::mulUnsupportedReason(const QString &internal, bool asTarget) const
{
    const QString n = core::LanguageRegistry::normalize(internal);
    return asTarget ? mulTargetError(n) : mulSourceError(n);
}

LocalMtTranslator::Plan LocalMtTranslator::planFor(const QString &src, const QString &dst) const
{
    Plan plan;

    const QString s = core::LanguageRegistry::normalize(src);
    const QString d = core::LanguageRegistry::normalize(dst);
    if (s.isEmpty() || d.isEmpty() || s == d) {
        return plan;
    }
    if (isMulTag(s) || isMulTag(d)) {
        return plan;   // "mul" 不是真语言，任何方向都不该按它选路
    }

    // 1) 直连：装了就用，质量最好，也绝不能被 mul 抢走。
    if (pairAvailable(s, d)) {
        plan.kind = RouteKind::Direct;
        return plan;
    }

    // 源侧能不能到英文：en 本身不需要模型，其余看有没有 <src>-en。
    const bool reachesEnglish = (s == QLatin1String("en")) || pairAvailable(s, QStringLiteral("en"));
    const bool englishReachesTarget = pairAvailable(QStringLiteral("en"), d);
    const bool enMulReady = pairAvailable(QStringLiteral("en"), QStringLiteral("mul"));
    const bool mulEnReady = pairAvailable(QStringLiteral("mul"), QStringLiteral("en"));

    // 2) 英文中转：A→en→B，两次推理，单对各模型够用就不用 mul。
    if (s != QLatin1String("en") && d != QLatin1String("en") && reachesEnglish
        && englishReachesTarget) {
        plan.kind = RouteKind::EnglishPivot;
        return plan;
    }

    // 3) en-mul 兜底：A→en，再由多语言模型产出 B（目标靠 >>token<< 指定）。
    if (d != QLatin1String("en") && reachesEnglish && enMulReady) {
        if (mulCoversLanguage(d)) {
            plan.kind = RouteKind::EnMul;
            plan.targetToken = core::LanguageRegistry::toMarianToken(d);
            return plan;
        }
        // 目标语言不在多语言词表里（例如 ko）：先记下原因，继续试后面的路，
        // 全都不通时 resolve() 就能报出"多语言模型不支持韩语作为目标语言"。
        plan.error = mulUnsupportedReason(d, /*asTarget=*/true);
    }

    // 4) mul-en 兜底：源侧自动识别（mul-en 词表里没有任何语言 token，不需要前缀）
    //    → en，再走 en-dst。**源语言必须真的在多语言词表里**，否则 spm 切出来全是 <unk>，
    //    模型只会吐出垃圾（实测韩语就是这样），宁可拒绝。
    if (s != QLatin1String("en") && d != QLatin1String("en") && mulEnReady
        && englishReachesTarget) {
        if (mulCoversLanguage(s)) {
            plan.kind = RouteKind::MulEn;
            plan.error.clear();
            return plan;
        }
        if (plan.error.isEmpty()) {
            plan.error = mulUnsupportedReason(s, /*asTarget=*/false);
        }
    }

    // 5) mul 全兜底：mul-en 出英文，再交给 en-mul 出目标（目标加 >>token<<）。
    if (s != QLatin1String("en") && d != QLatin1String("en") && mulEnReady && enMulReady) {
        const bool srcOk = mulCoversLanguage(s);
        const bool dstOk = mulCoversLanguage(d);
        if (srcOk && dstOk) {
            plan.kind = RouteKind::MulBoth;
            plan.targetToken = core::LanguageRegistry::toMarianToken(d);
            plan.error.clear();
            return plan;
        }
        if (plan.error.isEmpty()) {
            plan.error = mulUnsupportedReason(srcOk ? d : s, /*asTarget=*/srcOk);
        }
    }

    return plan;
}

QString LocalMtTranslator::routeId(const QString &src, const QString &dst) const
{
    switch (planFor(src, dst).kind) {
    case RouteKind::Direct:
        return QStringLiteral("direct");
    case RouteKind::EnglishPivot:
        return QStringLiteral("pivot-en");
    case RouteKind::EnMul:
        return QStringLiteral("en-mul");
    case RouteKind::MulEn:
        return QStringLiteral("mul-en");
    case RouteKind::MulBoth:
        return QStringLiteral("mul-en+en-mul");
    case RouteKind::None:
        break;
    }
    return QString();
}

QString LocalMtTranslator::routeDescription(const QString &src, const QString &dst) const
{
    const Plan plan = planFor(src, dst);
    const QString s = core::LanguageRegistry::normalize(src);
    const QString d = core::LanguageRegistry::normalize(dst);

    switch (plan.kind) {
    case RouteKind::Direct:
        return i18n::sf("直连（mt.opus.%1-%2）", "Direct (mt.opus.%1-%2)", {s, d});
    case RouteKind::EnglishPivot:
        return i18n::sf("英文中转（%1→en→%2）", "English pivot (%1→en→%2)", {s, d});
    case RouteKind::EnMul:
        return i18n::sf("多语言模型 en-mul（%1→en→>>%2<<）",
                        "Multilingual en-mul (%1→en→>>%2<<)",
                        {s, plan.targetToken});
    case RouteKind::MulEn:
        return i18n::sf("多语言模型 mul-en（自动识别源语言→en→%1）",
                        "Multilingual mul-en (source auto-detected→en→%1)",
                        {d});
    case RouteKind::MulBoth:
        return i18n::sf("多语言模型 mul-en + en-mul（自动识别源语言→en→>>%1<<）",
                        "Multilingual mul-en + en-mul (source auto-detected→English→>>%1<<)",
                        {plan.targetToken});
    case RouteKind::None:
        break;
    }
    return plan.error;
}

bool LocalMtTranslator::supports(const QString &src, const QString &dst) const
{
    const QString s = core::LanguageRegistry::normalize(src);
    const QString d = core::LanguageRegistry::normalize(dst);
    if (d.isEmpty() || s == d || isMulTag(d) || isMulTag(s)) {
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
        // 没有直连 / en-d 时，多语言模型也能兜住（目标必须在多语言词表里有 token）。
        return pairAvailable(QStringLiteral("en"), QStringLiteral("mul")) && mulCoversLanguage(d);
    }

    return planFor(s, d).kind != RouteKind::None;
}

bool LocalMtTranslator::pivotsThroughEnglish(const QString &src, const QString &dst) const
{
    const QString s = core::LanguageRegistry::normalize(src);
    const QString d = core::LanguageRegistry::normalize(dst);
    if (s.isEmpty() || d.isEmpty() || s == QLatin1String("auto") || s == d) {
        return false;
    }
    if (pairAvailable(s, d)) {
        return false;
    }
    if (isMulTag(s) || isMulTag(d)) {
        return false;
    }
    return s != QLatin1String("en") && d != QLatin1String("en")
           && pairAvailable(s, QStringLiteral("en")) && pairAvailable(QStringLiteral("en"), d);
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

    // 文案必须反映**实际会走的路径**：直连 / 英文中转 / 多语言模型。
    const Plan plan = planFor(s, d);
    if (plan.kind != RouteKind::None) {
        r.ready = true;
        r.reason = routeDescription(s, d);
        return r;
    }

    if (!plan.error.isEmpty()) {
        r.ready = false;
        r.reason = plan.error;
        return r;
    }

    r.ready = false;
    r.reason = i18n::sf("缺少本地翻译模型：需要 %1，或「%2→en + en→%3」，或多语言模型 mt.opus.mul-en / mt.opus.en-mul",
                        "Missing local MT models: need %1, or %2→en plus en→%3, "
                        "or the multilingual models mt.opus.mul-en / mt.opus.en-mul",
                        {QStringLiteral("mt.opus.%1-%2").arg(s, d), s, d});
    r.missingAssets = {QStringLiteral("mt.opus.%1-%2").arg(s, d)};
    if (!pairAvailable(s, QStringLiteral("en"))) {
        r.missingAssets << QStringLiteral("mt.opus.%1-en").arg(s);
    }
    if (!pairAvailable(QStringLiteral("en"), d)) {
        r.missingAssets << QStringLiteral("mt.opus.en-%1").arg(d);
    }
    if (!pairAvailable(QStringLiteral("mul"), QStringLiteral("en"))) {
        r.missingAssets << QStringLiteral("mt.opus.mul-en");
    }
    if (!pairAvailable(QStringLiteral("en"), QStringLiteral("mul"))) {
        r.missingAssets << QStringLiteral("mt.opus.en-mul");
    }
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
    paths.decoderMergedOnnx = QDir(dir).filePath(QStringLiteral("decoder_model_merged_quantized.onnx"));
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
    const Plan plan = planFor(src, dst);

    const auto loadOrFail = [&](const QString &s, const QString &d) -> mt::MtModel * {
        mt::MtModel *model = modelFor(s, d);
        if (model == nullptr) {
            error = i18n::sf("本地 MT 模型加载失败：mt.opus.%1-%2",
                             "Failed to load the local MT model: mt.opus.%1-%2",
                             {s, d});
        }
        return model;
    };

    switch (plan.kind) {
    case RouteKind::Direct:
        if (mt::MtModel *direct = loadOrFail(src, dst)) {
            return direct->translate(text, outText, error);
        }
        return false;

    case RouteKind::EnMul: {
        // A -> en（源侧若是 en 就不用跑模型）-> en-mul（目标加 >>token<<）
        QString middle = text;
        if (core::LanguageRegistry::normalize(src) != QLatin1String("en")) {
            mt::MtModel *toEn = loadOrFail(src, QStringLiteral("en"));
            if (toEn == nullptr) {
                return false;
            }
            QString firstError;
            if (!toEn->translate(text, middle, firstError)) {
                error = firstError;
                return false;
            }
        }
        mt::MtModel *toTarget = loadOrFail(QStringLiteral("en"), QStringLiteral("mul"));
        if (toTarget == nullptr) {
            return false;
        }
        return toTarget->translate(middle, outText, error, plan.targetToken);
    }

    case RouteKind::MulEn: {
        // mul-en 自动识别源语言（词表里没有任何 >>xx<< token，所以**不带**语言前缀），
        // 出英文后再走 en-dst。
        mt::MtModel *toEn = loadOrFail(QStringLiteral("mul"), QStringLiteral("en"));
        if (toEn == nullptr) {
            return false;
        }
        QString middle;
        QString firstError;
        if (!toEn->translate(text, middle, firstError)) {
            error = firstError;
            return false;
        }
        mt::MtModel *toTarget = loadOrFail(QStringLiteral("en"), dst);
        if (toTarget == nullptr) {
            return false;
        }
        return toTarget->translate(middle, outText, error);
    }

    case RouteKind::MulBoth: {
        mt::MtModel *toEn = loadOrFail(QStringLiteral("mul"), QStringLiteral("en"));
        if (toEn == nullptr) {
            return false;
        }
        QString middle;
        QString firstError;
        if (!toEn->translate(text, middle, firstError)) {
            error = firstError;
            return false;
        }
        mt::MtModel *toTarget = loadOrFail(QStringLiteral("en"), QStringLiteral("mul"));
        if (toTarget == nullptr) {
            return false;
        }
        return toTarget->translate(middle, outText, error, plan.targetToken);
    }

    case RouteKind::EnglishPivot: {
        // 英文中转：A -> en -> B
        mt::MtModel *toEn = loadOrFail(src, QStringLiteral("en"));
        mt::MtModel *fromEn = loadOrFail(QStringLiteral("en"), dst);
        if (toEn == nullptr || fromEn == nullptr) {
            if (error.isEmpty()) {
                error = i18n::s("缺少英文中转所需的模型", "Models required for English pivoting are missing");
            }
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

    case RouteKind::None:
        break;
    }

    error = plan.error.isEmpty()
                ? i18n::sf("没有可用的本地翻译路径：%1→%2",
                           "No usable local MT route for %1→%2",
                           {core::LanguageRegistry::normalize(src),
                            core::LanguageRegistry::normalize(dst)})
                : plan.error;
    return false;
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
