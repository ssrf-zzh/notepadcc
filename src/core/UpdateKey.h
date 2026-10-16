#pragma once

namespace npcc::core {

// 更新清单验签用的 RSA-2048 公钥：BCrypt RSAPUBLICBLOB 格式的 base64（见 tools/make_update_key.ps1）。
// 公钥不是秘密，直接内嵌；私钥由发布方离线保管。
//
// 注意：仓库里当前这把是**开发/CI 用的一次性钥匙**（它的私钥也放在回归脚本里，
// 只用来给测试清单签名）。正式发布前必须用离线生成的真钥匙重新生成这个文件，
// 并同步更新 tools/ui_smoke_test.py 里的测试签名钥匙。
inline constexpr const char* kUpdatePublicKeyBase64 =
    "UlNBMQAIAAADAAAAAAEAAAAAAAAAAAAAAQABq7uKXBrlpmEEGUoYNuc4Vj2bhTWkRueW1JSPHtgX47ikAMNGP+RCkmUrbd05fZ99VRVm72sXpiixtjlXwoHtdNlBdk4D6zKi3rMFXXTszE8sLeHf1qKlfegL+5fgr1CvDQHGvsdp1INgKh9uvMo//UQ6GpdHy0ntapRNqJI3lFbuD+gv79sQ8ngBZQo4hQeqmBfkaSmB+kpkI/LHOJ7fq3or0LCgKJR+lIuu0gKgszuVJa5/PG3F9wpcMj9tfRxQBsDu6T13rrtHSnOpe7q0At4b//OGVz4mdMtaU58sEEGyU/tdgFgDId7TAurTcC1dAGaz252TCMZQRUd7++rBXQ==";

}  // namespace npcc::core
