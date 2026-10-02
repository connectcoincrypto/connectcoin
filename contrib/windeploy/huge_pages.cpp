// Copyright (c) 2026-present The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Standalone, opt-in Windows account configuration. Build as WIN32 (/MT), with
// advapi32, shell32, user32 and ole32. No wallet/node code is linked or launched.
// The MSI may offer to launch "configure" in the original user's context. It
// must NOT call "enable" in an installation/repair/uninstall execution action.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ntsecapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr wchar_t RIGHT_NAME[]{L"SeLockMemoryPrivilege"};
constexpr wchar_t TITLE[]{L"ConnectCoin Core - Large Pages"};
constexpr wchar_t LOGON_NOTE[]{L"Sign out of Windows and sign in again after changing the right. Existing process tokens are not updated. Then start Core normally, not as administrator. Large-page allocation can still fail if insufficient suitable RAM is available."};
enum ExitCode : int { SUCCESS = 0, FAILURE = 1, USAGE = 2, CANCELLED = 3, ACCOUNT_MISMATCH = 4, NOT_ELEVATED = 5 };

struct Error {
    std::wstring message;
    int exit_code{FAILURE};
};

[[noreturn]] void Fail(std::wstring operation, DWORD code = GetLastError())
{
    wchar_t* buffer{nullptr};
    const DWORD length{FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr)};
    std::wstring detail{length && buffer ? std::wstring(buffer, length) : L"Windows error"};
    if (buffer) LocalFree(buffer);
    throw Error{std::move(operation) + L": " + detail + L" (" + std::to_wstring(code) + L")"};
}

struct Handle {
    HANDLE value{nullptr};
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct LocalBuffer {
    void* value{nullptr};
    ~LocalBuffer() { if (value) LocalFree(value); }
};

struct Policy {
    LSA_HANDLE value{nullptr};
    explicit Policy(ACCESS_MASK access)
    {
        LSA_OBJECT_ATTRIBUTES attributes{};
        attributes.Length = sizeof(attributes);
        // NULL system name limits all operations to the local computer.
        const NTSTATUS status{LsaOpenPolicy(nullptr, &attributes, access, &value)};
        if (status != 0) Fail(L"Cannot open the local account-rights policy", LsaNtStatusToWinError(status));
    }
    ~Policy() { if (value) LsaClose(value); }
    Policy(const Policy&) = delete;
    Policy& operator=(const Policy&) = delete;
};

struct RightsBuffer {
    PLSA_UNICODE_STRING value{nullptr};
    ~RightsBuffer() { if (value) LsaFreeMemory(value); }
};

void Output(const std::wstring& text, bool error = false)
{
    HANDLE destination{GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE)};
    if (!destination || destination == INVALID_HANDLE_VALUE) return;
    const std::wstring line{text + L"\r\n"};
    DWORD mode{0};
    DWORD written{0};
    if (GetConsoleMode(destination, &mode)) {
        WriteConsoleW(destination, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        return;
    }
    const int needed{WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr)};
    if (needed <= 0) return;
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), utf8.data(), needed, nullptr, nullptr)) return;
    // Preserve redirected stdout/stderr supplied by subprocess.run/capture_output.
    WriteFile(destination, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void AttachOutput()
{
    const HANDLE output{GetStdHandle(STD_OUTPUT_HANDLE)};
    if (!output || output == INVALID_HANDLE_VALUE) AttachConsole(ATTACH_PARENT_PROCESS);
}

std::vector<BYTE> TokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind)
{
    DWORD needed{0};
    const BOOL unexpected_success{GetTokenInformation(token, kind, nullptr, 0, &needed)};
    const DWORD error{GetLastError()};
    if (unexpected_success || error != ERROR_INSUFFICIENT_BUFFER || needed == 0 || needed > 1024 * 1024) {
        Fail(L"Cannot size token information", error);
    }
    std::vector<BYTE> buffer(needed);
    if (!GetTokenInformation(token, kind, buffer.data(), needed, &needed)) Fail(L"Cannot read token information");
    return buffer;
}

std::wstring SidString(PSID sid)
{
    LocalBuffer text;
    if (!IsValidSid(sid) || !ConvertSidToStringSidW(sid, reinterpret_cast<wchar_t**>(&text.value))) Fail(L"Cannot represent account SID");
    return static_cast<const wchar_t*>(text.value);
}

bool CanonicalSid(const std::wstring& sid)
{
    if (sid.empty() || sid.size() > 184) return false;
    LocalBuffer binary;
    if (!ConvertStringSidToSidW(sid.c_str(), &binary.value)) return false;
    return IsValidSid(binary.value) && SidString(binary.value) == sid;
}

struct Identity {
    std::vector<BYTE> sid;
    std::wstring sid_text;
    bool elevated{false};
    bool administrator{false};
    bool privilege_present{false};
    bool privilege_enabled{false};
    DWORD session{0};
    PSID Sid() const { return const_cast<BYTE*>(sid.data()); }
};

Identity CurrentIdentity()
{
    // Never allow an impersonated service/MSI thread to redirect the account.
    Handle thread;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread.value)) {
        throw Error{L"Run this helper normally as the intended Windows user, not from an impersonated service thread."};
    }
    if (GetLastError() != ERROR_NO_TOKEN) Fail(L"Cannot validate the current thread identity");

    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) Fail(L"Cannot open this process token");
    Identity result;
    const auto user{TokenInformation(token.value, TokenUser)};
    if (user.size() < sizeof(TOKEN_USER)) throw Error{L"Invalid token user response."};
    const PSID sid{reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid};
    if (!IsValidSid(sid)) throw Error{L"Invalid account SID in this process token."};
    result.sid.resize(GetLengthSid(sid));
    if (!CopySid(static_cast<DWORD>(result.sid.size()), result.sid.data(), sid)) Fail(L"Cannot copy this process account SID");
    result.sid_text = SidString(result.Sid());

    TOKEN_ELEVATION elevation{};
    DWORD returned{0};
    // Fixed-size query: TokenElevation can reject a zero-length size probe.
    if (!GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &returned)) Fail(L"Cannot inspect token elevation");
    if (returned != sizeof(elevation)) throw Error{L"Invalid token elevation response."};
    result.elevated = elevation.TokenIsElevated != 0;
    if (!GetTokenInformation(token.value, TokenSessionId, &result.session, sizeof(result.session), &returned)) Fail(L"Cannot inspect token session");
    if (returned != sizeof(result.session)) throw Error{L"Invalid token session response."};

    BYTE administrators[SECURITY_MAX_SID_SIZE]{};
    DWORD administrators_size{sizeof(administrators)};
    BOOL member{FALSE};
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &administrators_size)) Fail(L"Cannot identify the Administrators group");
    // NULL token checks the calling token; impersonation was rejected above.
    if (!CheckTokenMembership(nullptr, administrators, &member)) Fail(L"Cannot inspect administrator membership");
    result.administrator = member != FALSE;

    const auto privileges{TokenInformation(token.value, TokenPrivileges)};
    if (privileges.size() < offsetof(TOKEN_PRIVILEGES, Privileges)) throw Error{L"Invalid privilege response."};
    const auto* list{reinterpret_cast<const TOKEN_PRIVILEGES*>(privileges.data())};
    const auto capacity{(privileges.size() - offsetof(TOKEN_PRIVILEGES, Privileges)) / sizeof(LUID_AND_ATTRIBUTES)};
    if (list->PrivilegeCount > capacity) throw Error{L"Invalid privilege count."};
    LUID lock_memory{};
    if (!LookupPrivilegeValueW(nullptr, RIGHT_NAME, &lock_memory)) Fail(L"Cannot identify the large-pages privilege");
    for (DWORD i{0}; i < list->PrivilegeCount; ++i) {
        const auto& entry{list->Privileges[i]};
        if (entry.Luid.LowPart == lock_memory.LowPart && entry.Luid.HighPart == lock_memory.HighPart) {
            result.privilege_present = true;
            result.privilege_enabled = (entry.Attributes & SE_PRIVILEGE_ENABLED) != 0;
        }
    }
    return result;
}

using Rights = std::vector<std::wstring>;

bool HasRight(const Rights& rights) { return std::find(rights.begin(), rights.end(), RIGHT_NAME) != rights.end(); }

bool PreservesRights(const Rights& before, const Rights& after)
{
    return std::all_of(before.begin(), before.end(), [&](const auto& right) { return std::find(after.begin(), after.end(), right) != after.end(); });
}

Rights ReadRights(const Identity& identity)
{
    Policy policy{POLICY_LOOKUP_NAMES};
    RightsBuffer buffer;
    ULONG count{0};
    const NTSTATUS status{LsaEnumerateAccountRights(policy.value, identity.Sid(), &buffer.value, &count)};
    // STATUS_OBJECT_NAME_NOT_FOUND: no direct rights assigned. Access denied is
    // NEVER interpreted as an empty rights list or as an absent privilege.
    if (static_cast<ULONG>(status) == 0xC0000034UL) return {};
    if (status != 0) Fail(L"Cannot read direct account rights", LsaNtStatusToWinError(status));
    if (count > 4096 || (count && !buffer.value)) throw Error{L"Invalid account-rights response."};
    Rights result;
    result.reserve(count);
    for (ULONG i{0}; i < count; ++i) {
        const auto& right{buffer.value[i]};
        if (!right.Buffer || right.Length % sizeof(wchar_t) || right.Length > right.MaximumLength) throw Error{L"Invalid account-right string."};
        result.emplace_back(right.Buffer, right.Length / sizeof(wchar_t));
    }
    std::sort(result.begin(), result.end());
    return result;
}

int GrantPrecondition(std::wstring_view current_sid, std::wstring_view expected_sid, bool elevated, bool administrator, bool interactive_user)
{
    if (current_sid != expected_sid) return ACCOUNT_MISMATCH;
    if (!elevated || !administrator || !interactive_user) return NOT_ELEVATED;
    return SUCCESS;
}

bool InteractiveUser(const Identity& identity)
{
    return identity.session != 0 && !IsWellKnownSid(identity.Sid(), WinLocalSystemSid) &&
           !IsWellKnownSid(identity.Sid(), WinLocalServiceSid) && !IsWellKnownSid(identity.Sid(), WinNetworkServiceSid);
}

void Grant(const Identity& identity, const std::wstring& expected_sid)
{
    const int precondition{GrantPrecondition(identity.sid_text, expected_sid, identity.elevated, identity.administrator, InteractiveUser(identity))};
    if (precondition == ACCOUNT_MISMATCH) {
        throw Error{L"UAC used a different Windows account. No right was changed. This helper only configures the same account that started it. An administrator must manually assign 'Lock pages in memory' to the intended original account using Windows account-policy tools.", ACCOUNT_MISMATCH};
    }
    if (precondition != SUCCESS) throw Error{L"An elevated administrator token for the same interactive Windows account is required.", NOT_ELEVATED};
    const Rights before{ReadRights(identity)};
    if (!HasRight(before)) {
        // POLICY_CREATE_ACCOUNT is needed if this SID has no direct LSA record.
        // No all-access handle, account creation elsewhere, or removal API.
        Policy policy{POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT};
        LSA_UNICODE_STRING right{};
        right.Buffer = const_cast<wchar_t*>(RIGHT_NAME);
        right.Length = static_cast<USHORT>(sizeof(RIGHT_NAME) - sizeof(wchar_t));
        right.MaximumLength = static_cast<USHORT>(sizeof(RIGHT_NAME));
        const NTSTATUS status{LsaAddAccountRights(policy.value, identity.Sid(), &right, 1)};
        if (status != 0) Fail(L"Cannot add the large-pages account right", LsaNtStatusToWinError(status));
    }
    const Rights after{ReadRights(identity)};
    if (!HasRight(after) || !PreservesRights(before, after)) {
        throw Error{L"The post-change verification did not confirm the requested right and all original rights. No automatic removal or rollback was attempted; review the local account policy."};
    }
    Output(L"Large-pages account right verified for " + identity.sid_text + L". Existing direct rights preserved.");
}

std::wstring SelfPath()
{
    std::vector<wchar_t> path(32768);
    const DWORD size{GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()))};
    if (!size || size >= path.size()) Fail(L"Cannot determine this helper's complete executable path");
    return {path.data(), size};
}

int Elevate(const Identity& original)
{
    if (!InteractiveUser(original)) throw Error{L"Start this helper from your Windows desktop account, not a service or installer system process."};
    const std::wstring path{SelfPath()};
    // Keep the exact executable locked against writes/deletion while relaunching.
    // MSI installs it in the protected per-machine application directory.
    Handle executable{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (executable.value == INVALID_HANDLE_VALUE) Fail(L"Cannot lock the helper executable for elevation");
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(executable.value, &information)) Fail(L"Cannot verify the helper executable");
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) throw Error{L"Refusing an indirect helper executable."};
    const std::wstring arguments{L"--enable-elevated " + original.sid_text}; // canonical token SID: no quoting/shell metacharacters
    const HRESULT initialized{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)};
    if (FAILED(initialized)) throw Error{L"Cannot initialize Windows elevation support."};
    struct ComCleanup { ~ComCleanup() { CoUninitialize(); } } cleanup;
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = path.c_str();
    execute.lpParameters = arguments.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        const DWORD error{GetLastError()};
        if (error == ERROR_CANCELLED) return CANCELLED;
        Fail(L"Cannot start administrator approval", error);
    }
    Handle child{execute.hProcess};
    if (!child.value) throw Error{L"Windows returned no handle for the elevated helper."};
    if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0) Fail(L"Cannot wait for the elevated helper");
    DWORD code{0};
    if (!GetExitCodeProcess(child.value, &code)) Fail(L"Cannot obtain the elevated helper result");
    if (code > static_cast<DWORD>(std::numeric_limits<int>::max())) return FAILURE;
    return static_cast<int>(code);
}

std::wstring Status(const Identity& identity, bool& directly_assigned)
{
    std::wstring direct;
    try {
        directly_assigned = HasRight(ReadRights(identity));
        direct = directly_assigned ? L"assigned" : L"not assigned";
    } catch (const Error& error) {
        directly_assigned = false;
        direct = L"unknown (" + error.message + L")";
    }
    return L"Windows account SID: " + identity.sid_text +
           L"\nDirect SeLockMemoryPrivilege: " + direct +
           L"\nPresent in this process token: " + (identity.privilege_present ? L"yes" : L"no") +
           L"\nEnabled in this process token: " + (identity.privilege_enabled ? L"yes" : L"no") +
           L"\nElevated: " + (identity.elevated ? L"yes" : L"no") +
           L"\n\nDirect account rights do not include rights inherited through groups. Status does not allocate RAM or change any right.";
}

int Enable(const Identity& original)
{
    if (original.elevated) {
        Grant(original, original.sid_text);
        return SUCCESS;
    }
    return Elevate(original);
}

void Check(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
}

void SelfTest()
{
    // Pure decision/serialization checks only: no token queries, LSA, UAC, files,
    // large-page allocation or account-policy changes in this test path.
    const std::wstring sid{L"S-1-5-21-1-2-3-1001"};
    Check(CanonicalSid(sid), "canonical SID");
    Check(!CanonicalSid(L"BA"), "SID alias rejected");
    Check(!CanonicalSid(L"S-1-5-21-1-2-3-1001 --arbitrary"), "SID injection rejected");
    Check(!CanonicalSid(L""), "empty SID rejected");
    Check(GrantPrecondition(sid, sid, true, true, true) == SUCCESS, "same elevated account accepted");
    Check(GrantPrecondition(sid, L"S-1-5-21-1-2-3-500", true, true, true) == ACCOUNT_MISMATCH, "alternate admin rejected");
    Check(GrantPrecondition(sid, sid, false, true, true) == NOT_ELEVATED, "filtered token rejected");
    Check(GrantPrecondition(sid, sid, true, false, true) == NOT_ELEVATED, "non-admin rejected");
    Check(GrantPrecondition(sid, sid, true, true, false) == NOT_ELEVATED, "service context rejected");
    Check(!HasRight({}), "empty rights");
    Check(!HasRight({L"SeDebugPrivilege"}), "unrelated right");
    Check(HasRight({L"SeDebugPrivilege", RIGHT_NAME}), "requested right");
    Check(PreservesRights({}, {RIGHT_NAME}), "first direct right");
    Check(PreservesRights({L"SeBatchLogonRight"}, {RIGHT_NAME, L"SeBatchLogonRight"}), "existing rights preserved");
    Check(!PreservesRights({L"SeBatchLogonRight"}, {RIGHT_NAME}), "lost right rejected");
    Check(PreservesRights({RIGHT_NAME}, {RIGHT_NAME}), "idempotence");
    Output(L"PASS: 16 read-only large-pages helper self-tests.");
}

int Run(const std::vector<std::wstring>& arguments)
{
    const std::wstring command{arguments.size() > 1 ? arguments[1] : L"status"};
    const bool configure{command == L"configure"};
    if (!configure) AttachOutput();
    try {
        if (command == L"--self-test" && arguments.size() == 2) {
            SelfTest();
            return SUCCESS;
        }
        if ((command == L"--help" || command == L"help") && arguments.size() == 2) {
            Output(L"ConnectCoin large-pages helper: status | configure | enable | --self-test\nstatus is read-only. configure asks before elevation. enable explicitly requests the right for this account only. There is no arbitrary-SID or removal option.");
            return SUCCESS;
        }
        const bool child{command == L"--enable-elevated"};
        if (child) {
            if (arguments.size() != 3 || !CanonicalSid(arguments[2])) throw Error{L"Invalid internal elevation arguments.", USAGE};
        } else if ((command != L"status" && command != L"enable" && !configure) || arguments.size() > 2) {
            throw Error{L"Usage: connectcoin-huge-pages.exe status | configure | enable | --self-test", USAGE};
        }
        const Identity identity{CurrentIdentity()};
        if (child) {
            // The passed SID is only an equality guard. The actual LSA target
            // always comes from this process's verified token, never argv.
            Grant(identity, arguments[2]);
            return SUCCESS;
        }
        bool assigned{false};
        const std::wstring status{Status(identity, assigned)};
        if (command == L"status") {
            Output(status);
            return SUCCESS;
        }
        if (!InteractiveUser(identity)) throw Error{L"Start the helper as the intended interactive desktop user, not from a system/installer service."};
        if (assigned || identity.privilege_present) {
            const std::wstring message{status + L"\n\nThe right is already assigned or available in this token. No account-policy change is needed.\n\n" + LOGON_NOTE};
            if (configure) MessageBoxW(nullptr, message.c_str(), TITLE, MB_OK | MB_ICONINFORMATION);
            else Output(message);
            return SUCCESS;
        }
        if (configure) {
            const std::wstring question{status +
                L"\n\nAllow this Windows account to use large pages ('Lock pages in memory')? This account-wide right can be used by other programs running as this account, not just Core. Locked RAM cannot be paged out and can reduce memory available to other applications.\n\n"
                L"Only SeLockMemoryPrivilege is added; existing rights are preserved. This helper does not allocate RAM, start/elevate Core, touch wallet data, start mining, or change execution/UAC policy. The right remains after uninstalling Core.\n\n"
                L"Administrator approval is required. If UAC asks for a DIFFERENT administrator account, this helper will refuse the change; ask an administrator to configure the intended account manually.\n\n" +
                LOGON_NOTE + L"\n\nEnable this right for the account shown above?"};
            if (MessageBoxW(nullptr, question.c_str(), TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return CANCELLED;
        }
        const int result{Enable(identity)};
        if (result == ACCOUNT_MISMATCH) throw Error{L"UAC used a different Windows account. No right was changed. Ask an administrator to manually assign 'Lock pages in memory' to the original account shown by status. Do not run Core as administrator.", ACCOUNT_MISMATCH};
        if (result == CANCELLED) {
            if (configure) MessageBoxW(nullptr, L"Administrator approval was cancelled. No right was changed.", TITLE, MB_OK | MB_ICONINFORMATION);
            return CANCELLED;
        }
        if (result != SUCCESS) throw Error{L"The elevated helper could not verify completion. Run status to inspect the current account right; if necessary, ask an administrator to review local account policy. No rollback or removal was attempted.", result};
        const std::wstring message{L"The large-pages account right and preservation of existing rights were verified for " + identity.sid_text + L".\n\n" + LOGON_NOTE};
        if (configure) MessageBoxW(nullptr, message.c_str(), TITLE, MB_OK | MB_ICONINFORMATION);
        else Output(message);
        return SUCCESS;
    } catch (const Error& error) {
        if (configure) MessageBoxW(nullptr, error.message.c_str(), TITLE, MB_OK | MB_ICONERROR);
        else Output(error.message, true);
        return error.exit_code;
    } catch (const std::exception&) {
        if (configure) MessageBoxW(nullptr, L"An unexpected helper failure occurred. No automatic removal or rollback was attempted.", TITLE, MB_OK | MB_ICONERROR);
        else Output(L"An unexpected helper failure occurred. No automatic removal or rollback was attempted.", true);
        return FAILURE;
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count{0};
    LocalBuffer argv;
    argv.value = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv.value || count < 1) return FAILURE;
    const auto* strings{static_cast<wchar_t**>(argv.value)};
    try {
        return Run(std::vector<std::wstring>(strings, strings + count));
    } catch (...) {
        return FAILURE;
    }
}
