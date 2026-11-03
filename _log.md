# Studious 开发日志

## 2026-11-04 前端 demo（frontend/，纯文本翻译测试，无截图/OCR/浮层）

### 成功路线
1. 读后端头文件确认 API：`router::TranslatorList = std::vector<std::unique_ptr<ITranslator>>`、
   `factory::makeDefaultTranslators(net::IHttpClient&)`、`TranslatorRouter::translate/status`。
2. `frontend/CMakeLists.txt`：`find_package(Qt6 Core Widgets Concurrent)` +
   `add_subdirectory(../backend ${CMAKE_BINARY_DIR}/backend)` + 执行文件**不加 WIN32**（保住控制台看 smoke 输出）。
3. `TranslateJob`：`QtConcurrent::run(translateInWorkerThread, req)` +
   `QFutureWatcher<core::TranslateOutcome>`；工作线程内构造 `QtHttpClient` → factory → router，
   跨线程只搬 `TranslateRequest` / `TranslateOutcome` 纯数据。
4. `MainWindow`：顶部语言/引擎行、QSplitter 左右原文译文、底部 status + engineStatus；
   Ctrl+Return / Ctrl+Enter 双 QShortcut；status 每帧用主线程临时 `QtHttpClient` + `router.status()` 刷新。
5. `main.cpp --smoke`：`QCoreApplication` + `TranslateJob` + `QEventLoop` + 60s 超时保护，UTF-8 字节直写 stdout。
6. 验证：clean 构建 23 步 0 warning / 0 error；web 冒烟 exit 0 出「您好，世界！」；
   mock 冒烟 exit 0 出「【ja】Hello」；GUI 起 3s 存活且标题正确；backend/ 0 文件被改动。

### 踩坑
- **TranslatorList 不能用 QVector**：Qt6 QList 要求元素可拷贝，`std::unique_ptr` 直接编译失败 → 用 `std::vector`。
- **QtHttpClient 线程亲和**：QNetworkAccessManager 只能在本线程用。共享会踩
  "Cannot create children for a parent in a different thread" 及请求不回调。每次作业现造一套（微秒级）。
- **WIN32 子系统砍控制台**：`add_executable(... WIN32)` 后 `--smoke` 什么都看不到 → 不加 WIN32，
  GUI 照样弹窗（实测）。
- **QFutureWatcher 结果取回**：`finished` 信号里调 `result()` 即可；`TranslateOutcome` 可拷贝，无需 `Q_DECLARE_METATYPE`。
- **重复快捷键会 ambiguous**：别同时给按钮 `setShortcut(Ctrl+Return)` 又注册同序列 QShortcut，
  会变成 ambiguous shortcut 谁都不触发。改成只注册 `Ctrl+Return` + `Ctrl+Enter` 两个不同序列。
- **运行需 Qt DLL 在 PATH**（MinGW，无水印部署）：`C:\Code\language\Qt\6.12.0\mingw_64\bin`。
  否则直接 `exit -1073741515 (0xC0000135, DLL_NOT_FOUND)`。
- **后端 `add_subdirectory` 不影响 exe 输出目录**：backend/CMakeLists 里的
  `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 只作用于子目录 scope，产物仍在 `frontend/build/studious.exe`。
- **默认「自动」引擎实际走 mock**：后端默认优先级 `mock → web → llm → local-mt`，mock 永远 ready。
  要看真翻译必须显式选 `web`。（demo 行为，不改后端）
- **web 冒烟 ~16s**：Google 免费接口在该网络下走 15s 超时，然后自动降级 MyMemory 成功。
  stderr 有一句 `QIODevice::read (QNetworkReplyHttpImpl): device not open`——后端 abort 后 readAll 的噪音，无害。
- **控制台编码**：本机 `[Console]::OutputEncoding = utf-8 (65001)`，UTF-8 字节直写即可正常显示中文；
  老 cp936 控制台需先 `chcp 65001`。
