# Studious

跨平台截图翻译工具（Windows 优先，Linux 预留）。

> 当前阶段：**v0.1 demo** —— 仅后端翻译链路 + 纯文本测试前端，未实现截图/OCR/浮层。

---

## 架构

前后端严格分离，为兄弟目录：

```
Studious/
├─ backend/     纯翻译引擎（平台无关，C++20 + Qt Core）
└─ frontend/    Qt Widgets 界面（截图/选区/浮层/OCR 后续实现）
```

**边界原则**：`backend/` 不接收图像、不返回坐标、不依赖平台头文件。OCR 属于前端（平台能力）。

详见 [`docs/backend-architecture.md`](docs/backend-architecture.md)。

---

## 翻译组件

| 组件 | 状态 | 说明 |
|---|---|---|
| **web** | ✅ 已实现 | Google 免费接口 + MyMemory，双适配器自动降级 |
| **local-mt** | 🚧 接口预留 | 离线小模型（onnxruntime + OPUS-MT），走英文中转 |
| **llm** | 🚧 接口预留 | llama-server 子进程（CPU + Vulkan），OpenAI 兼容协议 |
| **mock** | ✅ 已实现 | 离线自测用，不联网 |

降级链：`mock → web → llm → local-mt`（可按配置调整）。

---

## 构建

### 依赖

| 组件 | 版本 | 路径示例 |
|---|---|---|
| Qt | 6.12.0 (mingw_64) | `C:\Code\language\Qt\6.12.0\mingw_64` |
| MinGW | GCC 13.1.0 | `C:\Code\language\Qt\Tools\mingw1310_64\bin` |
| CMake | 3.30+ | `C:\Code\language\Qt\Tools\CMake_64\bin` |
| Ninja | 1.12+ | `C:\Code\language\Qt\Tools\Ninja` |

### 一键构建

```powershell
# 1. 环境
$env:Path = "C:\Code\language\Qt\Tools\mingw1310_64\bin;C:\Code\language\Qt\Tools\CMake_64\bin;C:\Code\language\Qt\Tools\Ninja;C:\Code\language\Qt\6.12.0\mingw_64\bin;" + $env:Path

# 2. 后端
cmake -S backend -B backend/build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/Code/language/Qt/6.12.0/mingw_64" `
  -DCMAKE_CXX_COMPILER="C:/Code/language/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build backend/build

# 3. 前端
cmake -S frontend -B frontend/build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/Code/language/Qt/6.12.0/mingw_64" `
  -DCMAKE_CXX_COMPILER="C:/Code/language/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build frontend/build
```

或直接运行 `.\build.ps1`。

### 后端自测

```powershell
.\backend\build\backend_selftest.exe --offline   # 离线核心测试
.\backend\build\backend_selftest.exe             # 含联网 web 翻译测试
```

---

## 运行 demo

```powershell
.\frontend\build\studious.exe
```

输入文本 → 选目标语言 → 点「翻译」。

---

## 许可

待定（倾向 Apache-2.0）。

## 贡献者

见 [`CONTRIBUTORS.md`](CONTRIBUTORS.md)。
