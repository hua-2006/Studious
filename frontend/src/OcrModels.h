#pragma once

#include <QString>

namespace studious::ui::ocr {

// 把 OCR 三件套收拢到 <资产根>/models/ocr/ 下，返回那个目录。
//
// 为什么要这一步：资产是「一条资产一个文件」装的（落点 models/<assetId>），
// 而 OcrEngine 只收一个目录。这里统一做中转，ScreenFlow 与 LiveSession
// 都走这一份实现（以前各抄一份，改模型集合要改两处）。
QString stageModels();

} // namespace studious::ui::ocr
