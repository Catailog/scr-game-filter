#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "room_proxy.h"
#include "system_proxy.h"

namespace {

constexpr int kIdFieldTitle = 101;
constexpr int kIdFieldType = 102;
constexpr int kIdFieldMap = 103;
constexpr int kIdFieldCreator = 104;
constexpr int kIdDefaultEdit = 110;
constexpr int kIdStatus = 200;

constexpr UINT_PTR kSaveTimerId = 1;
constexpr UINT kSaveDebounceMs = 500;

struct FieldCheckbox {
    int id;
    unsigned bit;
    const wchar_t* label;
};

constexpr FieldCheckbox kFieldCheckboxes[] = {
    {kIdFieldTitle, kRoomFilterTitle, L"방 제목"},
    {kIdFieldType, kRoomFilterType, L"게임 타입"},
    {kIdFieldMap, kRoomFilterMap, L"맵 이름"},
    {kIdFieldCreator, kRoomFilterCreator, L"방장 이름"},
};

RoomProxy g_roomProxy;
HFONT g_uiFont = nullptr;
bool g_proxyRunning = false;
// 저장된 값을 컨트롤에 채우는 동안 발생하는 변경 알림은 사용자 입력이 아니다.
bool g_loading = false;
bool g_saveDirty = false;

struct Settings {
    unsigned fields = kRoomFilterDefaultFields;
    std::string defaultFilter;  // UTF-8
};

std::wstring SettingsPath() {
    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath, len);
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
    return dir + L"scr-game-filter.json";
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr,
                        nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), wlen);
    return w;
}

// 방목록 필터에는 줄바꿈이라는 개념이 없으므로 입력칸에 줄바꿈이 남지 않게 한다.
std::wstring StripNewlines(const std::wstring& text) {
    std::wstring out;
    for (wchar_t ch : text) {
        if (ch != L'\r' && ch != L'\n') out += ch;
    }
    return out;
}

// ---- 설정 JSON: {"fields": 15, "defaultFilter": "..."} ----

std::string JsonEscape(const std::string& utf8) {
    std::string out;
    for (unsigned char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void AppendUtf8(std::string* out, unsigned cp) {
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// "key" 다음의 ':' 뒤 첫 비공백 위치. 키가 없으면 npos.
size_t FindJsonValueStart(const std::string& json, const char* key) {
    size_t pos = json.find(std::string("\"") + key + "\"");
    if (pos == std::string::npos) return std::string::npos;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return std::string::npos;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' ||
                                 json[pos] == '\n')) {
        ++pos;
    }
    return pos;
}

unsigned ParseHex4(const std::string& s, size_t pos) {
    if (pos + 4 > s.size()) return 0;
    return static_cast<unsigned>(std::strtoul(s.substr(pos, 4).c_str(), nullptr, 16));
}

std::optional<std::string> ReadJsonString(const std::string& json, const char* key) {
    size_t pos = FindJsonValueStart(json, key);
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] != '\\' || pos + 1 >= json.size()) {
            out += json[pos++];
            continue;
        }
        char esc = json[pos + 1];
        pos += 2;
        switch (esc) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = ParseHex4(json, pos);
                pos += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && pos + 6 <= json.size() && json[pos] == '\\' &&
                    json[pos + 1] == 'u') {
                    unsigned low = ParseHex4(json, pos + 2);
                    if (low >= 0xDC00 && low < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        pos += 6;
                    }
                }
                AppendUtf8(&out, cp);
                break;
            }
            default: out += esc;  // " \ / 그대로
        }
    }
    return out;
}

std::optional<long> ReadJsonNumber(const std::string& json, const char* key) {
    size_t pos = FindJsonValueStart(json, key);
    if (pos == std::string::npos || pos >= json.size()) return std::nullopt;
    const char* begin = json.c_str() + pos;
    char* end = nullptr;
    long value = std::strtol(begin, &end, 10);
    if (end == begin) return std::nullopt;
    return value;
}

Settings LoadSettings() {
    Settings s;
    HANDLE f = CreateFileW(SettingsPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s;
    DWORD size = GetFileSize(f, nullptr);
    std::string bytes(size, '\0');
    DWORD read = 0;
    if (size > 0) ReadFile(f, bytes.data(), size, &read, nullptr);
    CloseHandle(f);
    bytes.resize(read);
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);

    if (auto fields = ReadJsonNumber(bytes, "fields")) {
        unsigned mask = static_cast<unsigned>(*fields) & kRoomFilterAllFields;
        if (mask != 0) s.fields = mask;
    }
    if (auto text = ReadJsonString(bytes, "defaultFilter")) s.defaultFilter = *text;
    return s;
}

// 임시 파일에 쓴 뒤 교체한다. 쓰는 도중 종료돼도 기존 설정 파일이 깨지지 않는다.
void SaveSettings(const Settings& s) {
    std::string json = "{\n  \"fields\": " + std::to_string(s.fields) +
                       ",\n  \"defaultFilter\": \"" + JsonEscape(s.defaultFilter) + "\"\n}\n";
    std::wstring path = SettingsPath();
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    BOOL ok = WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != json.size()) {
        DeleteFileW(tmp.c_str());
        return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

unsigned ReadMaskFromCheckboxes(HWND hwnd) {
    unsigned mask = 0;
    for (const auto& cb : kFieldCheckboxes) {
        if (IsDlgButtonChecked(hwnd, cb.id) == BST_CHECKED) mask |= cb.bit;
    }
    return mask;
}

std::string ReadDefaultFilterFromEdit(HWND hwnd) {
    HWND edit = GetDlgItem(hwnd, kIdDefaultEdit);
    int len = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(edit, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return WideToUtf8(text);
}

// 입력칸에 줄바꿈이 있으면 지우고 true를 돌려준다. 커서는 지운 글자 수만큼 당겨서
// 원래 위치에 남긴다.
bool RemoveNewlinesFromEdit(HWND hwnd) {
    HWND edit = GetDlgItem(hwnd, kIdDefaultEdit);
    int len = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(edit, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    if (text.find_first_of(L"\r\n") == std::wstring::npos) return false;

    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart),
                 reinterpret_cast<LPARAM>(&selEnd));
    DWORD removedBeforeCaret = 0;
    for (DWORD i = 0; i < selStart && i < text.size(); ++i) {
        if (text[i] == L'\r' || text[i] == L'\n') ++removedBeforeCaret;
    }
    SetWindowTextW(edit, StripNewlines(text).c_str());
    DWORD caret = selStart - removedBeforeCaret;
    SendMessageW(edit, EM_SETSEL, caret, caret);
    return true;
}

// 변경이 연달아 들어오면 타이머를 계속 다시 걸어서, 마지막 변경 500ms 뒤에 한 번만 저장한다.
void ScheduleSave(HWND hwnd) {
    g_saveDirty = true;
    SetTimer(hwnd, kSaveTimerId, kSaveDebounceMs, nullptr);
}

void FlushSave(HWND hwnd) {
    KillTimer(hwnd, kSaveTimerId);
    if (!g_saveDirty) return;
    g_saveDirty = false;
    Settings s;
    s.fields = ReadMaskFromCheckboxes(hwnd);
    s.defaultFilter = ReadDefaultFilterFromEdit(hwnd);
    SaveSettings(s);
}

// 기본 필터 내용을 프록시에 넘기고, 적용 중인지 상태줄에 표시한다.
void ApplyDefaultFilter(HWND hwnd) {
    std::string text = ReadDefaultFilterFromEdit(hwnd);
    g_roomProxy.SetDefaultFilter(text);
    if (g_proxyRunning) {
        bool hasContent = text.find_first_not_of(" \t\r\n") != std::string::npos;
        SetWindowTextW(GetDlgItem(hwnd, kIdStatus),
                       hasContent ? L"프록시 동작 중... (기본 필터 적용 중)" : L"프록시 동작 중...");
    }
}

HWND AddControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
                int w, int h, int id = 0, DWORD exStyle = 0) {
    HWND ctrl = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    if (g_uiFont) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    return ctrl;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            NONCLIENTMETRICSW ncm{sizeof(ncm)};
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
                g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
            }

            AddControl(hwnd, L"STATIC", L"검색어를 판정할 항목", 0, 14, 12, 360, 18);
            int y = 36;
            for (const auto& cb : kFieldCheckboxes) {
                AddControl(hwnd, L"BUTTON", cb.label, BS_AUTOCHECKBOX, 24, y, 280, 20, cb.id);
                y += 24;
            }
            AddControl(hwnd, L"STATIC",
                       L"기본 필터 (검색창이 비어 있어도 항상 적용, 검색창 내용과 AND)", 0, 14, 142,
                       360, 18);
            AddControl(hwnd, L"EDIT", L"",
                       ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 14, 164, 360,
                       90, kIdDefaultEdit, WS_EX_CLIENTEDGE);
            AddControl(hwnd, L"STATIC",
                       L"A,B : A 또는 B 포함 (쉼표로만 구분, 공백은 글자)\r\n-A : A 포함 방 제외\r\n\\, \\- \\\\ : 쉼표, 하이픈, 역슬래시 글자",
                       0, 14, 264, 360, 54);
            AddControl(hwnd, L"STATIC",
                       L"※ 체크박스와 기본 필터를 바꾸면\r\n방목록 패널을 다시 열어야 반영됩니다.", 0, 14,
                       324, 360, 36);
            AddControl(hwnd, L"STATIC", L"", 0, 14, 366, 360, 18, kIdStatus);

            Settings settings = LoadSettings();
            g_loading = true;
            for (const auto& cb : kFieldCheckboxes) {
                CheckDlgButton(hwnd, cb.id,
                               (settings.fields & cb.bit) ? BST_CHECKED : BST_UNCHECKED);
            }
            SetWindowTextW(GetDlgItem(hwnd, kIdDefaultEdit),
                           StripNewlines(Utf8ToWide(settings.defaultFilter)).c_str());
            g_loading = false;
            g_roomProxy.SetFieldMask(settings.fields);
            g_roomProxy.SetDefaultFilter(settings.defaultFilter);

            // 시스템 프록시는 게임 프로세스와 무관한 OS 전역 설정이라 게임이 켜져
            // 있는지와 상관없이 시작할 때 바로 건다.
            std::string error;
            if (g_roomProxy.Start(&error)) {
                g_proxyRunning = true;
                ApplyDefaultFilter(hwnd);
            } else {
                SetWindowTextW(GetDlgItem(hwnd, kIdStatus), L"프록시 시작 실패");
                MessageBoxW(hwnd, Utf8ToWide(error).c_str(), L"방목록 프록시 시작 실패",
                            MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == kIdDefaultEdit && code == EN_CHANGE) {
                if (g_loading) return 0;
                // 붙여넣기로 줄바꿈이 들어오면 지운다. 텍스트를 다시 넣으면 이 알림이
                // 한 번 더 오고, 적용과 저장은 그쪽에서 한다.
                if (RemoveNewlinesFromEdit(hwnd)) return 0;
                ApplyDefaultFilter(hwnd);
                ScheduleSave(hwnd);
                return 0;
            }
            if (code != BN_CLICKED) break;
            for (const auto& cb : kFieldCheckboxes) {
                if (cb.id != id) continue;
                // 항목이 전부 꺼지면 어떤 검색어도 맞지 않아 방목록이 비므로, 마지막
                // 하나는 끌 수 없게 한다.
                if (ReadMaskFromCheckboxes(hwnd) == 0) {
                    CheckDlgButton(hwnd, id, BST_CHECKED);
                }
                g_roomProxy.SetFieldMask(ReadMaskFromCheckboxes(hwnd));
                ScheduleSave(hwnd);
                break;
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == kSaveTimerId) {
                FlushSave(hwnd);
                return 0;
            }
            break;
        case WM_DESTROY:
            // 마지막 변경 뒤 500ms가 지나기 전에 종료해도 변경이 사라지지 않게 한다.
            FlushSave(hwnd);
            g_roomProxy.Stop();
            if (g_uiFont) DeleteObject(g_uiFont);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 커맨드라인에 "--proxy-guardian <pid>"가 있으면(SystemProxyGuard::Start가
// 자기 자신을 이 인자로 재실행) 창을 띄우지 않고 그 프로세스가 죽을 때까지
// 기다렸다가 시스템 프록시를 강제로 원복해야 한다.
std::optional<DWORD> ParseGuardianParentPid() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return std::nullopt;
    std::optional<DWORD> result;
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--proxy-guardian") == 0) {
            result = static_cast<DWORD>(_wtoi(argv[i + 1]));
            break;
        }
    }
    LocalFree(argv);
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    if (auto parentPid = ParseGuardianParentPid()) {
        return SystemProxyGuard::RunGuardianAndExit(*parentPid);
    }

    const wchar_t* className = L"scr-game-filterWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, className, L"scr-game-filter",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 404, 454, nullptr, nullptr,
                                hInstance, nullptr);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // 다중 줄 에디트 컨트롤은 Ctrl+A를 기본으로 처리하지 않는다. 복사, 붙여넣기,
        // 잘라내기, 실행 취소는 컨트롤이 알아서 처리한다. 줄바꿈이 없는 입력칸이라
        // Enter는 막는다.
        if (msg.message == WM_KEYDOWN) {
            HWND edit = GetDlgItem(hwnd, kIdDefaultEdit);
            if (msg.hwnd == edit) {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (ctrl && msg.wParam == 'A') {
                    SendMessageW(edit, EM_SETSEL, 0, -1);
                    continue;
                }
                if (msg.wParam == VK_RETURN) continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
