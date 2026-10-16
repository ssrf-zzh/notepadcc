#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <windows.h>

#include "core/ScriptEngine.h"

namespace npcc::infra {

// 探测脚本解释器：按 core::ScriptInterpreterCandidates 的顺序找——
// 配置里写了路径就只认那个文件；否则在 PATH 里找 python.exe / py.exe（找到的写回完整路径）。
// 找不到时返回 false，error 里是给用户看的说明（去哪儿改配置、PATH 是怎么回事）。
bool ResolveScriptInterpreter(std::wstring_view configured, core::ScriptInterpreter* out,
                              std::wstring* error);

// 脚本执行器（M3 第十二块）：把"起进程 + 用临时文件喂 stdin + 收 stdout/stderr +
// 超时杀"封成一个由调用方轮询的小状态机。
//
// 两条与运行命令保持一致的取舍：
//  1 等待发生在 UI 线程的定时器里（Exited / TimedOut / Kill / Finish），
//    没有跨线程的所有权问题；
//  2 子进程的标准流走**临时文件**而不是管道——管道要在两个方向同时读写才不死锁，
//    用文件没有这个问题（输出先落盘，收尾时一次性读回来）。
class ScriptRunner {
public:
    struct Options {
        std::wstring  commandLine;   // 完整命令行（含引号），解释器 + 脚本
        std::wstring  workingDir;    // 空 = 用当前目录
        std::string   stdinBytes;    // 喂给脚本的输入（编辑器内部就是 UTF-8 字节）
        std::uint64_t timeoutMs = 60000;
    };
    struct Result {
        bool          started = false;
        std::wstring  startError;      // 启动失败的人话说明（已含系统错误描述）
        bool          killed = false;  // 超时被 Kill() 终止（退出码没有意义）
        unsigned long exitCode = 0;
        std::uint64_t elapsedMs = 0;
        bool          stdoutTooLarge = false;   // 输出超过上限，没有收回来
        bool          stderrTooLarge = false;
        std::string   stdoutBytes;
        std::string   stderrBytes;
    };

    ScriptRunner() = default;
    ~ScriptRunner();   // 还在跑就先终止：窗口都不在了，结果没有去处
    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    // 启动失败返回 false 并把原因写进 result（临时文件已经清理）。
    bool Start(const Options& options, Result* result);
    bool Running() const noexcept { return process_ != nullptr; }
    bool Exited() const;      // 进程已退出（退出但还没收尾也返回 true）
    bool TimedOut() const;    // 超过 timeoutMs 还没退出
    void Kill();              // 超时后调用：强制终止，结果里 killed=true
    // 收尾：读回输出、清理临时文件、关句柄。Exited() 或 Kill() 之后调用一次。
    Result Finish();

private:
    void RemoveTempFiles();

    HANDLE        process_ = nullptr;
    std::uint64_t startedTick_ = 0;
    std::uint64_t timeoutMs_ = 60000;
    bool          killed_ = false;
    std::wstring  inputPath_;
    std::wstring  outputPath_;
    std::wstring  errorPath_;
};

}  // namespace npcc::infra
