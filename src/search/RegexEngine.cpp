#include "search/RegexEngine.h"

#include <windows.h>

#include <vector>

#include "search/TextBytes.h"

// 必须是包含 pcre2.h 之前的第一个定义
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace npcc::search {
namespace {

// PCRE2 的回溯上限。数值取得足够大以覆盖正常模式，又能及时拦下灾难性回溯
// （例如 (a+)+$ 作用在长 a 串上）。超限时返回 PCRE2_ERROR_MATCHLIMIT。
constexpr std::uint32_t kMatchLimit = 2'000'000;
constexpr std::uint32_t kDepthLimit = 1'000;

std::wstring ToWideUtf8(std::string_view bytes) {
    if (bytes.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                          wide.data(), need);
    return wide;
}

std::wstring DescribePatternError(int errorCode, std::size_t offset) {
    PCRE2_UCHAR buffer[256] = {};
    std::wstring detail;
    if (pcre2_get_error_message(errorCode, buffer, sizeof(buffer)) > 0) {
        detail = ToWideUtf8(std::string_view(reinterpret_cast<const char*>(buffer)));
    }
    std::wstring message = L"正则表达式有误";
    if (!detail.empty()) {
        message += L"：" + detail;
    }
    message += L"（第 " + std::to_wstring(offset + 1) + L" 个字节起）";
    return message;
}

}  // namespace

struct RegexEngine::Impl {
    pcre2_code*          code = nullptr;
    pcre2_match_data*    matchData = nullptr;
    pcre2_match_context* matchContext = nullptr;
    RegexOptions         options;
    bool                 jit = false;
    bool                 lastMatched = false;
    // PCRE2 要求 SUBSTITUTE_MATCHED 的 subject 指针、长度、起始偏移与匹配选项
    // 都与产生该 match_data 的那次 pcre2_match 完全一致，所以必须记下来。
    PCRE2_SIZE           lastStartOffset = 0;
    std::uint32_t        lastMatchOptions = 0;

    ~Impl() {
        if (matchData != nullptr) {
            pcre2_match_data_free(matchData);
        }
        if (matchContext != nullptr) {
            pcre2_match_context_free(matchContext);
        }
        if (code != nullptr) {
            pcre2_code_free(code);
        }
    }

    void Reset() {
        if (matchData != nullptr) {
            pcre2_match_data_free(matchData);
            matchData = nullptr;
        }
        if (matchContext != nullptr) {
            pcre2_match_context_free(matchContext);
            matchContext = nullptr;
        }
        if (code != nullptr) {
            pcre2_code_free(code);
            code = nullptr;
        }
        jit = false;
        lastMatched = false;
        lastStartOffset = 0;
        lastMatchOptions = 0;
    }
};

RegexEngine::RegexEngine() : impl_(std::make_unique<Impl>()) {}
RegexEngine::~RegexEngine() = default;

bool RegexEngine::Ready() const noexcept {
    return impl_->code != nullptr;
}

bool RegexEngine::JitEnabled() const noexcept {
    return impl_->jit;
}

bool RegexEngine::Compile(std::wstring_view pattern, const RegexOptions& options,
                          SearchOutcome& outcome) {
    impl_->Reset();
    impl_->options = options;

    std::string patternBytes;
    if (!ConvertToDocBytes(pattern, options.utfMode, options.codePage, patternBytes)) {
        outcome = SearchOutcome::Failure(SearchError::EncodingFailed,
                                         L"搜索内容无法按当前文档编码表示。");
        return false;
    }
    if (patternBytes.empty()) {
        outcome = SearchOutcome::Failure(SearchError::EmptyPattern, L"搜索内容为空。");
        return false;
    }

    std::uint32_t flags = 0;
    if (options.utfMode) {
        // UTF 模式下让 \w \d \b 也按 Unicode 语义工作，符合用户预期
        flags |= PCRE2_UTF | PCRE2_UCP;
    }
    if (!options.matchCase) {
        flags |= PCRE2_CASELESS;
    }
    if (options.dotMatchesNewline) {
        flags |= PCRE2_DOTALL;
    }

    // 全字匹配靠包裹实现；用 (?:...) 非捕获组，避免改变用户的捕获组编号
    std::string effective = patternBytes;
    if (options.wholeWordOnly) {
        effective = "\\b(?:" + patternBytes + ")\\b";
    }

    int         errorCode = 0;
    PCRE2_SIZE  errorOffset = 0;
    pcre2_code* code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(effective.data()),
        static_cast<PCRE2_SIZE>(effective.size()), flags, &errorCode, &errorOffset, nullptr);
    if (code == nullptr) {
        outcome = SearchOutcome::Failure(SearchError::BadPattern,
                                         DescribePatternError(errorCode, errorOffset));
        return false;
    }

    // JIT 失败不是致命问题，退回解释执行即可
    impl_->jit = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) == 0;

    impl_->matchContext = pcre2_match_context_create(nullptr);
    impl_->matchData = pcre2_match_data_create_from_pattern(code, nullptr);
    if (impl_->matchContext == nullptr || impl_->matchData == nullptr) {
        pcre2_code_free(code);
        outcome = SearchOutcome::Failure(SearchError::InternalError,
                                         L"初始化正则匹配上下文失败。");
        return false;
    }
    pcre2_set_match_limit(impl_->matchContext, kMatchLimit);
    pcre2_set_depth_limit(impl_->matchContext, kDepthLimit);

    impl_->code = code;
    return true;
}

bool RegexEngine::Find(std::string_view subject, std::int64_t fromOffset, MatchRange& match,
                       SearchOutcome& outcome) {
    outcome = SearchOutcome::Success();
    impl_->lastMatched = false;

    if (impl_->code == nullptr) {
        outcome = SearchOutcome::Failure(SearchError::InternalError, L"正则尚未编译。");
        return false;
    }
    if (fromOffset < 0) {
        fromOffset = 0;
    }
    if (static_cast<std::size_t>(fromOffset) > subject.size()) {
        return false;   // 起点已在末尾之外
    }

    const int rc = pcre2_match(impl_->code,
                               reinterpret_cast<PCRE2_SPTR>(subject.data()),
                               static_cast<PCRE2_SIZE>(subject.size()),
                               static_cast<PCRE2_SIZE>(fromOffset), 0, impl_->matchData,
                               impl_->matchContext);
    if (rc == PCRE2_ERROR_NOMATCH) {
        return false;
    }
    if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
        rc == PCRE2_ERROR_HEAPLIMIT) {
        outcome = SearchOutcome::Failure(
            SearchError::MatchLimitExceeded,
            L"正则表达式过于复杂，已在回溯上限内中止。请简化模式或缩小搜索范围。");
        return false;
    }
    if (rc < 0) {
        outcome = SearchOutcome::Failure(SearchError::InternalError,
                                         L"正则匹配失败（PCRE2 错误码 " +
                                             std::to_wstring(rc) + L"）。");
        return false;
    }

    const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(impl_->matchData);
    match.start = static_cast<std::int64_t>(ovector[0]);
    match.end = static_cast<std::int64_t>(ovector[1]);
    impl_->lastMatched = true;
    impl_->lastStartOffset = static_cast<PCRE2_SIZE>(fromOffset);
    impl_->lastMatchOptions = 0;
    return true;
}

bool RegexEngine::ExpandReplacement(std::string_view subject, std::wstring_view replacement,
                                    std::string& out, SearchOutcome& outcome) {
    out.clear();
    outcome = SearchOutcome::Success();

    if (impl_->code == nullptr || !impl_->lastMatched) {
        outcome = SearchOutcome::Failure(SearchError::InternalError,
                                         L"没有可用于展开的匹配结果。");
        return false;
    }

    std::string replacementBytes;
    if (!ConvertToDocBytes(replacement, impl_->options.utfMode, impl_->options.codePage,
                           replacementBytes)) {
        outcome = SearchOutcome::Failure(SearchError::EncodingFailed,
                                         L"替换内容无法按当前文档编码表示。");
        return false;
    }

    // MATCHED：用 match_data 里已有的匹配，展开的是"这一个"而不是重新搜出来的第一个。
    // REPLACEMENT_ONLY：只要替换后的文本本身，而不是"整篇 subject 替换后的结果"——
    // 少了这一项就会把整篇文档当成替换串返回去（曾经错得很离谱）。
    const std::uint32_t baseFlags =
        PCRE2_SUBSTITUTE_MATCHED | PCRE2_SUBSTITUTE_REPLACEMENT_ONLY;
    const std::uint32_t flags = baseFlags | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;

    std::vector<char> buffer(replacementBytes.size() + subject.size() + 64);
    PCRE2_SIZE outLength = buffer.size();
    int rc = pcre2_substitute(impl_->code,
                              reinterpret_cast<PCRE2_SPTR>(subject.data()),
                              static_cast<PCRE2_SIZE>(subject.size()),
                              impl_->lastStartOffset, flags,
                              impl_->matchData, impl_->matchContext,
                              reinterpret_cast<PCRE2_SPTR>(replacementBytes.data()),
                              static_cast<PCRE2_SIZE>(replacementBytes.size()),
                              reinterpret_cast<PCRE2_UCHAR*>(buffer.data()), &outLength);
    if (rc == PCRE2_ERROR_NOMEMORY) {
        buffer.resize(outLength + 1);
        outLength = buffer.size();
        rc = pcre2_substitute(impl_->code,
                              reinterpret_cast<PCRE2_SPTR>(subject.data()),
                              static_cast<PCRE2_SIZE>(subject.size()),
                              impl_->lastStartOffset, baseFlags,
                              impl_->matchData, impl_->matchContext,
                              reinterpret_cast<PCRE2_SPTR>(replacementBytes.data()),
                              static_cast<PCRE2_SIZE>(replacementBytes.size()),
                              reinterpret_cast<PCRE2_UCHAR*>(buffer.data()), &outLength);
    }
    if (rc < 0) {
        std::wstring detail;
        PCRE2_UCHAR message[256] = {};
        if (pcre2_get_error_message(rc, message, sizeof(message)) > 0) {
            detail = ToWideUtf8(std::string_view(reinterpret_cast<const char*>(message)));
        }
        outcome = SearchOutcome::Failure(SearchError::BadPattern,
                                         L"替换内容有误" +
                                             (detail.empty() ? std::wstring() : L"：" + detail));
        return false;
    }
    out.assign(buffer.data(), outLength);
    return true;
}

}  // namespace npcc::search
