# -*- coding: utf-8 -*-
"""B14 兼容性与性能回归：冷启动、内存、50 MB 打开/查找/保存、T3 大文件。

脚本把被测程序复制到一个干净的临时目录，以 -portable 启动，避免测试配置与
开发目录互相污染。窗口驱动复用 ui_smoke_test.py，计时从 Popen 到主窗口
可见且 UI 线程能及时回话为止。
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ui_smoke_test as ui

MAIN_CLASS = "notepadccMainWindow"
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL
psapi.GetProcessMemoryInfo.restype = wintypes.BOOL


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


psapi.GetProcessMemoryInfo.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX),
    wintypes.DWORD,
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def process_memory(pid: int) -> dict[str, float]:
    access = PROCESS_QUERY_LIMITED_INFORMATION
    handle = kernel32.OpenProcess(access, False, pid)
    if not handle:
        access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
        handle = kernel32.OpenProcess(access, False, pid)
    if not handle:
        raise RuntimeError(f"OpenProcess failed for pid {pid}: {ctypes.get_last_error()}")
    try:
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), counters.cb):
            raise RuntimeError(
                f"GetProcessMemoryInfo failed: {ctypes.get_last_error()}")
        scale = 1024.0 * 1024.0
        return {
            "workingSetMiB": counters.WorkingSetSize / scale,
            "privateMiB": counters.PrivateUsage / scale,
            "peakWorkingSetMiB": counters.PeakWorkingSetSize / scale,
        }
    finally:
        kernel32.CloseHandle(handle)


def sample_memory(pid: int, samples: int = 5, delay: float = 0.2) -> dict[str, float]:
    values: list[dict[str, float]] = []
    for index in range(samples):
        values.append(process_memory(pid))
        if index + 1 < samples:
            time.sleep(delay)
    return {
        key: statistics.median(item[key] for item in values)
        for key in values[0]
    }


def clean_portable_config(work: Path) -> None:
    config = work / "config"
    if config.exists():
        shutil.rmtree(config, ignore_errors=True)


def close_app(proc: subprocess.Popen, window: int | None, grace_s: float = 8.0) -> None:
    if window and ui.user32.IsWindow(window):
        ui.user32.PostMessageW(window, ui.WM_CLOSE, 0, 0)
    try:
        proc.wait(timeout=grace_s)
        return
    except subprocess.TimeoutExpired:
        pass
    if window and ui.user32.IsWindow(window):
        ui.user32.PostMessageW(window, ui.WM_CLOSE, 0, 0)
    try:
        proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5.0)


def wait_any_main_window(timeout: float = 30.0) -> int | None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        window = ui.user32.FindWindowW(MAIN_CLASS, None)
        if window and ui.user32.IsWindowVisible(window):
            if ui.send_bounded(window, ui.WM_NULL, timeout_ms=500) is not None:
                return window
        time.sleep(0.05)
    return None


def launch(work: Path, exe: Path, args: list[str], title: str | None,
           timeout: float = 30.0) -> tuple[subprocess.Popen, int | None, float]:
    started = time.perf_counter()
    proc = subprocess.Popen([str(exe), ui.MULTI_INSTANCE, "-portable", *args],
                            cwd=str(work))
    window = (ui.wait_for_main_window(title, timeout=timeout) if title
              else wait_any_main_window(timeout))
    return proc, window, time.perf_counter() - started


def make_medium_documents(data: Path, count: int = 10) -> list[Path]:
    data.mkdir(parents=True, exist_ok=True)
    unit = (b"medium document line 0123456789abcdef\n" * 256)
    result = []
    for index in range(1, count + 1):
        path = data / f"doc{index:02d}.txt"
        with path.open("wb") as handle:
            for _ in range(32):
                handle.write(unit)
        result.append(path)
    return result


def make_50mb_file(path: Path) -> tuple[int, int, str]:
    line_count = 4096
    block = b"".join(
        f"line {index:08d} ".encode("ascii").ljust(127, b".") + b"\n"
        for index in range(line_count)
    )
    target_size = 50 * 1024 * 1024
    if len(block) * 100 != target_size:
        raise RuntimeError("50 MB fixture block size is inconsistent")
    marker = "NPCC_PERF_NEEDLE_2026_10_17"
    marker_offset = target_size - 4096
    with path.open("wb") as handle:
        for _ in range(100):
            handle.write(block)
        handle.seek(marker_offset)
        handle.write(marker.encode("ascii"))
    return target_size, marker_offset, marker


def make_tier3_file(path: Path) -> tuple[int, int]:
    line_bytes = 893
    line_count = 80000
    block = bytearray()
    for number in range(1, 1001):
        block += ("LINE %06d " % number).encode("ascii")
        block += b"x" * 880
        block += b"\n"
    with path.open("wb") as handle:
        for _ in range(line_count // 1000):
            handle.write(block)
    size = path.stat().st_size
    if size != line_bytes * line_count:
        raise RuntimeError(f"T3 fixture size mismatch: {size}")
    return size, line_count + 1


def wait_status_contains(window: int, needle: str, timeout: float) -> tuple[str, float]:
    started = time.perf_counter()
    last = ""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        last = ui.read_status_bar(window) or ""
        if needle in last:
            return last, time.perf_counter() - started
        time.sleep(0.1)
    return last, time.perf_counter() - started


def run_cold_start(work: Path, exe: Path, runs: int) -> tuple[list[float], dict[str, float]]:
    durations = []
    idle_memory: dict[str, float] = {}
    for _ in range(runs):
        clean_portable_config(work)
        proc, window, elapsed = launch(
            work, exe, [], "notepadcc - 新建 1", timeout=15.0)
        try:
            if not window:
                raise RuntimeError("冷启动未等到主窗口")
            durations.append(elapsed)
            if not idle_memory:
                time.sleep(1.0)
                idle_memory = sample_memory(proc.pid)
        finally:
            close_app(proc, window)
    clean_portable_config(work)
    return durations, idle_memory


def run_ten_documents(work: Path, exe: Path, data: Path) -> tuple[dict[str, float], int]:
    docs = make_medium_documents(data)
    clean_portable_config(work)
    proc, window, _ = launch(
        work, exe, [str(path) for path in docs], None, timeout=30.0)
    try:
        if not window:
            raise RuntimeError("10 文档场景未等到主窗口")
        time.sleep(1.2)
        memory = sample_memory(proc.pid)
    finally:
        close_app(proc, window)
    session = work / "config" / "session.xml"
    document_count = 0
    if session.exists():
        document_count = len(re.findall(r"<Doc\b", session.read_text(
            encoding="utf-8", errors="replace")))
    clean_portable_config(work)
    return memory, document_count


def run_50mb(work: Path, exe: Path, data: Path) -> dict[str, float]:
    path = data / "perf50.txt"
    size, marker_offset, marker = make_50mb_file(path)
    clean_portable_config(work)
    proc, window, open_seconds = launch(
        work, exe, [str(path)], "notepadcc - perf50.txt", timeout=60.0)
    result: dict[str, float] = {"openSeconds": open_seconds}
    try:
        if not window:
            raise RuntimeError("50 MB 文件未等到主窗口")
        scinti = ui.visible_scintilla(window)
        if not scinti:
            raise RuntimeError("50 MB 文件没有可用的 Scintilla 编辑控件")
        length = ui.user32.SendMessageW(scinti, ui.SCI_GETLENGTH, 0, 0)
        if length != size:
            raise RuntimeError(f"50 MB 装载长度不符: {length} != {size}")
        time.sleep(1.0)
        result.update({"workingSetMiB": sample_memory(proc.pid)["workingSetMiB"]})

        ui.user32.SendMessageW(window, ui.WM_COMMAND, ui.CMD_FIND, 0)
        time.sleep(0.15)
        find_bar = ui.find_child(window, "notepadccFindBar")
        if not find_bar:
            raise RuntimeError("查找栏没有创建")
        if not ui.set_text(find_bar, ui.CTRL_FIND_EDIT, marker):
            raise RuntimeError("无法填写查找词")
        time.sleep(0.1)
        started = time.perf_counter()
        completed = ui.send_bounded(
            window, ui.WM_COMMAND, ui.BTN_FIND_NEXT, 0, timeout_ms=15000)
        result["findSeconds"] = time.perf_counter() - started
        if completed is None:
            raise RuntimeError("50 MB 查找超过 15 秒仍未回话")
        selection_start = ui.user32.SendMessageW(
            scinti, ui.SCI_GETSELECTIONSTART, 0, 0)
        selection_end = ui.user32.SendMessageW(
            scinti, ui.SCI_GETSELECTIONEND, 0, 0)
        if selection_start != marker_offset or selection_end != marker_offset + len(marker):
            raise RuntimeError(
                f"50 MB 查找选区不符: {selection_start}..{selection_end}，"
                f"期望 {marker_offset}..{marker_offset + len(marker)}")

        ui.user32.SendMessageW(scinti, ui.SCI_GOTOPOS, size, 0)
        ui.user32.PostMessageW(scinti, 0x0102, ord("X"), 1)
        deadline = time.monotonic() + 5.0
        while (time.monotonic() < deadline and
               ui.user32.SendMessageW(scinti, ui.SCI_GETLENGTH, 0, 0) != size + 1):
            time.sleep(0.05)
        if ui.user32.SendMessageW(scinti, ui.SCI_GETLENGTH, 0, 0) != size + 1:
            raise RuntimeError("无法向 50 MB 文档追加一个字符")
        started = time.perf_counter()
        saved = ui.send_bounded(
            window, ui.WM_COMMAND, ui.CMD_FILE_SAVE, 0, timeout_ms=30000)
        result["saveSeconds"] = time.perf_counter() - started
        if saved is None:
            raise RuntimeError("50 MB 保存超过 30 秒仍未回话")
        deadline = time.monotonic() + 10.0
        while (time.monotonic() < deadline and
               path.stat().st_size != size + 1):
            time.sleep(0.05)
        if path.stat().st_size != size + 1:
            raise RuntimeError(
                f"50 MB 保存后文件大小不符: {path.stat().st_size} != {size + 1}")
        if ui.user32.SendMessageW(scinti, ui.SCI_GETMODIFY, 0, 0) != 0:
            raise RuntimeError("50 MB 保存后文档仍处于已修改状态")
        return result
    finally:
        close_app(proc, window)
        clean_portable_config(work)


def run_tier3(work: Path, exe: Path, data: Path) -> dict[str, float]:
    path = data / "huge.log"
    size, total_lines = make_tier3_file(path)
    clean_portable_config(work)
    proc, window, window_seconds = launch(
        work, exe, [str(path)], "notepadcc - huge.log", timeout=90.0)
    result: dict[str, float] = {"windowSeconds": window_seconds}
    try:
        if not window:
            raise RuntimeError("T3 大文件未等到主窗口")
        status, ready_seconds = wait_status_contains(
            window, f"共 {total_lines} 行", timeout=45.0)
        result["readySeconds"] = window_seconds + ready_seconds
        if "只读大文件视图" not in status or f"共 {total_lines} 行" not in status:
            raise RuntimeError(f"T3 状态栏未就绪: {status!r}")
        view = ui.find_child(window, ui.TEXTLOG_VIEW_CLASS)
        if not view or not ui.user32.IsWindowVisible(view) or not ui.window_text(view):
            raise RuntimeError("T3 自绘视口不可见或没有文字")
        if ui.visible_scintilla(window) is not None:
            raise RuntimeError("T3 档不应有可见的 Scintilla")
        response = ui.wait_until_responsive(window, timeout=10.0)
        if response is None:
            raise RuntimeError("T3 视口在 10 秒内没有恢复响应")
        result["responsiveSeconds"] = response
        result["workingSetMiB"] = sample_memory(proc.pid)["workingSetMiB"]
        if path.stat().st_size != size:
            raise RuntimeError("T3 文件在只读打开期间被改动")
        return result
    finally:
        close_app(proc, window)
        clean_portable_config(work)


def system_dpi() -> int:
    try:
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.GetDpiForSystem.restype = wintypes.UINT
        return int(user32.GetDpiForSystem())
    except (AttributeError, OSError):
        dc = ui.user32.GetDC(0)
        try:
            return int(ui.gdi32.GetDeviceCaps(dc, 88))
        finally:
            ui.user32.ReleaseDC(0, dc)


def make_checks(metrics: dict[str, object]) -> list[dict[str, object]]:
    cold = metrics["coldStart"]
    open50 = metrics["file50mb"]
    memory10 = metrics["tenDocuments"]
    checks = [
        {"name": "冷启动中位数", "actual": cold["medianSeconds"],
         "limit": 1.0, "unit": "s", "op": "<=",
         "pass": cold["medianSeconds"] <= 1.0},
        {"name": "空闲常驻内存", "actual": cold["idle"]["workingSetMiB"],
         "limit": 40.0, "unit": "MiB", "op": "<=",
         "pass": cold["idle"]["workingSetMiB"] <= 40.0},
        {"name": "10 个中等文档内存", "actual": memory10["memory"]["workingSetMiB"],
         "limit": 40.0, "unit": "MiB", "op": "<=",
         "pass": memory10["memory"]["workingSetMiB"] <= 40.0},
        {"name": "50 MB 打开到可交互", "actual": open50["openSeconds"],
         "limit": 2.0, "unit": "s", "op": "<=",
         "pass": open50["openSeconds"] <= 2.0},
        {"name": "50 MB 首个字面量匹配", "actual": open50["findSeconds"],
         "limit": 0.3, "unit": "s", "op": "<=",
         "pass": open50["findSeconds"] <= 0.3},
        {"name": "10 文档确实装入", "actual": memory10["documentCount"],
         "limit": 10, "unit": "个", "op": "==",
         "pass": memory10["documentCount"] == 10},
        {"name": "50 MB 保存完成", "actual": open50["saveSeconds"],
         "limit": 30.0, "unit": "s", "op": "<=",
         "pass": open50["saveSeconds"] <= 30.0},
    ]
    return checks


def render_markdown(data: dict[str, object]) -> str:
    metrics = data["metrics"]
    cold = metrics["coldStart"]
    open50 = metrics["file50mb"]
    memory10 = metrics["tenDocuments"]
    tier3 = metrics["tier3"]
    checks = data["checks"]
    compatible = data["compatibility"]
    lines = [
        "# notepadcc B14 兼容性与性能回归报告",
        "",
        f"- 运行时间：{data['generatedAt']}",
        f"- 构建目录：`{data['buildDir']}`",
        f"- 程序 SHA-256：`{data['exeSha256']}`",
        f"- 当前系统：{compatible['os']}，架构 {compatible['architecture']}，"
        f"系统 DPI {compatible['dpi']}",
        f"- 结论：{'PASS' if all(item['pass'] for item in checks) else 'FAIL'}",
        "",
        "## 硬指标",
        "",
        "| 项目 | 实测 | 预算 | 结果 |",
        "|---|---:|---:|---|",
    ]
    for item in checks:
        lines.append(
            f"| {item['name']} | {item['actual']:.3f} {item['unit']} | "
            f"{item['op']} {item['limit']:.3f} {item['unit']} | "
            f"{'PASS' if item['pass'] else 'FAIL'} |")
    lines += [
        "",
        "## 观测指标",
        "",
        "| 项目 | 数值 |",
        "|---|---:|",
        f"| 冷启动样本 | {', '.join(f'{value:.3f} s' for value in cold['samplesSeconds'])} |",
        f"| 冷启动中位数 / 最大值 | {cold['medianSeconds']:.3f} s / {cold['maxSeconds']:.3f} s |",
        f"| 空闲工作集 / 私有提交 | {cold['idle']['workingSetMiB']:.2f} / "
        f"{cold['idle']['privateMiB']:.2f} MiB |",
        f"| 10 文档工作集 / 私有提交 | {memory10['memory']['workingSetMiB']:.2f} / "
        f"{memory10['memory']['privateMiB']:.2f} MiB |",
        f"| 50 MB 工作集 | {open50['workingSetMiB']:.2f} MiB |",
        f"| 50 MB 保存 | {open50['saveSeconds']:.3f} s |",
        f"| T3 大文件窗口出现 | {tier3['windowSeconds']:.3f} s |",
        f"| T3 大文件索引/视口就绪 | {tier3['readySeconds']:.3f} s |",
        f"| T3 大文件工作集 | {tier3['workingSetMiB']:.2f} MiB |",
        "",
        "## 兼容性证据",
        "",
        f"- 当前系统：{compatible['os']}，Windows 版本号 {compatible['version']}，"
        f"满足最低 build 17763 的版本下限检查："
        f"{'是' if compatible['meetsRuntimeFloor'] else '否'}。",
        f"- Win10 1809：{compatible['win10_1809']}",
        f"- 多缩放矩阵：{compatible['scaling']}",
        f"- 深色 / 护眼 / 当前 DPI：{compatible['uiRegression']}",
        "",
        "## 说明",
        "",
        "- 计时包含进程创建、窗口创建与首次装载；同一台机器上的后台负载会影响结果。",
        "- 冷启动不主动清空 OS 文件缓存：首样本可能偏高，门槛按 3 次中位数判定，全部样本见上表。",
        "- 本脚本只验证运行期兼容性与性能，不替代 MSI 安装、代码签名和真实旧系统虚机回归。",
        "- 50 MB 查找的计时从发出“查找下一个”到 UI 线程处理完毕，并核对命中偏移与文件内容一致。",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="B14 compatibility and performance regression")
    parser.add_argument("--repo-root", default=str(HERE.parent))
    parser.add_argument("--build-dir", default="build-msvc-final")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--skip-tier3", action="store_true")
    parser.add_argument("--keep-work", action="store_true")
    parser.add_argument("--json-out", default="")
    parser.add_argument("--markdown-out", default="")
    parser.add_argument("--ui-regression-evidence",
                        default="未提供；深色 / 护眼 / 缩放由 ui_smoke_test.py 另行覆盖")
    parser.add_argument("--win10-1809-evidence",
                        default="未实测：本机没有 Win10 1809 主机或现成虚拟机")
    parser.add_argument("--scaling-evidence",
                        default="仅实测当前系统 DPI；100%/125%/150%/200% 非本机矩阵")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = root / build_dir
    source_exe = build_dir / "bin" / "notepadcc.exe"
    if not source_exe.is_file():
        print(f"FAIL: 找不到 {source_exe}", file=sys.stderr)
        return 2
    if ui.main_windows():
        print("FAIL: 已有 notepadcc 窗口在运行；请先关闭后重试。", file=sys.stderr)
        return 2

    work = root / "build" / "perf_compat"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    exe = work / "notepadcc.exe"
    shutil.copy2(source_exe, exe)
    data = work / "data"
    data.mkdir()

    try:
        cold_samples, idle = run_cold_start(work, exe, max(1, args.runs))
        memory10, document_count = run_ten_documents(work, exe, data)
        file50 = run_50mb(work, exe, data)
        tier3 = ({"windowSeconds": 0.0, "readySeconds": 0.0,
                  "responsiveSeconds": 0.0, "workingSetMiB": 0.0, "skipped": True}
                 if args.skip_tier3 else run_tier3(work, exe, data))

        windows_version = sys.getwindowsversion()
        metrics = {
            "coldStart": {
                "samplesSeconds": cold_samples,
                "medianSeconds": statistics.median(cold_samples),
                "maxSeconds": max(cold_samples),
                "idle": idle,
            },
            "tenDocuments": {"memory": memory10, "documentCount": document_count},
            "file50mb": file50,
            "tier3": tier3,
        }
        checks = make_checks(metrics)
        generated = datetime.now().astimezone()
        data_report = {
            "schema": "notepadcc-b14-performance-v1",
            "generatedAt": generated.isoformat(timespec="seconds"),
            "buildDir": str(build_dir),
            "exeSha256": sha256_file(source_exe),
            "metrics": metrics,
            "checks": checks,
            "compatibility": {
                "os": platform.platform(),
                "version": f"{windows_version.major}.{windows_version.minor} build "
                           f"{windows_version.build}",
                "architecture": platform.machine(),
                "dpi": system_dpi(),
                "meetsRuntimeFloor": windows_version.build >= 17763,
                "win10_1809": args.win10_1809_evidence,
                "scaling": args.scaling_evidence,
                "uiRegression": args.ui_regression_evidence,
            },
        }
        markdown = render_markdown(data_report)
        if args.json_out:
            output = Path(args.json_out)
            if not output.is_absolute():
                output = root / output
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(data_report, ensure_ascii=False, indent=2) + "\n",
                              encoding="utf-8")
        if args.markdown_out:
            output = Path(args.markdown_out)
            if not output.is_absolute():
                output = root / output
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(markdown + "\n", encoding="utf-8")
        print(markdown)
        return 0 if all(item["pass"] for item in checks) else 1
    finally:
        if not args.keep_work:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
