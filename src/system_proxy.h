#pragma once

#include <windows.h>

#include <string>

// 게임의 임베디드 브라우저(SceneProxy)가 패널(HTML/JS)을 요청할 때 로컬
// 프록시(room_proxy.h)를 거치도록 Windows 시스템 프록시 설정을 건드린다.
//
// SceneProxy는 PAC/AutoConfigURL을 읽지 않아서, ProxyServer를 정적으로 걸고
// (ProxyEnable=1, ProxyOverride 비움 = 예외 없이 전부 프록시를 거침) 쓴다.
// 그래서 이 컴퓨터의 다른 프로그램의 HTTP/HTTPS 트래픽도 전부 로컬 프록시를
// 거치고, 프록시가 죽거나 응답이 없으면 컴퓨터 전체의 인터넷이 끊길 수 있다.
// 종료 시 반드시 원래 값으로 되돌려야 하므로 AutoDetect/AutoConfigURL/
// ProxyEnable/ProxyServer/ProxyOverride 다섯 값을 전부 백업해두고 원복한다.
// 크래시 등으로 정상 종료 경로를 못 타는 경우에 대비해 별도 감시 프로세스
// (자기 자신을 --proxy-guardian <pid>로 재실행)가 부모 프로세스 생존 여부를
// 지켜보다가 죽으면 강제로 원복한다.
class SystemProxyGuard {
public:
    SystemProxyGuard() = default;
    ~SystemProxyGuard();

    SystemProxyGuard(const SystemProxyGuard&) = delete;
    SystemProxyGuard& operator=(const SystemProxyGuard&) = delete;

    // proxyHostPort(예: "127.0.0.1:28898")를 ProxyServer로 등록하고
    // ProxyEnable=1, ProxyOverride=""(예외 없음)로 설정한다. PAC/자동감지가
    // 끼어들지 않도록 AutoConfigURL/AutoDetect도 같이 꺼둔다. 시작 전에
    // 이전 실행이 크래시로 못 지운 백업이 남아있으면 그것부터 원복하고,
    // 그 다음 지금 상태를 새로 백업한 뒤 우리 값을 적용한다. 감시
    // 프로세스도 이때 같이 띄운다.
    bool Start(const std::string& proxyHostPort, std::string* errorOut = nullptr);

    // 다섯 값을 전부 백업해둔 원래 상태로 되돌리고 백업을 지운다.
    void Stop();

    bool IsActive() const { return active_; }

    // 크래시 복구 진입점. main()에서 커맨드라인에 "--proxy-guardian <pid>"가
    // 있으면 창을 띄우지 않고 이 함수만 실행한 뒤 프로세스를 종료해야 한다.
    // 부모(ppid)가 살아있는 동안 대기하다가, 부모가 죽으면(정상/비정상 종료
    // 무관) 백업이 아직 남아있을 때만(=부모가 정상 종료 시 Stop()으로 이미
    // 지웠으면 아무것도 안 함) 강제로 원복한다.
    static int RunGuardianAndExit(DWORD parentPid);

private:
    bool active_ = false;
};
