# data/ —— 运行时数据（不入库）

这个目录**只有结构进版本库**（就是本文件），里面的模型、日志、数据库都是可再生的，
不提交。想恢复：启动程序后在「设置 · 资产」里下载即可，路径会自动落到这里。

```
data/
├─ assets/                     资产根（设置里可以改到别的盘）
│  ├─ models/                  按用途分块，别混着放
│  │  ├─ llm/                  清单里下载的 GGUF
│  │  │  └─ model.qwen2.5-3b-instruct.q4km
│  │  ├─ llm-user/             你自己导入的 GGUF（只验文件头）
│  │  │  └─ Qwen2.5-0.5B-Instruct-Q4_K_M.gguf
│  │  ├─ mt/                   本地 MT 语言包（每对一个目录）
│  │  │  └─ mt.opus.en-zh/{encoder,decoder-quantized.onnx,source.spm,target.spm,vocab.json,config.json}
│  │  ├─ mt-user/              你自己放进来的语言包（六件套齐全才认）
│  │  └─ ocr/                  OCR 三件套
│  │     ├─ ocr.det.ppocrv4
│  │     ├─ ocr.rec.ppocrv5
│  │     ├─ ocr.dict.ppocrv5
│  │     └─ run/               启动时收拢出来的 det.onnx / rec.onnx / ppocr_keys_v1.txt
│  ├─ runtime/                 本地 LLM 的加速组件
│  │  └─ runtime.llama.{vulkan,cpu}.win-x64/
│  ├─ downloads/               .part 临时文件（可续传，可随时删）
│  ├─ state/                   安装状态与来源
│  │  ├─ asset-origins.json    download / import（用户导入的不校验哈希）
│  │  └─ imported-llm.json     导入过的 GGUF 登记（名字、大小、来源路径）
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
- `llm-user/`、`mt-user/` 是**用户自己的**，不参与清单核对：GGUF 只验文件头（开头四个
  字节是 `GGUF`）和大小，语言包认六件套齐不齐。清单资产和用户资产互不影响 —— 卸载清单
  里那条不会动这两格，反过来也一样。
- 清单里那几格（`llm/`、`mt/`、`ocr/`、`runtime/`）每次启动前按清单核对 sha256，
  对不上就标回未安装。`state/imported-llm.json` 只记导入过哪些 GGUF（名字、大小、
  来源路径），给界面列表和「删除」用。
