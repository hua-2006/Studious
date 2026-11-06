#pragma once

#include <QString>
#include <QStringList>

// 前端界面文案（业务语言：中文 / English）。
//
// 设计取舍：
//   · 不用 Qt 的 tr()/QTranslator —— 那要 .ts/.qm 文件和 lupdate 流程，
//     而本项目只有两种语言、字符串总量可控，一个结构体更直接、编译期就能查错。
//   · 用结构体字段而不是 key-value 表：拼错字段名 = 编译失败，不会静默漏字。
//   · 与后端 i18n 解耦：后端错误文案由 studious::i18n 决定（见下方 kBackendLanguage），
//     前端只负责自己的界面文字。
namespace studious::ui::i18n {

enum class Lang { Chinese, English };

// 当前界面语言。
Lang current();

// 切换界面语言。只影响前端文案，不影响后端。
void setLanguage(Lang lang);

// 跟随系统（QLocale::system().language() == Chinese → 中文，否则英文）。
Lang detectSystemLanguage();

// 语言自身的显示名，用于「外观 → 界面语言」下拉。
QString displayName(Lang lang);

// 全部界面文案。按页面分组。
struct Strings {
    struct Nav {
        QString translate, hotkeys, config, settings, appearance;
        QString appTitle;   // 窗口标题
        QString tagline;    // 副标题
    } nav;

    struct Common {
        QString ok, cancel, save, reset, delete_, close, retry, apply;
        QString enabled, disabled, none, unknown, notSet;
        QString ready, notReady, missingAsset, unsupported;
        QString copy, copyAll, paste, clear, refresh, browse;
        QString yes, no, confirm, back;
        QString unitParagraph, unitChars, unitItems;
        QString seconds, milliseconds;
        QString loading;
    } common;

    struct Translate {
        QString heading, subheading;
        QString srcLang, dstLang, dstLangHint, autoDetect, moreLangs;
        QString engine, engineAuto, engineWeb, engineLlmCloud, engineLlmLocal, engineMt;
        QString originalPane, translatedPane;
        QString originalCompare, cacheKeyHasEngine;
        QString run, runHint, running, cancelHint;
        QString passthroughBadge, copySelection;
        QString statusLine, engineStatus, viewEngineStatus;
        QString emptyOriginal, emptyTranslated;
        QString detectConfidence;
    } translate;

    struct Hotkeys {
        QString heading, subheading;
        QString sceneGroup, sceneGroupHint;
        QString captureGroup, clipboardGroup, windowGroup, manageGroup;
        QString conflictCheck;
        QString colAction, colKey, colScope, colStatus;
        QString scopeGlobal, scopeApp;
        QString pickKey, clearKey, restoreDefaults;
        QString platformUnsupported, conflictWith;
        QString actionCaptureRegion, actionCaptureWindow, actionCaptureScreen;
        QString actionRepeatLast, actionTranslateClipboard, actionCopyBack;
        QString actionShowWindow, actionCancelTranslate, actionQuit;
        QString actionToggleOverlay, actionLockOverlay, actionClearOverlays;
    } hotkeys;

    struct Config {
        QString heading, subheading;
        QString engineSection, priorityHint, moveUp, moveDown;
        QString statusPanel, statusPanelHint;
        QString webSection, providerNote;
        QString azureKey, azureRegion;
        QString llmSection, baseUrl, apiKey, model, fetchModels;
        QString temperature, maxTokens, systemPrompt, systemPromptHint;
        QString reasoningSection, reasoningLevel, probeReasoning, probeHint;
        QString batchSize, timeoutMs, retryBudgetMs, stripModelNotes, stripModelNotesHint;
        QString localMtSection, installedPairs, routeHint, needDownload;
        QString localLlmSection, modelFile, contextSize, threads, gpuLayers, extraArgs;
        QString startServer, stopServer, port, pid, logTail;
        QString probeDisabled, probeStillThinking, probeUnknown;
    } config;

    struct Settings {
        QString heading, subheading;
        QString general, uiLanguage, followSystem, startOnBoot, minimizeToTray;
        QString closeBehavior, closeExit, closeMinimize, singleInstance;
        QString behavior, passthroughHint, suspiciousWarning, autoDetectSource;
        QString cacheSection, cacheEnabled, cacheCount, cacheSize, cachePath;
        QString cacheHits, cacheMisses, cacheClear, cacheResetStats;
        QString network, proxy, proxyHint, proxyRestartRequired, timeout;
        QString assets, assetList, assetStatus, assetSize, assetPath;
        QString download, downloadCancel, uninstall, verify, importFile, resumable;
        QString manifest, manifestKeyId, manifestSchema, manifestCheckUpdate, manifestVerified;
        QString totalInstalled, data, installRoot, logDir, resetApp;
        QString about, version, thirdParty, license;
        QString placeholderUnsupported;
    } settings;

    struct Appearance {
        QString heading, subheading;
        QString theme, themeSystem, themeLight, themeDark;
        QString font, uiFont, transFont, scale;
        QString mainWindow, compact, standard, statusDensity;
        QString langDisplay, shortForm, fullForm;
        QString clearCacheTitle, clearCacheBody;
        QString resetAppTitle, resetAppBody;
    } appearance;
};

// 当前语言的文案表。
const Strings &s();

} // namespace studious::ui::i18n
