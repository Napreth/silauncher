#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

enum class Action {
    Minimize,
    Close,
};

struct Options {
    Action action = Action::Minimize;
    std::wstring executable;
    std::vector<std::wstring> arguments;
    std::optional<std::wstring> working_directory;
    DWORD timeout_ms = 30000;
    DWORD settle_ms = 250;
    DWORD grace_ms = 1500;
    bool quiet = false;
};

enum class Phase {
    Suppressing,
    Applied,
};

struct RuntimeContext {
    DWORD root_pid = 0;
    Action action = Action::Minimize;
    Phase phase = Phase::Suppressing;
    HWND candidate = nullptr;
    Clock::time_point candidate_since{};
    Clock::time_point applied_at{};
    DWORD settle_ms = 250;
    DWORD grace_ms = 1500;
};

RuntimeContext* g_context = nullptr;

void ShowError(const std::wstring& message, bool quiet) {
    if (!quiet) {
        MessageBoxW(nullptr, message.c_str(), L"silauncher", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}

std::wstring Win32ErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);

    if (length == 0 || buffer == nullptr) {
        return L"Win32 error " + std::to_wstring(error);
    }

    std::wstring result(buffer, length);
    LocalFree(buffer);

    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

std::optional<DWORD> ParseDword(std::wstring_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    unsigned long long value = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<unsigned long long>(ch - L'0');
        if (value > 0xFFFFFFFFull) {
            return std::nullopt;
        }
    }

    return static_cast<DWORD>(value);
}

std::wstring QuoteArgument(std::wstring_view arg) {
    if (arg.empty()) {
        return L"\"\"";
    }

    const bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) {
        return std::wstring(arg);
    }

    std::wstring out;
    out.push_back(L'\"');

    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            backslashes = 0;
            continue;
        }

        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }

    // Backslashes immediately before the closing quote must be doubled.
    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

std::wstring BuildCommandLine(const Options& options) {
    std::wstring command = QuoteArgument(options.executable);
    for (const auto& arg : options.arguments) {
        command.push_back(L' ');
        command += QuoteArgument(arg);
    }
    return command;
}

std::wstring UsageText() {
    return
        L"Silent Launcher (silauncher)\n\n"
        L"Usage:\n"
        L"  silauncher.exe [options] <program> [program arguments...]\n\n"
        L"Options:\n"
        L"  --minimize           Hide startup UI, then leave the main window minimized (default)\n"
        L"  --close              Hide startup UI, then send WM_CLOSE to the main window\n"
        L"  --cwd <path>         Working directory for the launched program\n"
        L"  --timeout-ms <n>     Stop waiting for a window after n ms (default: 30000)\n"
        L"  --settle-ms <n>      Keep the first window hidden for n ms before acting (default: 250)\n"
        L"  --grace-ms <n>       Keep enforcing the action for n ms afterward (default: 1500)\n"
        L"  --quiet              Do not show error message boxes\n"
        L"  --help               Show this help\n\n"
        L"Examples:\n"
        L"  silauncher.exe \"C:\\Program Files (x86)\\eM Client\\MailClient.exe\"\n"
        L"  silauncher.exe --close \"C:\\Program Files\\Feishu\\Feishu.exe\"\n";
}

struct ParseResult {
    bool ok = false;
    bool show_help = false;
    Options options;
    std::wstring error;
};

ParseResult ParseCommandLine() {
    ParseResult result;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        result.error = L"Unable to parse the command line.";
        return result;
    }

    auto cleanup = [&]() { LocalFree(argv); };

    bool target_seen = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg(argv[i]);

        if (target_seen) {
            result.options.arguments.emplace_back(arg);
            continue;
        }

        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            result.show_help = true;
            cleanup();
            return result;
        }
        if (arg == L"--quiet") {
            result.options.quiet = true;
            continue;
        }
        if (arg == L"--minimize") {
            result.options.action = Action::Minimize;
            continue;
        }
        if (arg == L"--close") {
            result.options.action = Action::Close;
            continue;
        }

        auto read_value = [&](std::wstring_view option_name) -> std::optional<std::wstring> {
            if (i + 1 >= argc) {
                result.error = L"Missing value for " + std::wstring(option_name) + L".";
                return std::nullopt;
            }
            ++i;
            return std::wstring(argv[i]);
        };

        if (arg == L"--cwd") {
            auto value = read_value(arg);
            if (!value) {
                cleanup();
                return result;
            }
            result.options.working_directory = std::move(*value);
            continue;
        }

        auto parse_ms_option = [&](DWORD& destination) -> bool {
            auto value = read_value(arg);
            if (!value) {
                return false;
            }
            auto parsed = ParseDword(*value);
            if (!parsed) {
                result.error = L"Invalid numeric value for " + std::wstring(arg) + L".";
                return false;
            }
            destination = *parsed;
            return true;
        };

        if (arg == L"--timeout-ms") {
            if (!parse_ms_option(result.options.timeout_ms)) {
                cleanup();
                return result;
            }
            continue;
        }
        if (arg == L"--settle-ms") {
            if (!parse_ms_option(result.options.settle_ms)) {
                cleanup();
                return result;
            }
            continue;
        }
        if (arg == L"--grace-ms") {
            if (!parse_ms_option(result.options.grace_ms)) {
                cleanup();
                return result;
            }
            continue;
        }

        if (!arg.empty() && arg.front() == L'-') {
            result.error = L"Unknown silauncher option: " + std::wstring(arg) +
                           L"\n\nPlace the program path first; all following arguments are passed to the program.";
            cleanup();
            return result;
        }

        result.options.executable = std::wstring(arg);
        target_seen = true;
    }

    cleanup();

    if (!target_seen || result.options.executable.empty()) {
        result.error = L"No program was specified.\n\n" + UsageText();
        return result;
    }

    result.ok = true;
    return result;
}

std::unordered_set<DWORD> CollectProcessTree(DWORD root_pid) {
    std::unordered_set<DWORD> pids;
    pids.insert(root_pid);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return pids;
    }

    std::vector<PROCESSENTRY32W> entries;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            entries.push_back(entry);
            entry.dwSize = sizeof(entry);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& process : entries) {
            if (pids.contains(process.th32ParentProcessID) && !pids.contains(process.th32ProcessID)) {
                pids.insert(process.th32ProcessID);
                changed = true;
            }
        }
    }

    return pids;
}

bool IsProcessInTree(DWORD pid, DWORD root_pid) {
    if (pid == root_pid) {
        return true;
    }
    const auto tree = CollectProcessTree(root_pid);
    return tree.contains(pid);
}

bool IsCandidateWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if ((style & WS_CHILD) != 0) {
        return false;
    }

    // Tool windows are usually splash/helper windows. Keep APPWINDOW windows eligible.
    if ((ex_style & WS_EX_TOOLWINDOW) != 0 && (ex_style & WS_EX_APPWINDOW) == 0) {
        return false;
    }

    const HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != nullptr && (ex_style & WS_EX_APPWINDOW) == 0) {
        return false;
    }

    // A real application window normally has at least one of these traits.
    const bool application_like =
        (style & WS_CAPTION) != 0 ||
        (style & WS_THICKFRAME) != 0 ||
        (ex_style & WS_EX_APPWINDOW) != 0;

    return application_like;
}

void HideWindow(HWND hwnd) {
    ShowWindowAsync(hwnd, SW_HIDE);
}

void MinimizeWindow(HWND hwnd) {
    // SW_SHOWMINNOACTIVE also turns an initially hidden window into a minimized taskbar entry
    // without activating it.
    ShowWindowAsync(hwnd, SW_SHOWMINNOACTIVE);
}

void CloseTargetWindow(HWND hwnd) {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(
        hwnd,
        WM_CLOSE,
        0,
        0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        1000,
        &ignored);
}

void ApplyAction(HWND hwnd, Action action) {
    if (!IsWindow(hwnd)) {
        return;
    }

    if (action == Action::Minimize) {
        MinimizeWindow(hwnd);
    } else {
        HideWindow(hwnd);
        CloseTargetWindow(hwnd);
    }
}

void ConsiderWindow(RuntimeContext& context, HWND hwnd, DWORD pid) {
    if (!IsProcessInTree(pid, context.root_pid) || !IsCandidateWindow(hwnd)) {
        return;
    }

    if (context.phase == Phase::Suppressing) {
        HideWindow(hwnd);
        if (context.candidate == nullptr || !IsWindow(context.candidate)) {
            context.candidate = hwnd;
            context.candidate_since = Clock::now();
        }
    } else {
        // During the short grace period, stop apps from immediately undoing the requested state.
        ApplyAction(hwnd, context.action);
    }
}

void CALLBACK WinEventCallback(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG id_object,
    LONG id_child,
    DWORD event_thread,
    DWORD event_time) {

    (void)event_thread;
    (void)event_time;

    if (!g_context || event != EVENT_OBJECT_SHOW || hwnd == nullptr ||
        id_object != OBJID_WINDOW || id_child != CHILDID_SELF) {
        return;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        ConsiderWindow(*g_context, hwnd, pid);
    }
}

struct EnumData {
    RuntimeContext* context = nullptr;
    const std::unordered_set<DWORD>* pids = nullptr;
};

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lparam) {
    auto* data = reinterpret_cast<EnumData*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    if (pid == 0 || !data->pids->contains(pid) || !IsCandidateWindow(hwnd)) {
        return TRUE;
    }

    auto& context = *data->context;

    // EnumWindows is only a race-condition fallback. Do not treat an application's
    // intentionally hidden helper windows as its main window. A window already
    // captured by EVENT_OBJECT_SHOW remains eligible after we hide it.
    if (!IsWindowVisible(hwnd) && hwnd != context.candidate) {
        return TRUE;
    }

    if (context.phase == Phase::Suppressing) {
        HideWindow(hwnd);
        if (context.candidate == nullptr || !IsWindow(context.candidate)) {
            context.candidate = hwnd;
            context.candidate_since = Clock::now();
        }
    } else {
        ApplyAction(hwnd, context.action);
    }

    return TRUE;
}

void SweepWindows(RuntimeContext& context) {
    const auto pids = CollectProcessTree(context.root_pid);
    EnumData data{&context, &pids};
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&data));
}

void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool PathLooksUsable(const std::wstring& executable) {
    // Bare command names (e.g. "notepad.exe") may resolve via the normal Windows search path.
    if (executable.find(L'\\') == std::wstring::npos && executable.find(L'/') == std::wstring::npos) {
        return true;
    }

    std::error_code ec;
    return std::filesystem::exists(executable, ec);
}

int Run(const Options& options) {
    if (!PathLooksUsable(options.executable)) {
        ShowError(L"Program not found:\n" + options.executable, options.quiet);
        return 2;
    }

    std::wstring command_line = BuildCommandLine(options);
    // CreateProcess requires a mutable command-line buffer.
    std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
    command_buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};

    const wchar_t* cwd = options.working_directory ? options.working_directory->c_str() : nullptr;

    if (!CreateProcessW(
            options.executable.c_str(),
            command_buffer.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED,
            nullptr,
            cwd,
            &startup,
            &process)) {
        const DWORD error = GetLastError();
        ShowError(
            L"Failed to launch:\n" + options.executable + L"\n\n" + Win32ErrorMessage(error),
            options.quiet);
        return 3;
    }

    RuntimeContext context{};
    context.root_pid = process.dwProcessId;
    context.action = options.action;
    context.settle_ms = options.settle_ms;
    context.grace_ms = options.grace_ms;
    g_context = &context;

    HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_OBJECT_SHOW,
        EVENT_OBJECT_SHOW,
        nullptr,
        WinEventCallback,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!hook) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        g_context = nullptr;
        ShowError(L"Unable to install the window event hook.\n\n" + Win32ErrorMessage(error), options.quiet);
        return 4;
    }

    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        UnhookWinEvent(hook);
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        g_context = nullptr;
        ShowError(L"Unable to resume the launched process.\n\n" + Win32ErrorMessage(error), options.quiet);
        return 5;
    }

    CloseHandle(process.hThread);

    const auto started_at = Clock::now();
    auto last_sweep = Clock::time_point{};

    int exit_code = 0;
    for (;;) {
        PumpMessages();
        const auto now = Clock::now();

        if (last_sweep == Clock::time_point{} || now - last_sweep >= 40ms) {
            SweepWindows(context);
            last_sweep = now;
        }

        if (context.phase == Phase::Suppressing && context.candidate != nullptr) {
            if (!IsWindow(context.candidate)) {
                context.candidate = nullptr;
            } else if (now - context.candidate_since >= std::chrono::milliseconds(context.settle_ms)) {
                ApplyAction(context.candidate, context.action);
                context.phase = Phase::Applied;
                context.applied_at = now;
            }
        }

        if (context.phase == Phase::Applied &&
            now - context.applied_at >= std::chrono::milliseconds(context.grace_ms)) {
            break;
        }

        if (now - started_at >= std::chrono::milliseconds(options.timeout_ms)) {
            // Timing out is not a launch failure: some apps intentionally create no top-level window.
            exit_code = 0;
            break;
        }

        // Wait a little while still allowing WinEvent callbacks/messages to be delivered promptly.
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    UnhookWinEvent(hook);
    g_context = nullptr;
    CloseHandle(process.hProcess);
    return exit_code;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const ParseResult parsed = ParseCommandLine();

    if (parsed.show_help) {
        MessageBoxW(nullptr, UsageText().c_str(), L"silauncher", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (!parsed.ok) {
        ShowError(parsed.error, parsed.options.quiet);
        return 1;
    }

    return Run(parsed.options);
}
