/*
 * sre_raknet_c.c
 * Pure C RakNet UDP socket emulation & deeply-connected 60 Hz LAN sync engine for SRE.
 */

#include "sre_lua.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SRE_RAKNET_PEER_MT "RakNet.Peer"

typedef struct {
    int sockfd;
    uint16_t port;
    int max_incoming;
    bool is_started;
    bool is_connected;
    struct sockaddr_in dest_addr;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint32_t pkts_sent;
    uint32_t pkts_recv;
} SreRakNetPeer;

static SreRakNetPeer g_sre_raknet_peer_instance = { -1, 0, 32, false, false, {0}, 0, 0, 0, 0 };

/* Global LAN Auto-Sync State */
int   g_sre_lan_sync_active = 0;
int   g_sre_lan_is_host = 0;
void* g_remote_hero_ghost = NULL;

extern void* caver_getHero_from_view(void* view);
extern void* caver_getHero_impl(void);
extern void caver_getPosition_impl(void* obj, float* x, float* y, float* z);
extern void caver_setPosition_impl(void* obj, float x, float y, float z);
extern void (*g_sre_CreateHeroObjectAt)(void* sc, void* pos, int facing_dir, int add_to_scene);

/* Forward declaration — defined at bottom of this file */
void sre_raknet_shutdown_impl(void);

/* peer:Startup(maxConn, socketDesc, priority) */
static int l_raknet_peer_startup(lua_State* L) {
    int max_conn = (g_lua_isnumber && g_lua_isnumber(L, 2)) ? (int)g_lua_tointeger(L, 2) : 32;
    uint16_t port = 0;

    if (g_lua_type && g_lua_type(L, 3) == 5 /* LUA_TTABLE */ && g_lua_getfield) {
        g_lua_getfield(L, 3, "port");
        if (g_lua_isnumber && g_lua_isnumber(L, -1)) {
            port = (uint16_t)g_lua_tointeger(L, -1);
        }
        if (g_lua_settop) g_lua_settop(L, -2);
    }
    if (port == 0) port = 12345;

    if (g_sre_raknet_peer_instance.sockfd >= 0) {
        close(g_sre_raknet_peer_instance.sockfd);
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (g_lua_pushstring) g_lua_pushstring(L, "SOCKET_FAILED_TO_BIND");
        return 1;
    }

    /* Enable SO_REUSEADDR and SO_REUSEPORT so bind never fails on port reuse */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        printf("[SRE-Net Error] Bind failed on port %d! Port already bound.\n", (int)port);
        if (g_lua_pushstring) g_lua_pushstring(L, "SOCKET_PORT_ALREADY_IN_USE");
        return 1;
    }

    g_sre_raknet_peer_instance.sockfd = fd;
    g_sre_raknet_peer_instance.port = port;
    g_sre_raknet_peer_instance.max_incoming = max_conn;
    g_sre_raknet_peer_instance.is_started = true;
    g_sre_lan_sync_active = 1;
    g_sre_lan_is_host = 1;

    printf("\n========================================================\n");
    printf("[SRE-Net] UDP Host Server Started on Port %d!\n", (int)port);
    printf("[SRE-Net] 60 Hz World & Hero Sync Engine ACTIVATED\n");
    printf("========================================================\n\n");

    if (g_lua_pushstring) g_lua_pushstring(L, "RAKNET_STARTED");
    return 1;
}

/* peer:SetMaximumIncomingConnections(maxConn) */
static int l_raknet_peer_set_max_incoming(lua_State* L) {
    if (g_lua_isnumber && g_lua_isnumber(L, 2)) {
        g_sre_raknet_peer_instance.max_incoming = (int)g_lua_tointeger(L, 2);
    }
    return 0;
}

/* peer:Connect(host, port, password) */
static int l_raknet_peer_connect(lua_State* L) {
    const char* host = (g_lua_tolstring) ? g_lua_tolstring(L, 2, NULL) : "127.0.0.1";
    int port = (g_lua_isnumber && g_lua_isnumber(L, 3)) ? (int)g_lua_tointeger(L, 3) : 12345;
    if (!host) host = "127.0.0.1";

    if (g_sre_raknet_peer_instance.sockfd < 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        g_sre_raknet_peer_instance.sockfd = fd;
        g_sre_raknet_peer_instance.is_started = true;
    }

    memset(&g_sre_raknet_peer_instance.dest_addr, 0, sizeof(g_sre_raknet_peer_instance.dest_addr));
    g_sre_raknet_peer_instance.dest_addr.sin_family = AF_INET;
    g_sre_raknet_peer_instance.dest_addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &g_sre_raknet_peer_instance.dest_addr.sin_addr);
    g_sre_raknet_peer_instance.is_connected = true;
    g_sre_lan_sync_active = 1;

    printf("\n========================================================\n");
    printf("[SRE-Net] Connecting to Host at %s:%d...\n", host, port);
    printf("[SRE-Net] 60 Hz World & Hero Sync Engine ACTIVATED\n");
    printf("========================================================\n\n");

    /* Send initial UDP ping packet (ID_CONNECTION_REQUEST = 12) */
    const char ping_pkt[1] = { 12 };
    sendto(g_sre_raknet_peer_instance.sockfd, ping_pkt, 1, 0,
           (struct sockaddr*)&g_sre_raknet_peer_instance.dest_addr, sizeof(g_sre_raknet_peer_instance.dest_addr));

    if (g_lua_pushstring) g_lua_pushstring(L, "CONNECTION_ATTEMPT_STARTED");
    return 1;
}

/* peer:Send(data, priority, reliability, channel, target_guid, broadcast) */
static int l_raknet_peer_send(lua_State* L) {
    if (g_sre_raknet_peer_instance.sockfd < 0 || !g_sre_raknet_peer_instance.is_started) {
        if (g_lua_pushinteger) g_lua_pushinteger(L, 0);
        return 1;
    }

    size_t payload_len = 0;
    const char* payload = (g_lua_tolstring) ? g_lua_tolstring(L, 2, &payload_len) : NULL;
    if (!payload || payload_len == 0) {
        if (g_lua_pushinteger) g_lua_pushinteger(L, 0);
        return 1;
    }

    struct sockaddr_in target = g_sre_raknet_peer_instance.dest_addr;
    if (target.sin_port == 0) {
        target.sin_family = AF_INET;
        target.sin_port = htons(12345);
        inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    }

    ssize_t sent = sendto(g_sre_raknet_peer_instance.sockfd, payload, payload_len, 0,
                          (struct sockaddr*)&target, sizeof(target));
    if (sent > 0) {
        g_sre_raknet_peer_instance.bytes_sent += (uint64_t)sent;
        g_sre_raknet_peer_instance.pkts_sent++;
    }

    if (g_lua_pushinteger) g_lua_pushinteger(L, (int)sent);
    return 1;
}

/* peer:Receive() */
static int l_raknet_peer_receive(lua_State* L) {
    if (g_sre_raknet_peer_instance.sockfd < 0 || !g_sre_raknet_peer_instance.is_started) {
        if (g_lua_pushnil) g_lua_pushnil(L);
        return 1;
    }

    char buf[2048];
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);

    ssize_t bytes_read = recvfrom(g_sre_raknet_peer_instance.sockfd, buf, sizeof(buf), 0,
                                  (struct sockaddr*)&src_addr, &addr_len);

    if (bytes_read <= 0) {
        if (g_lua_pushnil) g_lua_pushnil(L);
        return 1;
    }

    g_sre_raknet_peer_instance.dest_addr = src_addr;
    g_sre_raknet_peer_instance.is_connected = true;
    g_sre_raknet_peer_instance.bytes_recv += (uint64_t)bytes_read;
    g_sre_raknet_peer_instance.pkts_recv++;

    if (g_lua_createtable && g_lua_pushlstring && g_lua_setfield && g_lua_pushinteger) {
        g_lua_createtable(L, 0, 4);
        g_lua_pushlstring(L, buf, (size_t)bytes_read);
        g_lua_setfield(L, -2, "data");

        char ip_str[64];
        snprintf(ip_str, sizeof(ip_str), "%s:%d", inet_ntoa(src_addr.sin_addr), ntohs(src_addr.sin_port));
        g_lua_pushstring(L, ip_str);
        g_lua_setfield(L, -2, "systemAddress");

        g_lua_pushinteger(L, (int)bytes_read);
        g_lua_setfield(L, -2, "length");
        return 1;
    }

    if (g_lua_pushnil) g_lua_pushnil(L);
    return 1;
}

/* peer:DeallocatePacket(packet) */
static int l_raknet_peer_deallocate_packet(lua_State* L) {
    (void)L;
    return 0;
}

/* peer:Shutdown(blockDuration) */
static int l_raknet_peer_shutdown(lua_State* L) {
    (void)L;
    /* Delegate to the canonical shutdown implementation */
    sre_raknet_shutdown_impl();
    return 0;
}

/* Extra RakNet Telemetry APIs */
static int l_raknet_peer_is_connected(lua_State* L) {
    if (g_lua_pushboolean) g_lua_pushboolean(L, g_sre_raknet_peer_instance.is_connected ? 1 : 0);
    return 1;
}

static int l_raknet_peer_get_system_address(lua_State* L) {
    char ip_str[64];
    snprintf(ip_str, sizeof(ip_str), "%s:%d", inet_ntoa(g_sre_raknet_peer_instance.dest_addr.sin_addr),
             ntohs(g_sre_raknet_peer_instance.dest_addr.sin_port));
    if (g_lua_pushstring) g_lua_pushstring(L, ip_str);
    return 1;
}

static int l_raknet_peer_get_guid(lua_State* L) {
    if (g_lua_pushstring) g_lua_pushstring(L, "123456789");
    return 1;
}

static int l_raknet_peer_get_statistics(lua_State* L) {
    if (g_lua_createtable && g_lua_pushinteger && g_lua_setfield) {
        g_lua_createtable(L, 0, 4);
        g_lua_pushinteger(L, (int)g_sre_raknet_peer_instance.bytes_sent); g_lua_setfield(L, -2, "bytesSent");
        g_lua_pushinteger(L, (int)g_sre_raknet_peer_instance.bytes_recv); g_lua_setfield(L, -2, "bytesReceived");
        g_lua_pushinteger(L, (int)g_sre_raknet_peer_instance.pkts_sent);  g_lua_setfield(L, -2, "packetsSent");
        g_lua_pushinteger(L, (int)g_sre_raknet_peer_instance.pkts_recv);  g_lua_setfield(L, -2, "packetsReceived");
        return 1;
    }
    return 0;
}

static int l_raknet_get_local_ip(lua_State* L) {
    if (g_lua_pushstring) g_lua_pushstring(L, "127.0.0.1");
    return 1;
}

/* peer:NumberOfConnections() -> int */
static int l_raknet_peer_number_of_connections(lua_State* L) {
    if (g_lua_pushinteger)
        g_lua_pushinteger(L, g_sre_raknet_peer_instance.is_connected ? 1 : 0);
    return 1;
}

/* peer:GetMyGUID() -> string  (returns fd-based unique string like RakNet GUID) */
static int l_raknet_peer_get_my_guid(lua_State* L) {
    char guid_str[32];
    /* Build a reproducible GUID from the local sockfd to mimic RakNet's random GUID */
    snprintf(guid_str, sizeof(guid_str), "SREGUID_%d", g_sre_raknet_peer_instance.sockfd);
    if (g_lua_pushstring) g_lua_pushstring(L, guid_str);
    return 1;
}

/* peer:GetInternalID(address, idx) -> string  (returns local IP:port) */
static int l_raknet_peer_get_internal_id(lua_State* L) {
    char id_str[64];
    snprintf(id_str, sizeof(id_str), "127.0.0.1|%d", (int)g_sre_raknet_peer_instance.port);
    if (g_lua_pushstring) g_lua_pushstring(L, id_str);
    return 1;
}

/* peer:Ping(host, remotePort, onlyReplyOnAcceptingConnections) */
static int l_raknet_peer_ping(lua_State* L) {
    if (g_sre_raknet_peer_instance.sockfd < 0) {
        if (g_lua_pushboolean) g_lua_pushboolean(L, 0);
        return 1;
    }
    const char* host = (g_lua_tolstring) ? g_lua_tolstring(L, 2, NULL) : "127.0.0.1";
    int port = (g_lua_isnumber && g_lua_isnumber(L, 3)) ? (int)g_lua_tointeger(L, 3) : 12345;
    if (!host) host = "127.0.0.1";

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &target.sin_addr);

    /* RakNet Ping packet: ID_CONNECTED_PING = 0x00 */
    const char ping_pkt[1] = { 0x00 };
    ssize_t sent = sendto(g_sre_raknet_peer_instance.sockfd, ping_pkt, 1, 0,
                          (struct sockaddr*)&target, sizeof(target));
    if (g_lua_pushboolean) g_lua_pushboolean(L, sent > 0 ? 1 : 0);
    return 1;
}

/* peer:CloseConnection(target_guid, sendDisconnectionNotification, channel) */
static int l_raknet_peer_close_connection(lua_State* L) {
    (void)L;
    /* Send RakNet ID_DISCONNECTION_NOTIFICATION = 0x09 to dest, then mark disconnected */
    if (g_sre_raknet_peer_instance.sockfd >= 0 && g_sre_raknet_peer_instance.is_connected) {
        const char disc_pkt[1] = { 0x09 };
        sendto(g_sre_raknet_peer_instance.sockfd, disc_pkt, 1, 0,
               (struct sockaddr*)&g_sre_raknet_peer_instance.dest_addr,
               sizeof(g_sre_raknet_peer_instance.dest_addr));
        g_sre_raknet_peer_instance.is_connected = false;
    }
    return 0;
}

/* RakNet.GetInstance() — returns a peer table with all RakNet methods.
 * Matches libneedlewarfare raknet_lua.cpp PeerHolder API surface. */
static int l_raknet_get_instance(lua_State* L) {
    if (!g_lua_createtable || !g_lua_pushcclosure || !g_lua_setfield) return 0;

    g_lua_createtable(L, 0, 18);

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_startup, 0);
    g_lua_setfield(L, -2, "Startup");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_set_max_incoming, 0);
    g_lua_setfield(L, -2, "SetMaximumIncomingConnections");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_connect, 0);
    g_lua_setfield(L, -2, "Connect");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_send, 0);
    g_lua_setfield(L, -2, "Send");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_receive, 0);
    g_lua_setfield(L, -2, "Receive");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_deallocate_packet, 0);
    g_lua_setfield(L, -2, "DeallocatePacket");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_shutdown, 0);
    g_lua_setfield(L, -2, "Shutdown");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_is_connected, 0);
    g_lua_setfield(L, -2, "IsConnected");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_system_address, 0);
    g_lua_setfield(L, -2, "GetSystemAddress");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_guid, 0);
    g_lua_setfield(L, -2, "GetGUID");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_statistics, 0);
    g_lua_setfield(L, -2, "GetStatistics");

    /* --- libneedlewarfare parity: missing methods added --- */
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_number_of_connections, 0);
    g_lua_setfield(L, -2, "NumberOfConnections");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_my_guid, 0);
    g_lua_setfield(L, -2, "GetMyGUID");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_internal_id, 0);
    g_lua_setfield(L, -2, "GetInternalID");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_ping, 0);
    g_lua_setfield(L, -2, "Ping");

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_close_connection, 0);
    g_lua_setfield(L, -2, "CloseConnection");

    return 1;
}

static const void* raknet_lib[] = {
    (const void*)"GetInstance", (const void*)l_raknet_get_instance,
    (const void*)"GetLocalIP",  (const void*)l_raknet_get_local_ip,
    (const void*)0,             (const void*)0
};

void sre_register_raknet_lib(lua_State* L) {
    if (!g_luaL_register) return;
    g_luaL_register(L, "RakNet", (const void*)raknet_lib);
    if (g_lua_settop) g_lua_settop(L, -2);
}

/* =========================================================================
 * Deeply Connected 60 Hz Background Hero Sync Engine (sre_Scene_Update Integration)
 * ========================================================================= */
void sre_raknet_lan_sync_update(void* game_scene_view) {
    if (!g_sre_lan_sync_active || g_sre_raknet_peer_instance.sockfd < 0) return;

    static uint32_t frame_counter = 0;
    frame_counter++;

    /* 1. Broadcast Local Player Position */
    if (frame_counter % 2 == 0) {
        void* local_hero = caver_getHero_from_view(game_scene_view);
        if (!local_hero) local_hero = caver_getHero_impl();
        
        if (local_hero) {
            float x = 0, y = 0, z = 0;
            caver_getPosition_impl(local_hero, &x, &y, &z);

            if (x != 0.0f || y != 0.0f) {
                static int send_log_counter = 0;
                if (send_log_counter++ % 60 == 0) {
                    printf("[SRE-Net] Transmitting Player Position (x=%.2f, y=%.2f, z=%.2f) over UDP...\n", x, y, z);
                }

                char pkt_buf[17];
                pkt_buf[0] = (char)213; /* ID_SWORDIGO_PLAYER_SYNC */
                memcpy(pkt_buf + 1,  &x, 4);
                memcpy(pkt_buf + 5,  &y, 4);
                memcpy(pkt_buf + 9,  &z, 4);
                float dummy_dir = 1.0f;
                memcpy(pkt_buf + 13, &dummy_dir, 4);

                struct sockaddr_in target = g_sre_raknet_peer_instance.dest_addr;
                if (target.sin_port == 0) {
                    target.sin_family = AF_INET;
                    target.sin_port = htons(12345);
                    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
                }
                sendto(g_sre_raknet_peer_instance.sockfd, pkt_buf, 17, 0,
                       (struct sockaddr*)&target, sizeof(target));
            }
        }
    }

    /* 2. Receive Remote Player UDP Packets */
    char recv_buf[512];
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);

    while (1) {
        ssize_t n = recvfrom(g_sre_raknet_peer_instance.sockfd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr*)&src_addr, &addr_len);
        if (n < 17) break;

        if ((uint8_t)recv_buf[0] == 213) {
            float rx = 0, ry = 0, rz = 0;
            memcpy(&rx, recv_buf + 1, 4);
            memcpy(&ry, recv_buf + 5, 4);
            memcpy(&rz, recv_buf + 9, 4);

            if (rx != 0.0f || ry != 0.0f) {
                g_sre_raknet_peer_instance.dest_addr = src_addr;
                g_sre_raknet_peer_instance.is_connected = true;

                static int recv_log_counter = 0;
                if (recv_log_counter++ % 30 == 0) {
                    printf("[SRE-Net] Received Remote Hero Position from %s:%d -> (x=%.2f, y=%.2f, z=%.2f)\n",
                           inet_ntoa(src_addr.sin_addr), ntohs(src_addr.sin_port), rx, ry, rz);
                }

                /* Automatically instantiate remote player hero model in render scene without replacing main player hero */
                if (!g_remote_hero_ghost && g_sre_CreateHeroObjectAt && game_scene_view) {
                    uint64_t gc = *(uint64_t*)((char*)game_scene_view + 0xF8);
                    if (gc) {
                        void* sc = (void*)*(uint64_t*)((char*)gc + 0xC8);
                        if (sc) {
                            void* main_hero = *(void**)((char*)sc + 0x8); /* Save main hero pointer */
                            float spawn_pos[3] = { rx, ry, rz };
                            g_sre_CreateHeroObjectAt(sc, spawn_pos, 1, 1);
                            g_remote_hero_ghost = *(void**)((char*)sc + 0x8); /* Capture newly created 2nd hero ghost pointer */
                            *(void**)((char*)sc + 0x8) = main_hero; /* 1. Restore main hero pointer! */

                            /* 2. Re-bind CameraController to main_hero via UpdateTarget (0x0034A6BC) */
                            extern uint64_t g_swordigo_base;
                            typedef void (*pfn_UpdateTarget)(void* sc);
                            pfn_UpdateTarget fn_UpdateTarget = (pfn_UpdateTarget)(g_swordigo_base + 0x34A6BC);
                            if (fn_UpdateTarget) fn_UpdateTarget(sc);

                            printf("[SRE-Net] FORCED SPAWN Remote Player Hero Ghost in Scene at (x=%.2f, y=%.2f, z=%.2f)!\n", rx, ry, rz);
                        }
                    }
                }

                if (g_remote_hero_ghost) {
                    caver_setPosition_impl(g_remote_hero_ghost, rx, ry, rz);
                }
            }
        }
    }
}

/* =========================================================================
 * sre_raknet_c.c Public C API (called from sre_lua_libs.c Swd.Net delegates)
 *
 * These functions expose the RakNet peer lifecycle without requiring a
 * lua_State. Called from l_swd_net_host / l_swd_net_join in sre_lua_libs.c.
 * ========================================================================= */

int sre_raknet_startup_impl(uint16_t port) {
    if (port == 0) port = 12345;

    if (g_sre_raknet_peer_instance.sockfd >= 0) {
        close(g_sre_raknet_peer_instance.sockfd);
        g_sre_raknet_peer_instance.sockfd = -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[SRE-Net] Swd.Net.Host: socket() failed\n");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        printf("[SRE-Net] Swd.Net.Host: bind() failed on port %d (already in use?)\n", (int)port);
        return -1;
    }

    g_sre_raknet_peer_instance.sockfd       = fd;
    g_sre_raknet_peer_instance.port         = port;
    g_sre_raknet_peer_instance.max_incoming = 32;
    g_sre_raknet_peer_instance.is_started   = true;
    g_sre_lan_sync_active = 1;
    g_sre_lan_is_host     = 1;

    printf("\n========================================================\n");
    printf("[SRE-Net] Swd.Net.Host: UDP server started on port %d\n", (int)port);
    printf("[SRE-Net] 60 Hz LAN sync engine ACTIVE\n");
    printf("========================================================\n\n");
    return 0;
}

int sre_raknet_connect_impl(const char* host, uint16_t port) {
    if (!host || host[0] == '\0') host = "127.0.0.1";
    if (port == 0) port = 12345;

    /* Create client socket if not already open */
    if (g_sre_raknet_peer_instance.sockfd < 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            printf("[SRE-Net] Swd.Net.Join: socket() failed\n");
            return -1;
        }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        g_sre_raknet_peer_instance.sockfd     = fd;
        g_sre_raknet_peer_instance.is_started = true;
    }

    memset(&g_sre_raknet_peer_instance.dest_addr, 0,
           sizeof(g_sre_raknet_peer_instance.dest_addr));
    g_sre_raknet_peer_instance.dest_addr.sin_family = AF_INET;
    g_sre_raknet_peer_instance.dest_addr.sin_port   = htons(port);
    inet_pton(AF_INET, host, &g_sre_raknet_peer_instance.dest_addr.sin_addr);
    g_sre_raknet_peer_instance.is_connected = true;
    g_sre_lan_sync_active = 1;
    g_sre_lan_is_host     = 0;

    /* Send RakNet ID_CONNECTION_REQUEST = 0x0C */
    const char ping_pkt[1] = { 0x0C };
    sendto(g_sre_raknet_peer_instance.sockfd, ping_pkt, 1, 0,
           (struct sockaddr*)&g_sre_raknet_peer_instance.dest_addr,
           sizeof(g_sre_raknet_peer_instance.dest_addr));

    printf("\n========================================================\n");
    printf("[SRE-Net] Swd.Net.Join: connecting to %s:%d\n", host, (int)port);
    printf("[SRE-Net] 60 Hz LAN sync engine ACTIVE\n");
    printf("========================================================\n\n");
    return 0;
}

/**
 * sre_raknet_shutdown_impl — close the UDP peer socket and reset all state.
 * Called by Swd.Net.Disconnect() in sre_lua_libs.c, and by the Lua
 * peer:Shutdown() method in l_raknet_peer_shutdown.
 */
void sre_raknet_shutdown_impl(void) {
    if (g_sre_raknet_peer_instance.sockfd >= 0) {
        /* Send ID_DISCONNECTION_NOTIFICATION = 0x09 to peer before closing */
        if (g_sre_raknet_peer_instance.is_connected &&
            g_sre_raknet_peer_instance.dest_addr.sin_port != 0) {
            const char disc_pkt[1] = { 0x09 };
            sendto(g_sre_raknet_peer_instance.sockfd, disc_pkt, 1, 0,
                   (struct sockaddr*)&g_sre_raknet_peer_instance.dest_addr,
                   sizeof(g_sre_raknet_peer_instance.dest_addr));
        }
        close(g_sre_raknet_peer_instance.sockfd);
        g_sre_raknet_peer_instance.sockfd = -1;
    }
    g_sre_raknet_peer_instance.is_started   = false;
    g_sre_raknet_peer_instance.is_connected = false;
    g_sre_raknet_peer_instance.port         = 0;
    g_sre_lan_sync_active = 0;
    g_sre_lan_is_host     = 0;
    g_remote_hero_ghost   = NULL;
    printf("[SRE-Net] Peer socket closed — LAN sync stopped.\n");
}
