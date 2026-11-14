// Winsock (imported by ordinal). ponytail: networking is reported as down (WSAENETDOWN), so the game's online
// features see no connection; bridge real sockets here if online leaderboards turn out to matter.
#include "host.h"
#include <arpa/inet.h>
#include <string.h>

#define WS "WS2_32.dll"
#define LAST_ERROR(c) U32(REG(c, R_FS_OFFSET) + 0x34)
#define NETDOWN 10050u

IMPX(WS, "#115", WSAStartup) {   // (version, WSADATA*)
    uint32_t d = ARG(c, 1);
    memset(GM(d), 0, 400);
    V(GM(d), uint16_t) = 0x202;
    V(GM(d + 2), uint16_t) = 0x202;
    strcpy(GSTR(d + 4), "WinSock 2.0");
    ret_std(c, 0, 2);
}
IMPX(WS, "#116", WSACleanup) { ret_std(c, 0, 0); }
IMPX(WS, "#111", WSAGetLastError) { ret_std(c, LAST_ERROR(c), 0); }
IMPX(WS, "#112", WSASetLastError) { LAST_ERROR(c) = ARG(c, 0); ret_std(c, 0, 1); }
IMPX(WS, "#8", htonl) { ret_std(c, htonl(ARG(c, 0)), 1); }
IMPX(WS, "#14", ntohl) { ret_std(c, ntohl(ARG(c, 0)), 1); }
IMPX(WS, "#9", htons) { ret_std(c, htons((uint16_t)ARG(c, 0)), 1); }
IMPX(WS, "#15", ntohs) { ret_std(c, ntohs((uint16_t)ARG(c, 0)), 1); }
IMPX(WS, "#57", gethostname) {
    if (ARG(c, 1) < 10) { LAST_ERROR(c) = 10014; ret_std(c, 0xFFFFFFFF, 2); return; }
    strcpy(GSTR(ARG(c, 0)), "steamdeck");
    ret_std(c, 0, 2);
}
IMPX(WS, "#3", closesocket) { ret_std(c, 0, 1); }
IMPX(WS, "#151", __WSAFDIsSet) { ret_std(c, 0, 2); }
IMP(WS, getaddrinfo) { ret_std(c, 11001, 4); }   // WSAHOST_NOT_FOUND
IMP(WS, freeaddrinfo) { ret_std(c, 0, 1); }

// everything that would need a socket fails: SOCKET_ERROR / INVALID_SOCKET with WSAENETDOWN
#define FAIL(ord, id, nargs) IMPX(WS, ord, id) { LAST_ERROR(c) = NETDOWN; ret_std(c, 0xFFFFFFFF, nargs); }
FAIL("#23", socket, 3) FAIL("#1", accept, 3) FAIL("#2", bind, 3) FAIL("#4", connect, 3) FAIL("#5", getpeername, 3)
FAIL("#6", getsockname, 3) FAIL("#7", getsockopt, 5) FAIL("#10", ioctlsocket, 3) FAIL("#13", listen, 2)
FAIL("#16", recv, 4) FAIL("#17", recvfrom, 6) FAIL("#18", select, 5) FAIL("#19", send, 4) FAIL("#20", sendto, 6)
FAIL("#21", setsockopt, 5) FAIL("WSAIoctl", WSAIoctl_, 9)
