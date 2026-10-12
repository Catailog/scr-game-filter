#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>

#include "room_proxy.h"

#include <ws2tcpip.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "system_proxy.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

// 자체 포트를 쓴다. 다른 프로그램이 같은 포트를 잡고 있으면 시작 단계에서
// 바인드 실패로 알려준다.
constexpr int kRoomProxyPort = 28898;
constexpr const char* kPanelHtmlMarkerPath = "GameSearchPanel.html";
constexpr const char* kPanelJsMarkerPath = "GameSearchPanel.js";

// GameSearchPanel.js 안의 isGameValid 메서드는 검색어(압축 변수 t)를
// gameName/gameType/mapName/creatorName 중 하나에 포함되는지만 보는 단일
// substring 검사다. 이 판정 부분만 아래 코드로 치환하고, 나머지(state 갱신,
// LocalStorage 저장/복원, 비공개/리플레이/핑/인원수 필터)는 원본 그대로 둔다.
// 게임이 DOM을 그대로 그리므로 Preact 재렌더링과 충돌하지 않고 입력창 상태
// 동기화도 안 깨진다. 압축 변수명(t, e)에 의존하므로 게임이 패치되면 깨질 수
// 있고, 그때는 JS_PATCH_TOKEN_NOT_FOUND 로그가 남는다.
//
// 검색어 문법:
// - 쉼표로 나눈 토큰 중 하나라도 맞으면 보인다(OR). 공백은 구분자가 아니라
//   토큰 안의 글자이고, 토큰 앞뒤의 공백만 무시한다. 줄바꿈이라는 개념은 없다.
// - `-`로 시작하는 토큰은 제외 조건이다. 맞는 방은 다른 조건과 무관하게 숨긴다.
//   제외 토큰만 있으면 나머지 방을 전부 보여주고, 단독 `-`는 무시한다.
// - 쉼표가 만든 빈 토큰(쉼표 연달아, 맨 앞, 맨 끝)은 indexOf("")가 항상
//   맞으므로 전체 보기가 된다.
// - 역슬래시는 이스케이프로만 쓴다. `\,` `\-` `\\`는 각각 쉼표, 하이픈,
//   역슬래시 글자 그대로이고, `\-`는 토큰 맨 앞에서도 제외 표시가 아닌 일반
//   글자다. 그 외 글자 앞의 역슬래시나 맨 끝의 역슬래시는 잘못된 이스케이프
//   이므로 역슬래시와 뒤따르는 한 글자를 통째로 없는 것으로 친다.
//
// 구분은 정규식 split이 아니라 직접 훑어서 한다. 정규식으로는 이스케이프된
// 쉼표를 구분자에서 뺄 수 없기 때문이다.
// 아래 JS는 C++ 원시 문자열(R"JS(...)JS")이라 역슬래시가 C++ 이스케이프를
// 한 번 더 거치지 않는다. 소스에 보이는 `"\\"`가 곧 역슬래시 한 글자짜리 JS
// 문자열이다.
//
// 검사할 필드는 비트마스크 F로 정한다(room_proxy.h의 RoomFilterField).
// 포함 검사와 제외 검사에 같은 필드가 쓰인다.
//
// 기본 필터(D)는 검색창이 비어 있어도 항상 적용되고 검색창 내용(t)과 AND로
// 합쳐진다. 두 문자열은 같은 문법으로 ev()가 판정하며 빈 문자열은 항상
// 통과한다. 게임이 검색어를 소문자로 만들어 넘기므로 D는 JS 쪽에서
// toLowerCase()한다. D의 JS 문자열 리터럴은 BuildJsStringLiteral로 만든다.
constexpr const char* kSearchFilterFrom =
    "if(\"\"!==t&&!(e.gameName.toLowerCase().indexOf(t)>-1||e.gameType.toLowerCase().indexOf(t)>-1||"
    "e.mapName.toLowerCase().indexOf(t)>-1||e.creatorName.toLowerCase().indexOf(t)>-1))return!1;";
constexpr const char* kSearchFilterHead = R"JS(if(!function(){)JS";
constexpr const char* kSearchFilterTail =
    R"JS(function m(w){return(F&1&&e.gameName.toLowerCase().indexOf(w)>-1)||(F&2&&e.gameType.toLowerCase().indexOf(w)>-1)||)JS"
    R"JS((F&4&&e.mapName.toLowerCase().indexOf(w)>-1)||(F&8&&e.creatorName.toLowerCase().indexOf(w)>-1);})JS"
    R"JS(function ev(q){if(q==="")return true;var inc=[],exc=[],cur="",neg=false,st=true,ws=true,ac=false,i=0,n=q.length,c,d;)JS"
    R"JS(function p(f){cur=cur.replace(/\s+$/,"");)JS"
    R"JS(if(neg){if(cur.length)exc.push(cur);}else if(f||ac||cur.length)inc.push(cur);)JS"
    R"JS(cur="";neg=false;st=true;ws=true;ac=f;})JS"
    R"JS(while(i<n){c=q.charAt(i);)JS"
    R"JS(if(c=="\\"){d=q.charAt(i+1);if(d=="\\"||d=="-"||d==","){cur+=d;st=false;ws=false;}i+=2;continue;})JS"
    R"JS(if(c==","){p(true);i++;continue;})JS"
    R"JS(if(ws&&/\s/.test(c)){i++;continue;})JS"
    R"JS(if(c=="-"&&st){neg=true;st=false;i++;continue;}cur+=c;st=false;ws=false;i++;})JS"
    R"JS(p(false);)JS"
    R"JS(for(i=0;i<exc.length;i++)if(m(exc[i]))return false;)JS"
    R"JS(if(inc.length==0)return true;)JS"
    R"JS(for(i=0;i<inc.length;i++)if(m(inc[i]))return true;)JS"
    R"JS(return false;})JS"
    R"JS(return ev(D)&&ev(t);}())return!1;)JS";

// UTF-8 문자열을 JS 문자열 리터럴(따옴표 포함)로 만든다. JS 소스 안에 그대로
// 박히므로 따옴표, 역슬래시, 줄바꿈 같은 제어문자를 전부 이스케이프하지 않으면
// 패널 스크립트 전체가 문법 오류로 죽는다. U+2028/U+2029는 JS 문자열 안에서
// 줄 종결자로 취급될 수 있어 따로 이스케이프한다.
std::string BuildJsStringLiteral(const std::string& utf8) {
    std::string out = "\"";
    for (size_t i = 0; i < utf8.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else if (c == 0xE2 && i + 2 < utf8.size() &&
                   static_cast<unsigned char>(utf8[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(utf8[i + 2]) == 0xA8 ||
                    static_cast<unsigned char>(utf8[i + 2]) == 0xA9)) {
            out += static_cast<unsigned char>(utf8[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += "\"";
    return out;
}

bool IEquals(const std::string& a, const char* b) {
    return _stricmp(a.c_str(), b) == 0;
}

std::mutex g_roomProxyLogMutex;
void LogDebug(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_roomProxyLogMutex);
    std::filesystem::create_directories("debug");
    std::ofstream ofs("debug/room_proxy_debug.log", std::ios::app);
    if (!ofs) return;
    std::time_t t = std::time(nullptr);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t);
    ofs << std::put_time(&tmBuf, "%H:%M:%S") << " " << line << "\n";
}

bool SendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void PumpBytes(SOCKET from, SOCKET to) {
    char buf[8192];
    while (true) {
        int n = recv(from, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (!SendAll(to, buf, static_cast<size_t>(n))) break;
    }
    shutdown(to, SD_SEND);
}

SOCKET ConnectTo(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) {
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    return s;
}

void SendSimpleResponse(SOCKET client, int status, const std::string& contentType,
                         const std::string& body) {
    const char* reason = "OK";
    switch (status) {
        case 200: reason = "OK"; break;
        case 204: reason = "No Content"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 502: reason = "Bad Gateway"; break;
        default: reason = "Error"; break;
    }
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Cache-Control: no-store\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body;
    SendAll(client, resp.data(), resp.size());
}

using HeaderList = std::vector<std::pair<std::string, std::string>>;

std::optional<std::string> FindHeader(const HeaderList& headers, const char* name) {
    for (const auto& kv : headers) {
        if (IEquals(kv.first, name)) return kv.second;
    }
    return std::nullopt;
}

}  // namespace

struct RoomProxy::Impl {
    SystemProxyGuard proxyGuard;
    SOCKET listenSocket = INVALID_SOCKET;
    std::thread acceptThread;
    bool running = false;
    std::atomic<unsigned> fieldMask{kRoomFilterDefaultFields};
    std::mutex defaultFilterMutex;
    std::string defaultFilter;

    void AcceptLoop() {
        while (running) {
            SOCKET client = accept(listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (!running) break;
                continue;
            }
            std::thread(&Impl::HandleConnection, this, client).detach();
        }
    }

    // 헤더까지 다 받고 나면 {헤더 문자열, 헤더 뒤에 이미 같이 온 바디 조각}을
    // 돌려준다. 실패하면 빈 헤더를 돌려준다.
    static bool ReadHeaders(SOCKET client, std::string* headerPart, std::string* leftover) {
        std::string buf;
        char chunk[4096];
        size_t headerEnd = std::string::npos;
        while (headerEnd == std::string::npos) {
            int n = recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
            buf.append(chunk, n);
            headerEnd = buf.find("\r\n\r\n");
            if (buf.size() > 65536) return false;  // 비정상적으로 큰 헤더는 포기
        }
        *headerPart = buf.substr(0, headerEnd);
        *leftover = buf.substr(headerEnd + 4);
        return true;
    }

    static void ParseRequestLine(const std::string& headerPart, std::string* method,
                                  std::string* target, std::string* version, HeaderList* headers) {
        std::istringstream lines(headerPart);
        std::string requestLine;
        std::getline(lines, requestLine);
        if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();
        std::istringstream rl(requestLine);
        rl >> *method >> *target >> *version;

        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            size_t vstart = colon + 1;
            while (vstart < line.size() && line[vstart] == ' ') ++vstart;
            headers->emplace_back(std::move(name), line.substr(vstart));
        }
    }

    static void ReadBodyIfNeeded(SOCKET client, const HeaderList& headers, std::string* body) {
        auto cl = FindHeader(headers, "Content-Length");
        if (!cl) return;
        size_t need = 0;
        try {
            need = static_cast<size_t>(std::stoull(*cl));
        } catch (...) {
            return;
        }
        char chunk[4096];
        while (body->size() < need) {
            int n = recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            body->append(chunk, n);
        }
    }

    // HTTPS CONNECT 터널 - target은 "host:port" authority-form 그대로다.
    // 업스트림에 연결만 해주고 200을 돌려준 다음, 그 뒤로는 TLS를 전혀
    // 해석하지 않고 raw 바이트를 양방향으로 그대로 흘려보낸다(WS 업그레이드
    // 터널과 같은 방식) - 우리는 그냥 파이프일 뿐이다.
    void HandleConnectTunnel(SOCKET client, const std::string& target) {
        std::string host = target;
        int port = 443;
        size_t colon = target.rfind(':');
        if (colon != std::string::npos) {
            host = target.substr(0, colon);
            port = std::atoi(target.substr(colon + 1).c_str());
        }

        SOCKET upstream = ConnectTo(host, port);
        if (upstream == INVALID_SOCKET) {
            LogDebug("CONNECT_FAIL host=" + host + ":" + std::to_string(port));
            SendSimpleResponse(client, 502, "text/plain", "Bad Gateway");
            return;
        }

        const char* ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (!SendAll(client, ok, std::strlen(ok))) {
            closesocket(upstream);
            return;
        }

        std::thread t1([client, upstream]() { PumpBytes(upstream, client); });
        PumpBytes(client, upstream);
        if (t1.joinable()) t1.join();
        closesocket(upstream);
    }

    void RelayToUpstream(SOCKET client, const std::string& method, const std::string& target,
                          const std::string& version, const HeaderList& headers,
                          std::string leftoverBody) {
        std::string host;
        int port = 80;
        std::string path = target;

        if (target.rfind("http://", 0) == 0) {
            std::string rest = target.substr(7);
            size_t slash = rest.find('/');
            std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
            path = slash == std::string::npos ? "/" : rest.substr(slash);
            size_t colon = hostport.find(':');
            if (colon == std::string::npos) {
                host = hostport;
            } else {
                host = hostport.substr(0, colon);
                port = std::atoi(hostport.substr(colon + 1).c_str());
            }
        } else if (auto hostHeader = FindHeader(headers, "Host")) {
            size_t colon = hostHeader->find(':');
            if (colon == std::string::npos) {
                host = *hostHeader;
            } else {
                host = hostHeader->substr(0, colon);
                port = std::atoi(hostHeader->substr(colon + 1).c_str());
            }
        }

        if (host.empty()) {
            LogDebug("RELAY_FAIL reason=\"no host\" target=\"" + target + "\"");
            SendSimpleResponse(client, 400, "text/plain", "Bad Request");
            return;
        }

        bool isUpgrade = false;
        if (auto upgrade = FindHeader(headers, "Upgrade")) {
            isUpgrade = IEquals(*upgrade, "websocket");
        }
        LogDebug("RELAY " + method + " host=" + host + ":" + std::to_string(port) + " path=" +
                 path + (isUpgrade ? " upgrade=ws" : ""));

        SOCKET upstream = ConnectTo(host, port);
        if (upstream == INVALID_SOCKET) {
            LogDebug("RELAY_FAIL reason=\"connect upstream failed\" host=" + host + ":" +
                     std::to_string(port));
            SendSimpleResponse(client, 502, "text/plain", "Bad Gateway");
            return;
        }

        std::string outReq = method + " " + path + " " + version + "\r\n";
        for (const auto& kv : headers) {
            if (IEquals(kv.first, "Proxy-Connection")) continue;
            outReq += kv.first + ": " + kv.second + "\r\n";
        }
        outReq += "\r\n";
        SendAll(upstream, outReq.data(), outReq.size());

        ReadBodyIfNeeded(client, headers, &leftoverBody);
        if (!leftoverBody.empty()) {
            SendAll(upstream, leftoverBody.data(), leftoverBody.size());
        }

        if (isUpgrade) {
            // WS 핸드셰이크 응답(101 등)까지 통째로 릴레이한 다음 raw 바이트
            // 파이프로 전환한다 - WS 프레임 자체는 절대 해석하지 않는다(주입된
            // 스크립트가 진짜 WebSocket 객체로 알아서 디코딩한다).
            std::thread t1([client, upstream]() { PumpBytes(upstream, client); });
            PumpBytes(client, upstream);
            if (t1.joinable()) t1.join();
            closesocket(upstream);
            return;
        }

        std::string respBuf;
        size_t respHeaderEnd = std::string::npos;
        char rchunk[4096];
        while (respHeaderEnd == std::string::npos) {
            int n = recv(upstream, rchunk, sizeof(rchunk), 0);
            if (n <= 0) break;
            respBuf.append(rchunk, n);
            respHeaderEnd = respBuf.find("\r\n\r\n");
        }
        if (respHeaderEnd == std::string::npos) {
            closesocket(upstream);
            return;
        }

        std::string respHeaderPart = respBuf.substr(0, respHeaderEnd);
        std::string respBodySoFar = respBuf.substr(respHeaderEnd + 4);

        long long contentLength = -1;
        {
            std::istringstream hs(respHeaderPart);
            std::string hline;
            std::getline(hs, hline);  // status line
            while (std::getline(hs, hline)) {
                if (!hline.empty() && hline.back() == '\r') hline.pop_back();
                size_t colon = hline.find(':');
                if (colon == std::string::npos) continue;
                std::string name = hline.substr(0, colon);
                if (IEquals(name, "Content-Length")) {
                    size_t vstart = colon + 1;
                    while (vstart < hline.size() && hline[vstart] == ' ') ++vstart;
                    try {
                        contentLength = std::stoll(hline.substr(vstart));
                    } catch (...) {
                        contentLength = -1;
                    }
                }
            }
        }

        bool shouldPatchJs = path.find(kPanelJsMarkerPath) != std::string::npos;
        // GameSearchPanel.html은 내용을 안 건드리고, 그 안의
        // GameSearchPanel.js 참조에 캐시 무효화 쿼리만 붙인다. 안 그러면
        // 브라우저가 캐시된 예전 .js를 계속 써서 요청이 프록시를 다시 안
        // 거치고, 패치나 바뀐 검사 필드가 반영되지 않는다.
        bool shouldRewriteHtml = path.find(kPanelHtmlMarkerPath) != std::string::npos;

        if (!shouldPatchJs && !shouldRewriteHtml) {
            SendAll(client, respBuf.data(), respBuf.size());
            int n;
            while ((n = recv(upstream, rchunk, sizeof(rchunk), 0)) > 0) {
                SendAll(client, rchunk, static_cast<size_t>(n));
            }
            closesocket(upstream);
            return;
        }

        std::string body = respBodySoFar;
        if (contentLength >= 0) {
            while (static_cast<long long>(body.size()) < contentLength) {
                int n = recv(upstream, rchunk, sizeof(rchunk), 0);
                if (n <= 0) break;
                body.append(rchunk, n);
            }
        } else {
            int n;
            while ((n = recv(upstream, rchunk, sizeof(rchunk), 0)) > 0) {
                body.append(rchunk, n);
            }
        }
        closesocket(upstream);

        std::string modified = body;
        std::string contentType = shouldPatchJs ? "application/javascript; charset=utf-8"
                                                 : "text/html; charset=utf-8";

        if (shouldPatchJs) {
            size_t pos = modified.find(kSearchFilterFrom);
            if (pos != std::string::npos) {
                unsigned mask = fieldMask.load();
                std::string defaultText;
                {
                    std::lock_guard<std::mutex> lock(defaultFilterMutex);
                    defaultText = defaultFilter;
                }
                std::string patched = std::string(kSearchFilterHead) + "var F=" +
                                      std::to_string(mask) + ";var D=" +
                                      BuildJsStringLiteral(defaultText) + ".toLowerCase();" +
                                      kSearchFilterTail;
                modified.replace(pos, std::strlen(kSearchFilterFrom), patched);
                LogDebug("JS_PATCHED path=\"" + path + "\" fieldMask=" + std::to_string(mask) +
                         " defaultBytes=" + std::to_string(defaultText.size()));
            } else {
                LogDebug("JS_PATCH_TOKEN_NOT_FOUND path=\"" + path + "\"");
            }
        }

        if (shouldRewriteHtml) {
            std::string jsRef = kPanelJsMarkerPath;
            size_t jsPos = modified.find(jsRef);
            if (jsPos != std::string::npos) {
                size_t afterPos = jsPos + jsRef.size();
                bool alreadyHasQuery = afterPos < modified.size() && modified[afterPos] == '?';
                if (!alreadyHasQuery) {
                    std::string cacheBuster = "?owlbust=" + std::to_string(std::time(nullptr));
                    modified.insert(afterPos, cacheBuster);
                }
            }
        }

        std::istringstream hs2(respHeaderPart);
        std::string statusLine;
        std::getline(hs2, statusLine);
        if (!statusLine.empty() && statusLine.back() == '\r') statusLine.pop_back();

        std::string outResp = statusLine + "\r\n";
        outResp += "Content-Type: " + contentType + "\r\n";
        outResp += "Content-Length: " + std::to_string(modified.size()) + "\r\n";
        outResp += "Cache-Control: no-store\r\n";
        outResp += "Connection: close\r\n\r\n";
        outResp += modified;
        SendAll(client, outResp.data(), outResp.size());
    }

    void HandleConnection(SOCKET client) {
        std::string headerPart, leftover;
        if (!ReadHeaders(client, &headerPart, &leftover)) {
            closesocket(client);
            return;
        }

        std::string method, target, version;
        HeaderList headers;
        ParseRequestLine(headerPart, &method, &target, &version, &headers);
        if (method.empty() || target.empty()) {
            closesocket(client);
            return;
        }
        LogDebug("REQUEST " + method + " " + target +
                 (FindHeader(headers, "Host") ? (" host=" + *FindHeader(headers, "Host")) : ""));

        if (method == "CONNECT") {
            // HTTPS는 CONNECT로 터널링 요청이 온다 - target이 "host:port"
            // 그대로라(스킴/경로 없음) RelayToUpstream의 절대-URL/Host헤더
            // 파싱과는 완전히 다르게 다뤄야 한다. 여기서 처리 안 하면 이
            // 컴퓨터의 모든 HTTPS 트래픽(배틀넷 게이트웨이 목록 조회 포함)이
            // 그대로 깨진다 - ProxyOverride를 비워서 전체 트래픽이 우리를
            // 거치게 된 뒤 실제로 겪은 문제.
            HandleConnectTunnel(client, target);
            closesocket(client);
            return;
        }

        RelayToUpstream(client, method, target, version, headers, leftover);
        closesocket(client);
    }
};

RoomProxy::RoomProxy() : impl_(std::make_unique<Impl>()) {}

RoomProxy::~RoomProxy() {
    Stop();
}

bool RoomProxy::Start(std::string* errorOut) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        if (errorOut) *errorOut = "WSAStartup 실패";
        return false;
    }

    impl_->listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listenSocket == INVALID_SOCKET) {
        if (errorOut) *errorOut = "소켓 생성 실패";
        WSACleanup();
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(impl_->listenSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(static_cast<u_short>(kRoomProxyPort));
    if (bind(impl_->listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        if (errorOut)
            *errorOut =
                "포트 " + std::to_string(kRoomProxyPort) + " 바인드 실패 (다른 프로그램이 쓰는 중?)";
        closesocket(impl_->listenSocket);
        impl_->listenSocket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    if (listen(impl_->listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        if (errorOut) *errorOut = "리슨 실패";
        closesocket(impl_->listenSocket);
        impl_->listenSocket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    impl_->running = true;
    impl_->acceptThread = std::thread(&Impl::AcceptLoop, impl_.get());

    std::string proxyHostPort = "127.0.0.1:" + std::to_string(kRoomProxyPort);
    if (!impl_->proxyGuard.Start(proxyHostPort, errorOut)) {
        LogDebug("START_FAIL " + (errorOut ? *errorOut : std::string("(unknown)")));
        Stop();
        return false;
    }
    LogDebug("START ok proxyServer=" + proxyHostPort);
    return true;
}

void RoomProxy::SetFieldMask(unsigned mask) {
    impl_->fieldMask.store(mask & kRoomFilterAllFields);
}

void RoomProxy::SetDefaultFilter(const std::string& utf8) {
    std::lock_guard<std::mutex> lock(impl_->defaultFilterMutex);
    impl_->defaultFilter = utf8;
}

void RoomProxy::Stop() {
    impl_->proxyGuard.Stop();
    impl_->running = false;
    if (impl_->listenSocket != INVALID_SOCKET) {
        closesocket(impl_->listenSocket);  // accept()를 깨우기 위해
        impl_->listenSocket = INVALID_SOCKET;
        if (impl_->acceptThread.joinable()) impl_->acceptThread.join();
        WSACleanup();
    }
}
