# miniz（zip 解压）

- 来源：<https://github.com/richgel999/miniz> 发布包 `3.0.2`
- 许可证：MIT（见同目录 `LICENSE`）
- 形态：**官方 amalgamation 单文件版**（`miniz.c` + `miniz.h`），不是仓库 master 的拆分版

## 为什么用这个版本

仓库 master 是拆分结构（`miniz.c` / `miniz_tdef.c` / `miniz_tinfl.c` / `miniz_zip.c` + 若干头文件），
其中 `miniz.h` 还会 `#include "miniz_export.h"` 和 `"miniz_common.h"`，而这两个文件平时由
CMake 的 `generate_export_header()` 生成。直接源码引入会编译失败。

发布包里的 amalgamation 版是**两文件、零生成步骤**，正好适合 vendor 进本项目。

## 用法

```c
#include "miniz.h"

mz_zip_archive zip;
memset(&zip, 0, sizeof(zip));
mz_zip_reader_init_file(&zip, path, 0);
mz_zip_reader_get_num_files(&zip);
// 逐条读取，务必自行校验条目路径，防 zip-slip
mz_zip_reader_end(&zip);
```

## 安全要求（重要）

解压前必须逐条检查 `mz_zip_reader_get_filename()` 的结果：

- 拒绝包含 `..` 的路径段
- 拒绝绝对路径（`/` 开头、`C:\` 形式）
- 拒绝符号链接条目

否则恶意 zip 可以写到目标目录之外（zip-slip / zip bomb）。

## 已验证

用 mingw gcc 13.1 编译单文件版并成功读取一个测试 zip：

```
entries=1
first=hello.txt
```
