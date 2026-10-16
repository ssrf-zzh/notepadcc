#include "core/FtpProtocol.h"

#include <cctype>
#include <cstring>
#include <cstdlib>

namespace npcc::core {
namespace {

bool IsDigits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

bool IsModeString(const std::string& text) {
    if (text.size() < 10) {
        return false;
    }
    for (std::size_t index = 1; index < 10; ++index) {
        const char ch = text[index];
        if (ch != 'r' && ch != 'w' && ch != 'x' && ch != 's' && ch != 't' && ch != '-') {
            return false;
        }
    }
    return true;
}

std::string StripLineEnds(const std::string& line) {
    std::string out = line;
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> SplitWhitespace(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        if (index >= line.size()) {
            break;
        }
        const std::size_t start = index;
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
            ++index;
        }
        tokens.push_back(line.substr(start, index - start));
    }
    return tokens;
}

// 第 count 个字段之后的剩余内容（跳过字段之间的一串空白）
bool RestAfterTokens(const std::string& line, const std::size_t count, std::string* rest) {
    std::size_t index = 0;
    std::size_t seen = 0;
    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        if (index >= line.size()) {
            return false;
        }
        if (seen == count) {
            *rest = line.substr(index);
            return true;
        }
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
            ++index;
        }
        ++seen;
    }
    return false;
}

bool ParseUnixListLine(const std::string& line, FtpListEntry* out) {
    const char type = line[0];
    if (type != '-' && type != 'd' && type != 'l') {
        return false;
    }
    if (!IsModeString(line)) {
        return false;
    }
    const std::vector<std::string> tokens = SplitWhitespace(line);
    // 字段：权限 链接 属主 属组 大小 月 日 时间/年 名字……
    if (tokens.size() < 9) {
        return false;
    }
    if (!IsDigits(tokens[4])) {
        return false;   // 设备文件那种 "major,minor" 不认，跳过
    }
    std::string name;
    if (!RestAfterTokens(line, 8, &name)) {
        return false;
    }
    if (type == 'l') {
        const std::size_t arrow = name.find(" -> ");
        if (arrow != std::string::npos) {
            name = name.substr(0, arrow);   // 符号链接只留名字本身
        }
    }
    out->name = name;
    out->size = std::strtoull(tokens[4].c_str(), nullptr, 10);
    out->isDir = type == 'd';
    return true;
}

bool LooksLikeDosDate(const std::string& token) {
    if (token.size() != 8) {
        return false;
    }
    if (token[2] != '-' || token[5] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < token.size(); ++index) {
        if (index == 2 || index == 5) {
            continue;
        }
        if (token[index] < '0' || token[index] > '9') {
            return false;
        }
    }
    return true;
}

bool ParseDosListLine(const std::string& line, FtpListEntry* out) {
    const std::vector<std::string> tokens = SplitWhitespace(line);
    if (tokens.size() < 4) {
        return false;
    }
    if (!LooksLikeDosDate(tokens[0])) {
        return false;
    }
    std::string sizeToken = tokens[2];
    for (char& ch : sizeToken) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    std::string name;
    if (!RestAfterTokens(line, 3, &name)) {
        return false;
    }
    if (sizeToken == "<DIR>") {
        out->name = name;
        out->size = 0;
        out->isDir = true;
        return true;
    }
    if (!IsDigits(tokens[2])) {
        return false;
    }
    out->name = name;
    out->size = std::strtoull(tokens[2].c_str(), nullptr, 10);
    out->isDir = false;
    return true;
}

}  // namespace

bool ParseFtpReplyLine(const std::string& line, FtpReply* out) {
    const std::string text = StripLineEnds(line);
    if (text.size() < 3) {
        return false;
    }
    for (std::size_t index = 0; index < 3; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    if (text.size() > 3 && text[3] != ' ' && text[3] != '-') {
        return false;
    }
    out->code = (text[0] - '0') * 100 + (text[1] - '0') * 10 + (text[2] - '0');
    out->text = text.size() > 4 ? text.substr(4) : std::string();
    return true;
}

bool ParseFtpMultilineReply(const std::vector<std::string>& lines, int code, FtpReply* out) {
    if (lines.empty()) {
        return false;
    }
    const std::string head = StripLineEnds(lines[0]);
    if (head.size() < 4 || head[3] != '-') {
        return false;
    }
    if (code == 0) {
        FtpReply first;
        if (!ParseFtpReplyLine(head, &first)) {
            return false;
        }
        code = first.code;
    }
    char prefix[4] = {static_cast<char>('0' + code / 100),
                      static_cast<char>('0' + (code / 10) % 10),
                      static_cast<char>('0' + code % 10), '\0'};
    const std::string codeText(prefix);
    std::string body;
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const std::string current = StripLineEnds(lines[index]);
        const bool isEnd = current.rfind(codeText, 0) == 0 &&
                           (current.size() == 3 || current[3] == ' ');
        if (isEnd) {
            std::string tail = current.size() > 4 ? current.substr(4) : std::string();
            if (!tail.empty()) {
                if (!body.empty()) {
                    body += "\n";
                }
                body += tail;
            }
            out->code = code;
            out->text = body;
            return true;
        }
        if (!current.empty()) {
            if (!body.empty()) {
                body += "\n";
            }
            body += current;
        }
    }
    return false;   // 还没读到收尾行
}

bool ParseFtpPasvReply(const std::string& text, std::string* host, std::uint16_t* port) {
    const std::size_t open = text.find('(');
    const std::size_t close = text.find(')', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return false;
    }
    const std::string inside = text.substr(open + 1, close - open - 1);
    std::vector<int> numbers;
    std::size_t index = 0;
    while (index < inside.size()) {
        while (index < inside.size() && (inside[index] == ' ' || inside[index] == '\t')) {
            ++index;
        }
        if (index >= inside.size()) {
            break;
        }
        if (inside[index] < '0' || inside[index] > '9') {
            return false;
        }
        int value = 0;
        while (index < inside.size() && inside[index] >= '0' && inside[index] <= '9') {
            value = value * 10 + (inside[index] - '0');
            if (value > 255) {
                return false;
            }
            ++index;
        }
        numbers.push_back(value);
        while (index < inside.size() && inside[index] == ' ') {
            ++index;
        }
        if (index < inside.size()) {
            if (inside[index] != ',') {
                return false;
            }
            ++index;
        }
    }
    if (numbers.size() != 6) {
        return false;
    }
    *host = std::to_string(numbers[0]) + "." + std::to_string(numbers[1]) + "." +
            std::to_string(numbers[2]) + "." + std::to_string(numbers[3]);
    *port = static_cast<std::uint16_t>(numbers[4] * 256 + numbers[5]);
    return true;
}

bool ParseFtpListLine(const std::string& line, FtpListEntry* out) {
    const std::string text = StripLineEnds(line);
    if (text.empty()) {
        return false;
    }
    if (IsModeString(text) && (text[0] == '-' || text[0] == 'd' || text[0] == 'l')) {
        return ParseUnixListLine(text, out);
    }
    const std::vector<std::string> tokens = SplitWhitespace(text);
    if (!tokens.empty() && LooksLikeDosDate(tokens[0])) {
        return ParseDosListLine(text, out);
    }
    return false;
}

bool ParseSftpPwdOutput(const std::string& output, std::string* dir) {
    constexpr const char* kMarker = "Remote working directory:";
    std::string found;
    std::size_t index = 0;
    while (index <= output.size()) {
        const std::size_t end = output.find('\n', index);
        std::string line = end == std::string::npos ? output.substr(index)
                                                    : output.substr(index, end - index);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        const std::size_t marker = line.find(kMarker);
        if (marker != std::string::npos) {
            std::string value = line.substr(marker + std::strlen(kMarker));
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(0, 1);
            }
            if (!value.empty()) {
                found = value;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        index = end + 1;
    }
    if (found.empty()) {
        return false;
    }
    if (dir != nullptr) {
        *dir = std::move(found);
    }
    return true;
}

namespace {

// 动词词法：空格分隔的词，每个词可选 '-' 前缀，然后至少一个 ASCII 字母。
// 这样 "ls -l" 能过，而引号、斜杠、控制字符进不来（参数走引号那条路）。
bool IsValidSftpVerb(const std::string& verb) {
    if (verb.empty()) {
        return false;
    }
    std::size_t index = 0;
    while (index < verb.size()) {
        if (verb[index] == ' ') {
            ++index;
            continue;
        }
        if (verb[index] == '-') {
            ++index;
        }
        const std::size_t letters = index;
        while (index < verb.size()) {
            const char ch = verb[index];
            const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            if (!alpha) {
                break;
            }
            ++index;
        }
        if (index == letters) {
            return false;
        }
        if (index < verb.size() && verb[index] != ' ') {
            return false;
        }
    }
    return true;
}

}  // namespace

bool BuildSftpCommand(const std::string& verb, const std::vector<std::string>& args,
                      std::string* out) {
    if (!IsValidSftpVerb(verb)) {
        return false;
    }
    std::string command = verb;
    for (const std::string& arg : args) {
        for (const char ch : arg) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || byte == 0x7F || ch == '"') {
                return false;
            }
        }
        command += " \"";
        command += arg;
        command += '"';
    }
    command += "\n";
    if (out != nullptr) {
        *out = std::move(command);
    }
    return true;
}

}  // namespace npcc::core
