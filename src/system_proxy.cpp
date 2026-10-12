#include "system_proxy.h"

#include <wininet.h>

#include <string>

#pragma comment(lib, "wininet.lib")

namespace {

const wchar_t* kInternetSettingsKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

// 크래시로 못 지운 백업이 남아있는지 여기로 판단한다 - 이 키가 존재하면 이전
// 실행이 정상 종료(Stop())를 못 타고 죽었다는 뜻.
const wchar_t* kBackupKey = L"Software\\scr-game-filter\\ProxyBackup";

// ---- 레지스트리 문자열/DWORD 값 하나를 읽고/쓰고/지우는 범용 헬퍼 ----

bool ReadRegString(HKEY root, const wchar_t* subKey, const wchar_t* name, std::wstring* out) {
    HKEY hKey;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG rc = RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || type != REG_SZ || size == 0) {
        RegCloseKey(hKey);
        return false;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &size);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) return false;
    size_t nul = value.find(L'\0');
    if (nul != std::wstring::npos) value.resize(nul);
    *out = value;
    return true;
}

bool ReadRegDword(HKEY root, const wchar_t* subKey, const wchar_t* name, DWORD* out) {
    HKEY hKey;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    LONG rc = RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<LPBYTE>(out), &size);
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS && type == REG_DWORD;
}

bool WriteRegString(HKEY root, const wchar_t* subKey, const wchar_t* name, const std::wstring& value) {
    HKEY hKey;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    LONG rc = RegSetValueExW(hKey, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

bool WriteRegDword(HKEY root, const wchar_t* subKey, const wchar_t* name, DWORD value) {
    HKEY hKey;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    LONG rc = RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                              sizeof(value));
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

void DeleteRegValue(HKEY root, const wchar_t* subKey, const wchar_t* name) {
    HKEY hKey;
    if (RegOpenKeyExW(root, subKey, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;
    RegDeleteValueW(hKey, name);
    RegCloseKey(hKey);
}

void NotifyProxyChanged() {
    InternetSetOptionW(nullptr, INTERNET_OPTION_PROXY_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
}

// ---- 다섯 개 Internet Settings 값을 한 번에 다루기 위한 스냅샷 ----
// AutoDetect/ProxyEnable은 DWORD, 나머지는 문자열. 값이 원래 없었으면
// had=false로 표시해서 원복할 때 "지워야 할지/써야 할지"를 구분한다.
struct ProxySnapshot {
    bool hadAutoDetect = false;
    DWORD autoDetect = 0;
    bool hadAutoConfigUrl = false;
    std::wstring autoConfigUrl;
    bool hadProxyEnable = false;
    DWORD proxyEnable = 0;
    bool hadProxyServer = false;
    std::wstring proxyServer;
    bool hadProxyOverride = false;
    std::wstring proxyOverride;
};

ProxySnapshot ReadCurrentSnapshot() {
    ProxySnapshot s;
    s.hadAutoDetect = ReadRegDword(HKEY_CURRENT_USER, kInternetSettingsKey, L"AutoDetect", &s.autoDetect);
    s.hadAutoConfigUrl =
        ReadRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"AutoConfigURL", &s.autoConfigUrl);
    s.hadProxyEnable =
        ReadRegDword(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyEnable", &s.proxyEnable);
    s.hadProxyServer =
        ReadRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyServer", &s.proxyServer);
    s.hadProxyOverride =
        ReadRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyOverride", &s.proxyOverride);
    return s;
}

void ApplySnapshot(const ProxySnapshot& s) {
    if (s.hadAutoDetect) {
        WriteRegDword(HKEY_CURRENT_USER, kInternetSettingsKey, L"AutoDetect", s.autoDetect);
    } else {
        DeleteRegValue(HKEY_CURRENT_USER, kInternetSettingsKey, L"AutoDetect");
    }
    if (s.hadAutoConfigUrl) {
        WriteRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"AutoConfigURL", s.autoConfigUrl);
    } else {
        DeleteRegValue(HKEY_CURRENT_USER, kInternetSettingsKey, L"AutoConfigURL");
    }
    if (s.hadProxyEnable) {
        WriteRegDword(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyEnable", s.proxyEnable);
    } else {
        DeleteRegValue(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyEnable");
    }
    if (s.hadProxyServer) {
        WriteRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyServer", s.proxyServer);
    } else {
        DeleteRegValue(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyServer");
    }
    if (s.hadProxyOverride) {
        WriteRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyOverride", s.proxyOverride);
    } else {
        DeleteRegValue(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyOverride");
    }
    NotifyProxyChanged();
}

// ---- 백업 키 하나에 스냅샷 전체를 저장/복원 ----

void WriteBackup(const ProxySnapshot& s) {
    WriteRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadAutoDetect", s.hadAutoDetect ? 1 : 0);
    WriteRegDword(HKEY_CURRENT_USER, kBackupKey, L"AutoDetect", s.autoDetect);
    WriteRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadAutoConfigUrl", s.hadAutoConfigUrl ? 1 : 0);
    WriteRegString(HKEY_CURRENT_USER, kBackupKey, L"AutoConfigUrl", s.autoConfigUrl);
    WriteRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadProxyEnable", s.hadProxyEnable ? 1 : 0);
    WriteRegDword(HKEY_CURRENT_USER, kBackupKey, L"ProxyEnable", s.proxyEnable);
    WriteRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadProxyServer", s.hadProxyServer ? 1 : 0);
    WriteRegString(HKEY_CURRENT_USER, kBackupKey, L"ProxyServer", s.proxyServer);
    WriteRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadProxyOverride", s.hadProxyOverride ? 1 : 0);
    WriteRegString(HKEY_CURRENT_USER, kBackupKey, L"ProxyOverride", s.proxyOverride);
}

// 백업 키가 있으면 true를 돌려주고 snapshot을 채운다. 없으면 false.
bool ReadBackup(ProxySnapshot* out) {
    DWORD had = 0;
    if (!ReadRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadAutoDetect", &had)) {
        return false;  // 백업 키 자체가 없음
    }
    out->hadAutoDetect = had != 0;
    ReadRegDword(HKEY_CURRENT_USER, kBackupKey, L"AutoDetect", &out->autoDetect);

    ReadRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadAutoConfigUrl", &had);
    out->hadAutoConfigUrl = had != 0;
    ReadRegString(HKEY_CURRENT_USER, kBackupKey, L"AutoConfigUrl", &out->autoConfigUrl);

    ReadRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadProxyEnable", &had);
    out->hadProxyEnable = had != 0;
    ReadRegDword(HKEY_CURRENT_USER, kBackupKey, L"ProxyEnable", &out->proxyEnable);

    ReadRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadProxyServer", &had);
    out->hadProxyServer = had != 0;
    ReadRegString(HKEY_CURRENT_USER, kBackupKey, L"ProxyServer", &out->proxyServer);

    ReadRegDword(HKEY_CURRENT_USER, kBackupKey, L"HadProxyOverride", &had);
    out->hadProxyOverride = had != 0;
    ReadRegString(HKEY_CURRENT_USER, kBackupKey, L"ProxyOverride", &out->proxyOverride);
    return true;
}

void DeleteBackupKey() {
    RegDeleteTreeW(HKEY_CURRENT_USER, kBackupKey);
}

void RestoreFromBackupIfPresent() {
    ProxySnapshot backup;
    if (ReadBackup(&backup)) {
        ApplySnapshot(backup);
        DeleteBackupKey();
    }
}

void SpawnGuardian(DWORD mainPid) {
    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --proxy-guardian " +
                            std::to_wstring(mainPid);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW - 콘솔 창 없이 조용히 백그라운드에서 감시만 한다.
    if (CreateProcessW(exePath, cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

}  // namespace

bool SystemProxyGuard::Start(const std::string& proxyHostPort, std::string* errorOut) {
    // 이전 실행이 크래시로 못 지운 백업이 남아있으면 그 값으로 먼저 원복.
    RestoreFromBackupIfPresent();

    // 지금(=이 프로그램이 손대기 전) 상태를 새로 백업.
    ProxySnapshot before = ReadCurrentSnapshot();
    WriteBackup(before);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, proxyHostPort.c_str(), -1, nullptr, 0);
    std::wstring wHostPort(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 0) {
        MultiByteToWideChar(CP_UTF8, 0, proxyHostPort.c_str(), -1, wHostPort.data(), wlen);
    }

    // PAC/자동감지가 끼어들지 않게 끄고, 정적 프록시를 예외 없이(ProxyOverride
    // 비움) 건다. 게임의 임베디드 브라우저는 PAC를 읽지 않아 이 방식이어야 한다.
    bool ok = true;
    ok &= WriteRegDword(HKEY_CURRENT_USER, kInternetSettingsKey, L"AutoDetect", 0);
    ok &= WriteRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"AutoConfigURL", L"");
    ok &= WriteRegDword(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyEnable", 1);
    ok &= WriteRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyServer", wHostPort);
    ok &= WriteRegString(HKEY_CURRENT_USER, kInternetSettingsKey, L"ProxyOverride", L"");

    if (!ok) {
        if (errorOut) *errorOut = "시스템 프록시 레지스트리 쓰기 실패";
        RestoreFromBackupIfPresent();
        return false;
    }
    NotifyProxyChanged();

    SpawnGuardian(GetCurrentProcessId());

    active_ = true;
    return true;
}

void SystemProxyGuard::Stop() {
    if (!active_) {
        return;
    }
    RestoreFromBackupIfPresent();
    active_ = false;
}

SystemProxyGuard::~SystemProxyGuard() {
    Stop();
}

int SystemProxyGuard::RunGuardianAndExit(DWORD parentPid) {
    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (hParent) {
        WaitForSingleObject(hParent, INFINITE);
        CloseHandle(hParent);
    }
    // 부모가 정상 종료(Stop() 호출)했다면 이 시점에 백업 키가 이미 지워져
    // 있을 것이다 - 그럼 여기선 아무것도 안 한다. 크래시 등으로 못 지웠을
    // 때만 강제로 원복한다.
    RestoreFromBackupIfPresent();
    return 0;
}
