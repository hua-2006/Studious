#pragma once

namespace studious::asset {

// 清单验签公钥（编译进二进制，不从网络获取）。
// ⚠️ 私钥必须离线保存，绝不能进仓库；更换密钥 = 发新版客户端。
//
// 当前这对密钥是开发期用途（keyId = studious-dev），由 backend/tools/manifest_tool.cpp 的
// `genkey` 在 2026-11-04 生成。正式发版前必须重新生成一对，并且：
//   1. seed（私钥）只保存在离线机器 / 硬件令牌里，绝不写进仓库；
//   2. 只把 public key 的 64 位 hex 换到这里，同时更新 kManifestKeyId；
//   3. 服务端下发清单时用同一 keyId 标识，客户端按 keyId 选择对应公钥（多密钥轮换用）。
inline constexpr const char *kManifestPublicKeyHex =
    "aee979edc444c1aea210ff4f79c2284c0a775fc8390b302bb33f91f5f61fe859";
inline constexpr const char *kManifestKeyId = "studious-dev";

} // namespace studious::asset
