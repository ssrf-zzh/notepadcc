#include "io/XmlFile.h"

#include "io/AtomicFile.h"
#include "io/FileReader.h"

namespace npcc::io {

bool LoadXmlFile(const std::wstring& path, infra::XmlDocument& out, std::wstring* error) {
    const ReadResult result = ReadWholeFile(path, 16ull * 1024 * 1024);
    if (!result.ok) {
        if (error != nullptr) {
            *error = result.message;
        }
        return false;
    }
    return infra::XmlDocument::Parse(result.bytes, out, error);
}

bool SaveXmlFile(const std::wstring& path, const infra::XmlDocument& document,
                 std::wstring* error) {
    const std::string bytes = document.ToUtf8();
    const WriteResult result = WriteFileAtomic(path, bytes);
    if (!result.ok && error != nullptr) {
        *error = result.message;
    }
    return result.ok;
}

}  // namespace npcc::io
