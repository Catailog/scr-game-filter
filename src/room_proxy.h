#pragma once

#include <windows.h>

#include <memory>
#include <string>

// 방목록 검색어 판정에 쓸 필드. 비트마스크로 조합한다.
enum RoomFilterField : unsigned {
    kRoomFilterTitle = 1,    // 방 제목(gameName)
    kRoomFilterType = 2,     // 게임 타입(gameType)
    kRoomFilterMap = 4,      // 맵 이름(mapName)
    kRoomFilterCreator = 8,  // 방장 이름(creatorName)
    kRoomFilterAllFields = 15,
    // 설정 파일이 없을 때의 기본 선택. 게임 타입과 방장 이름은 꺼져 있다.
    kRoomFilterDefaultFields = kRoomFilterTitle | kRoomFilterMap,
};

// 게임의 임베디드 브라우저(SceneProxy)가 패널(HTML/JS)을 요청할 때 거치는
// 로컬 HTTP 프록시. GameSearchPanel.js의 검색 판정 부분만 쉼표 OR,
// `-` 제외, `\` 이스케이프를 지원하는 코드로 치환하고, 나머지 트래픽은 그대로
// 릴레이한다. 필터링과 화면 표시는 전부 게임 화면 안에서 끝난다.
class RoomProxy {
public:
    RoomProxy();
    ~RoomProxy();

    RoomProxy(const RoomProxy&) = delete;
    RoomProxy& operator=(const RoomProxy&) = delete;

    // 로컬 HTTP 프록시 서버를 띄우고 시스템 프록시(system_proxy.h)를 적용한다.
    // 시스템 프록시는 프로세스 단위가 아니라 OS 전역 설정이라 게임이 아직 안
    // 켜져 있어도 먼저 걸어둘 수 있다.
    bool Start(std::string* errorOut = nullptr);

    // 프록시 서버를 내리고 시스템 프록시 설정을 원래대로 되돌린다.
    void Stop();

    // 검색어 판정에 쓸 필드(RoomFilterField 조합)를 바꾼다. 이후 새로 받는
    // GameSearchPanel.js부터 반영되므로, 이미 열린 방목록 패널은 다시 열어야
    // 바뀐다.
    void SetFieldMask(unsigned mask);

    // 검색창이 비어 있어도 항상 적용되는 기본 필터(UTF-8)를 바꾼다. 검색창
    // 내용과 AND로 합쳐지고 같은 검색어 문법을 쓴다. 반영 시점은
    // SetFieldMask와 같다.
    void SetDefaultFilter(const std::string& utf8);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
