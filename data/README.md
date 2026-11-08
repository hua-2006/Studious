# data/ —— 运行时数据（不入库）

这个目录**只有结构进版本库**（就是本文件），里面的模型、日志、数据库都是可再生的，
不提交。想恢复：启动程序后在「设置 · 资产」里下载即可，路径会自动落到这里。

```
data/
├─ assets/                     资产根（设置里可以改到别的盘）
│  ├─ models/                  按用途分三格，别混着放
│  │  ├─ llm/                  本地 LLM 的 GGUF
│  │  │  └─ model.qwen2.5-3b-instruct.q4km
│  │  ├─ mt/                   本地 MT 语言包（每对一个目录）
│  │  │  └─ mt.opus.en-zh/{encoder,decoder-quantized.onnx,source.spm,target.spm,vocab.json,config.json}
│  │  └─ ocr/                  OCR 三件套
│  │     ├─ ocr.det.ppocrv4
│  │     ├─ ocr.rec.ppocrv5
│  │     ├─ ocr.dict.ppocrv5
│  │     └─ run/               启动时收拢出来的 det.onnx / rec.onnx / ppocr_keys_v1.txt
│  ├─ runtime/                 本地 LLM 的加速组件
│  │  └─ runtime.llama.{vulkan,cpu}.win-x64/
│  ├─ downloads/               .part 临时文件（可续传，可随时删）
│  ├─ state/                   安装状态与来源
│  │  └─ asset-origins.json    download / import（用户导入的不校验哈希）
│  └─ logs/                    llama-server 的日志
├─ logs/                       应用日志（studious.log，8 MB 轮转）
├─ records/                    翻译记录 capture.db / mini.db / live.db
├─ cache.db                    翻译缓存
└─ records.db                  旧版合并库（只为兼容，新数据不进这里）
```

规则：

- 目录位置由 exe 决定 —— 从 exe 往上找 `version.json`，找到就用它旁边的 `data/`。
  整个目录可以直接打包搬走，换机器不用改配置。
- `models/` 下三格是**硬约定**：`AssetRegistry` 按资产 id 前缀落位
  （`mt.` → `mt/`，`ocr.` → `ocr/`，其余 → `llm/`），手动挪动会导致「已安装」失效。
- 用户自己导入的文件记 `origin=import`，只确认文件在、能读，不做哈希比对；
  线上下载的那份每次启动前会按清单核对。
