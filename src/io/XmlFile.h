#pragma once

#include <string>

#include "infra/Xml.h"

namespace npcc::io {

// XML 的"内容是 infra 的事，落盘是 io 的事"——把文件访问放在 io 层，
// 免得 infra 反向依赖 io（设计文档 1.3 的分层规则）。

bool LoadXmlFile(const std::wstring& path, infra::XmlDocument& out, std::wstring* error);
bool SaveXmlFile(const std::wstring& path, const infra::XmlDocument& document,
                 std::wstring* error);

}  // namespace npcc::io
