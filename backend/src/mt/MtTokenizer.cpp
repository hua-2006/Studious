#include "mt/MtTokenizer.h"

#include "i18n/L10n.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <sentencepiece_processor.h>

#include <string>
#include <string_view>
#include <vector>

namespace studious::mt {
namespace {

// vocab.json 里的特殊符号（模型词表里它们的 id 是固定的：</s>=0、<unk>=1、<pad>=65000）。
bool isSpecialPiece(const QString &piece)
{
    return piece == QLatin1String("</s>") || piece == QLatin1String("<unk>")
           || piece == QLatin1String("<pad>") || piece == QLatin1String("<s>");
}

} // namespace

struct MtTokenizer::Impl {
    sentencepiece::SentencePieceProcessor processor;

    // 只在加载了 vocab.json 时填充：模型 id 空间 <-> piece。
    QHash<QString, int> pieceToId;
    QHash<int, QString> idToPiece;
    int unkId = 1;

    bool hasVocab = false;
    bool loaded = false;
};

MtTokenizer::MtTokenizer() : m_impl(std::make_unique<Impl>()) {}

MtTokenizer::~MtTokenizer() = default;

bool MtTokenizer::load(const QString &spmPath, QString *error)
{
    return loadImpl(spmPath, QString(), error);
}

bool MtTokenizer::load(const QString &spmPath, const QString &vocabJsonPath, QString *error)
{
    return loadImpl(spmPath, vocabJsonPath, error);
}

bool MtTokenizer::loadImpl(const QString &spmPath, const QString &vocabJsonPath, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    m_impl->loaded = false;
    m_impl->hasVocab = false;
    m_impl->pieceToId.clear();
    m_impl->idToPiece.clear();

    if (spmPath.isEmpty() || !QFileInfo::exists(spmPath)) {
        if (error != nullptr) {
            *error = i18n::sf("分词模型不存在：%1", "Tokenizer model not found: %1", {spmPath});
        }
        return false;
    }

    // sentencepiece 内部按本地编码 fopen 模型文件（Windows 上是 ANSI 代码页），
    // 所以路径必须走 toLocal8Bit()；用 toUtf8() 在中文用户名下会直接打不开。
    const QByteArray localPath = QDir::toNativeSeparators(spmPath).toLocal8Bit();
    const sentencepiece::util::Status status =
        m_impl->processor.Load(std::string_view(localPath.constData(), static_cast<size_t>(localPath.size())));
    if (!status.ok()) {
        if (error != nullptr) {
            *error = i18n::sf("分词模型加载失败：%1（%2）",
                              "Failed to load tokenizer model: %1 (%2)",
                              {spmPath, QString::fromStdString(status.ToString())});
        }
        return false;
    }

    if (!vocabJsonPath.isEmpty()) {
        QFile file(vocabJsonPath);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error != nullptr) {
                *error = i18n::sf("模型词表打不开：%1", "Cannot open model vocabulary: %1", {vocabJsonPath});
            }
            return false;
        }

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (doc.isNull() || !doc.isObject()) {
            if (error != nullptr) {
                *error = i18n::sf("模型词表解析失败：%1（%2）",
                                  "Failed to parse model vocabulary: %1 (%2)",
                                  {vocabJsonPath, parseError.errorString()});
            }
            return false;
        }

        const QJsonObject obj = doc.object();
        m_impl->pieceToId.reserve(obj.size());
        m_impl->idToPiece.reserve(obj.size());
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const int id = it.value().toInt(-1);
            if (id < 0) {
                continue;
            }
            m_impl->pieceToId.insert(it.key(), id);
            m_impl->idToPiece.insert(id, it.key());
        }
        m_impl->unkId = m_impl->pieceToId.value(QStringLiteral("<unk>"), 1);
        m_impl->hasVocab = !m_impl->pieceToId.isEmpty();
    }

    m_impl->loaded = true;
    return true;
}

QVector<int> MtTokenizer::encode(const QString &text) const
{
    if (!m_impl->loaded) {
        return {};
    }

    const QByteArray utf8 = text.toUtf8();
    const std::string_view view(utf8.constData(), static_cast<size_t>(utf8.size()));

    QVector<int> out;
    if (!m_impl->hasVocab) {
        // 没有 vocab.json：直接用 spm 的 id（只在 spm 本身就是模型词表时才对）。
        const std::vector<int> ids = m_impl->processor.EncodeAsIds(view);
        out.reserve(static_cast<int>(ids.size()));
        for (const int id : ids) {
            out.append(id);
        }
        return out;
    }

    // 有 vocab.json：spm 只负责切词，id 由模型词表决定。
    const std::vector<std::string> pieces = m_impl->processor.EncodeAsPieces(view);
    out.reserve(static_cast<int>(pieces.size()));
    for (const std::string &piece : pieces) {
        out.append(m_impl->pieceToId.value(QString::fromStdString(piece), m_impl->unkId));
    }
    return out;
}

QStringList MtTokenizer::encodePieces(const QString &text) const
{
    QStringList out;
    if (!m_impl->loaded) {
        return out;
    }

    const QByteArray utf8 = text.toUtf8();
    const std::string_view view(utf8.constData(), static_cast<size_t>(utf8.size()));
    for (const std::string &piece : m_impl->processor.EncodeAsPieces(view)) {
        out << QString::fromStdString(piece);
    }
    return out;
}

QString MtTokenizer::decode(const QVector<int> &ids) const
{
    if (!m_impl->loaded) {
        return {};
    }

    if (!m_impl->hasVocab) {
        const int pieceSize = m_impl->processor.GetPieceSize();
        std::vector<int> filtered;
        filtered.reserve(static_cast<size_t>(ids.size()));
        for (const int id : ids) {
            if (id < 0 || id >= pieceSize) {
                continue;
            }
            if (m_impl->processor.IsControl(id)) {
                continue;
            }
            filtered.push_back(id);
        }
        const std::string text = m_impl->processor.DecodeIds(filtered);
        return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
    }

    // 模型 id -> piece -> 用目标 spm 反分词（▁ -> 空格、中文不加空格都由它负责）。
    std::vector<std::string> pieces;
    pieces.reserve(static_cast<size_t>(ids.size()));
    for (const int id : ids) {
        const auto it = m_impl->idToPiece.constFind(id);
        if (it == m_impl->idToPiece.constEnd()) {
            continue;   // 越界 / pad / 词表外的 id，直接丢
        }
        if (isSpecialPiece(it.value())) {
            continue;
        }
        pieces.push_back(it.value().toStdString());
    }

    const std::string text = m_impl->processor.DecodePieces(pieces);
    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

bool MtTokenizer::isLoaded() const
{
    return m_impl->loaded;
}

int MtTokenizer::eosId() const
{
    if (!m_impl->loaded) {
        return -1;
    }
    if (m_impl->hasVocab) {
        return m_impl->pieceToId.value(QStringLiteral("</s>"), m_impl->processor.eos_id());
    }
    return m_impl->processor.eos_id();
}

int MtTokenizer::vocabSize() const
{
    if (!m_impl->loaded) {
        return 0;
    }
    return m_impl->hasVocab ? m_impl->pieceToId.size() : m_impl->processor.GetPieceSize();
}

std::string MtTokenizer::pieceOf(int id) const
{
    if (!m_impl->loaded) {
        return {};
    }
    if (m_impl->hasVocab) {
        return m_impl->idToPiece.value(id).toStdString();
    }
    if (id < 0 || id >= m_impl->processor.GetPieceSize()) {
        return {};
    }
    return m_impl->processor.IdToPiece(id);
}

int MtTokenizer::pieceToId(const QString &piece) const
{
    if (!m_impl->loaded || piece.isEmpty()) {
        return -1;
    }
    if (m_impl->hasVocab) {
        return m_impl->pieceToId.value(piece, -1);
    }

    // 没有 vocab.json 时退回 spm 自己的 id 空间：spm 对未知 piece 会返回 unk，
    // 那对"手工拼特殊 token"来说等于没找到，按 -1 报。
    const int id = m_impl->processor.PieceToId(piece.toStdString());
    if (id < 0 || m_impl->processor.IsUnknown(id)) {
        return -1;
    }
    return id;
}

} // namespace studious::mt
