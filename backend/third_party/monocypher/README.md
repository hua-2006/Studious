# Monocypher（third_party）

## 来源

- 上游仓库：<https://github.com/LoupVaillant/Monocypher>
- 版本：**4.0.2**（单文件发布版，本目录直接落地上游的 4 个文件，未做任何修改）
- 落地文件：
  - `monocypher.c` / `monocypher.h` —— 主库（BLAKE2b、ChaCha20、Poly1305、Argon2、X25519、`crypto_eddsa_*` 等）
  - `monocypher-ed25519.c` / `monocypher-ed25519.h` —— RFC 8032 兼容的 Ed25519（SHA-512 版）

## 许可证

上游为**双许可**，任选其一：

- **BSD-2-Clause**（`SPDX-License-Identifier: BSD-2-Clause`）
- **CC0-1.0**（公有领域贡献，`SPDX-License-Identifier: CC0-1.0`）

即：`BSD-2-Clause OR CC0-1.0`。版权声明见各源文件头部（Copyright (c) 2017-2019, Loup Vaillant）。
本项目按 **BSD-2-Clause** 使用（保留版权与免责声明即可，二进制分发需在文档中复述）。
Monocypher 上游另附 `LICENCE.md`（含两种许可全文），本目录未重复拷贝，需要全文时回上游取。

## 为什么选它

1. **单文件、零依赖**：`monocypher.c` + `monocypher-ed25519.c` 直接进我们的静态库，不需要
   OpenSSL / libsodium / vcpkg / 任何外部构建系统，也不需要处理 Windows 上的 DLL 分发问题。
   交叉编译（MinGW → 纯 C99）零摩擦。
2. **体积与审计成本**：核心实现约 3k 行、无宏魔法，作为"资产清单验签"这种单点需求足够，
   不必为了一个 Ed25519 把整个 OpenSSL 拖进产品。
3. **API 极简**：`crypto_ed25519_key_pair / sign / check` 三个函数，返回值只有 0/-1，
   调用方不需要理解任何上下文对象或错误码枚举。

## 为什么用 `monocypher-ed25519` 而不是 `crypto_eddsa_*`

主库里的 `crypto_eddsa_sign` / `crypto_eddsa_check` 是 **EdDSA over BLAKE2b** 变体，
**不与标准 Ed25519 互通**：同一私钥/消息用 `crypto_eddsa_*` 和 RFC 8032 实现签出来的字节完全不同。

我们的清单签名必须能和**其它工具链互通**（发布机器上可能是 Python `cryptography`、Go
`ed25519`、`ssh-keygen`、`openssl pkeyutl -sign -rawin`、签名服务器 KMS……），所以只用
`monocypher-ed25519.c/h` 里的 RFC 8032 版本：

```c
void crypto_ed25519_key_pair(uint8_t secret_key[64], uint8_t public_key[32], uint8_t seed[32]);
void crypto_ed25519_sign   (uint8_t signature[64], const uint8_t secret_key[64],
                            const uint8_t *msg, size_t len);
int  crypto_ed25519_check  (const uint8_t signature[64], const uint8_t public_key[32],
                            const uint8_t *msg, size_t len);   // 0 = 通过，-1 = 失败
```

## 在本项目里的使用点

| 位置 | 用途 |
|---|---|
| `backend/src/asset/ManifestParser.cpp` | `crypto_ed25519_check` 校验清单信封的 `sig` |
| `backend/tools/manifest_tool.cpp` | `crypto_ed25519_key_pair` 生成密钥、`crypto_ed25519_sign` 签清单（发布工具，不随产品分发） |
| `backend/src/selftest_main.cpp` | 用**固定的 TEST seed** 在内存里造签名，覆盖验签/篡改用例 |

构建注记：`backend` 静态库里同时编入 `monocypher.c` 与 `monocypher-ed25519.c`
（所以 `CMakeLists.txt` 的 `project(... LANGUAGES C CXX)` 里**必须有 C**）；
两者内部符号有依赖，**不能只打包其中一个**。
