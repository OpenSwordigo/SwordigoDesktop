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
#define MAX_PEERS 8

typedef struct {
    int sockfd;
    uint16_t port;
    int max_incoming;
    bool is_started;
    bool is_connected;
    struct sockaddr_in dest_addr; /* legacy for simple pings */
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint32_t pkts_sent;
    uint32_t pkts_recv;
} SreRakNetPeer;

typedef struct {
    bool is_active;
    struct sockaddr_in addr;
    char scene_name[128];
    uint32_t last_recv_time;
    uint8_t last_sequence;
    void* hero_ghost;
    /* Item 6 (docs/onlineplay/04_multiplayer_player_sync_and_interpolation_plan.md):
     * host->guest state is sent at 30 Hz but the ghost is rendered at frame rate.
     * Snapping the ghost to each received packet looks jittery, so we store the
     * latest received transform as an interpolation TARGET and ease the ghost's
     * visible position toward it every frame (see sre_raknet_lan_sync_update). */
    bool  have_target;      /* a PLAYER_SYNC target has been received at least once */
    float target_x, target_y, target_z; /* last received authoritative position */
    int   target_facing;    /* last received facing dir (EntityComponent + 0x70) */
} SrePeer;

static SreRakNetPeer g_sre_raknet_peer_instance = { -1, 0, 32, false, false, {0}, 0, 0, 0, 0 };
static SrePeer g_peers[MAX_PEERS];

enum {
    SRE_NET_DISCONNECT = 0x09,
    SRE_NET_CONNECTION_REQUEST = 0x0C,
    SRE_NET_CONNECTION_ACCEPTED = 0x0D,
    SRE_NET_PLAYER_SYNC = 213,
    SRE_NET_SCENE_CHANGE = 214,
    SRE_NET_KEEPALIVE = 215
};

/* Global LAN Auto-Sync State */
int   g_sre_lan_sync_active = 0;
int   g_sre_lan_is_host = 0;
void* g_remote_hero_ghost = NULL; /* Expose first connected client's ghost for global compat */

extern void* caver_getHero_from_view(void* view);
extern void* caver_getHero_impl(void);
extern void caver_getPosition_impl(void* obj, float* x, float* y, float* z);
extern void caver_setPosition_impl(void* obj, float x, float y, float z);
extern void (*g_sre_CreateHeroObjectAt)(void* sc, void* pos, int facing_dir, int add_to_scene);

/* g_SceneObject_ComponentWithInterface, EntityComponent_Interface, HealthComponent_Interface,
 * and the sre_health_get_hp / sre_health_set_hp static inline accessors are all declared in
 * sre_caver.h (included below). Declaring them as bare extern here left the health symbols
 * UNDEFINED in libsre.so (static inline never emits a definition), so every call resolved via
 * the bridge PLT stub -> "[Bridge64] !! UNHANDLED" flooding. Include the header instead so
 * each translation unit gets the inline definition and the exact typedefs. */
#include "sre_caver.h"

extern volatile int g_sre_scene_shift_pending;
extern char g_sre_scene_shift_target[128];
extern char g_sre_scene_shift_spawn[64];
extern char g_sre_current_scene_name[128];

void sre_raknet_shutdown_impl(void);

int sre_raknet_is_connected_impl(void) {
    for (int i=0; i<MAX_PEERS; i++) {
        if (g_peers[i].is_active) return 1;
    }
    return 0;
}

/* =========================================================================
 * Item 8 — Automatic nickname generator.
 *
 * Produces a fun, human-readable default player name for LAN sessions so a
 * player never has to type one. The name is deterministic for the lifetime of
 * the process (generated once, then cached) but varies between machines/runs
 * because it is seeded from pid + a time sample + the bound socket fd. Format:
 * "<Adjective><Noun><NN>" e.g. "SwiftGoblin42", which fits comfortably in the
 * 32-byte name field the multiplayer layer reserves for player names.
 * ========================================================================= */
static char g_sre_local_nickname[32] = {0};

const char* sre_generate_nickname(void) {
    if (g_sre_local_nickname[0]) return g_sre_local_nickname; /* cached */

    static const char* const adjectives[] = {
        "Swift", "Brave", "Silent", "Mighty", "Cunning", "Shadow", "Iron",
        "Golden", "Crimson", "Frost", "Storm", "Ancient", "Wild", "Noble",
        "Fierce", "Lucky", "Rogue", "Grim", "Blazing", "Emerald"
    };
    static const char* const nouns[] = {
        "Goblin", "Knight", "Mage", "Ranger", "Wyrm", "Golem", "Phantom",
        "Warden", "Hunter", "Slayer", "Wizard", "Paladin", "Reaper", "Druid",
        "Sentinel", "Nomad", "Raider", "Champion", "Warlock", "Sorcerer"
    };
    const int n_adj = (int)(sizeof(adjectives) / sizeof(adjectives[0]));
    const int n_noun = (int)(sizeof(nouns) / sizeof(nouns[0]));

    /* Mix a few cheap, dependency-free entropy sources so two instances differ.
     * We avoid getpid()/time() here because the SRE compiles against a minimal
     * libc shim (src/sre/include) that does not expose them; the socket fd, a
     * stack/static address (ASLR-influenced), and a monotonic counter give
     * enough variation for a friendly default name. */
    static unsigned int call_salt = 0x9E3779B9u;
    call_salt += 0x6D2B79F5u;
    unsigned int seed = call_salt;
    if (g_sre_raknet_peer_instance.sockfd >= 0)
        seed ^= (unsigned int)(g_sre_raknet_peer_instance.sockfd * 40503);
    seed ^= (unsigned int)((uintptr_t)&g_sre_local_nickname >> 4);
    seed ^= (unsigned int)((uintptr_t)&seed >> 3);

    /* xorshift so we don't rely on rand() global state. */
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;

    int ai = (int)(seed % (unsigned)n_adj);
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    int ni = (int)(seed % (unsigned)n_noun);
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    int num = (int)(seed % 100u); /* 00..99 */

    snprintf(g_sre_local_nickname, sizeof(g_sre_local_nickname),
             "%s%s%02d", adjectives[ai], nouns[ni], num);
    return g_sre_local_nickname;
}

static void sre_raknet_note_send(ssize_t sent) {
    if (sent > 0) {
        g_sre_raknet_peer_instance.bytes_sent += (uint64_t)sent;
        g_sre_raknet_peer_instance.pkts_sent++;
    }
}

static void sre_raknet_note_receive(ssize_t received) {
    if (received > 0) {
        g_sre_raknet_peer_instance.bytes_recv += (uint64_t)received;
        g_sre_raknet_peer_instance.pkts_recv++;
    }
}

static int find_or_add_peer(const struct sockaddr_in* addr) {
    int free_slot = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_peers[i].is_active) {
            if (g_peers[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
                g_peers[i].addr.sin_port == addr->sin_port) {
                return i;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        memset(&g_peers[free_slot], 0, sizeof(SrePeer));
        g_peers[free_slot].addr = *addr;
        g_peers[free_slot].is_active = true;
        g_sre_raknet_peer_instance.dest_addr = *addr;
        g_sre_raknet_peer_instance.is_connected = true;
        printf("[SRE-Net] Added peer %s:%d at slot %d\n", inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), free_slot);
        return free_slot;
    }
    return -1;
}

static void sre_raknet_accept_client(const struct sockaddr_in* source) {
    if (!source || !g_sre_lan_is_host) return;
    int idx = find_or_add_peer(source);
    if (idx >= 0) {
        const uint8_t accepted = SRE_NET_CONNECTION_ACCEPTED;
        ssize_t sent = sendto(g_sre_raknet_peer_instance.sockfd, &accepted, sizeof(accepted), 0,
                              (const struct sockaddr*)source, sizeof(*source));
        sre_raknet_note_send(sent);
        
        // Also send current scene immediately to sync newly connected client
        if (g_sre_current_scene_name[0]) {
            char pkt[129];
            pkt[0] = SRE_NET_SCENE_CHANGE;
            strncpy(pkt + 1, g_sre_current_scene_name, 128);
            sendto(g_sre_raknet_peer_instance.sockfd, pkt, sizeof(pkt), 0,
                   (struct sockaddr*)source, sizeof(*source));
        }
    }
}

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
    g_sre_raknet_peer_instance.is_connected = false;
    memset(&g_sre_raknet_peer_instance.dest_addr, 0, sizeof(g_sre_raknet_peer_instance.dest_addr));
    memset(g_peers, 0, sizeof(g_peers));
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
    
    memset(g_peers, 0, sizeof(g_peers));
    g_sre_raknet_peer_instance.is_connected = false;
    g_sre_lan_sync_active = 1;
    g_sre_lan_is_host = 0;

    printf("\n========================================================\n");
    printf("[SRE-Net] Connecting to Host at %s:%d...\n", host, port);
    printf("[SRE-Net] 60 Hz World & Hero Sync Engine ACTIVATED\n");
    printf("========================================================\n\n");

    const uint8_t ping_pkt = SRE_NET_CONNECTION_REQUEST;
    ssize_t sent = sendto(g_sre_raknet_peer_instance.sockfd, &ping_pkt, sizeof(ping_pkt), 0,
                          (struct sockaddr*)&g_sre_raknet_peer_instance.dest_addr,
                          sizeof(g_sre_raknet_peer_instance.dest_addr));
    sre_raknet_note_send(sent);

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

    // Try sending to all active peers, or just dest_addr if not connected
    ssize_t sent = 0;
    if (sre_raknet_is_connected_impl()) {
        for (int i=0; i<MAX_PEERS; i++) {
            if (g_peers[i].is_active) {
                sent = sendto(g_sre_raknet_peer_instance.sockfd, payload, payload_len, 0,
                              (struct sockaddr*)&g_peers[i].addr, sizeof(g_peers[i].addr));
            }
        }
    } else {
        struct sockaddr_in target = g_sre_raknet_peer_instance.dest_addr;
        if (target.sin_port == 0) {
            target.sin_family = AF_INET;
            target.sin_port = htons(12345);
            inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
        }
        sent = sendto(g_sre_raknet_peer_instance.sockfd, payload, payload_len, 0,
                      (struct sockaddr*)&target, sizeof(target));
    }
    
    sre_raknet_note_send(sent);

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

    if ((uint8_t)buf[0] == SRE_NET_CONNECTION_REQUEST) {
        sre_raknet_accept_client(&src_addr);
    } else {
        find_or_add_peer(&src_addr);
    }
    sre_raknet_note_receive(bytes_read);

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
    sre_raknet_shutdown_impl();
    return 0;
}

static int l_raknet_peer_is_connected(lua_State* L) {
    if (g_lua_pushboolean) g_lua_pushboolean(L, sre_raknet_is_connected_impl() ? 1 : 0);
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

static int l_raknet_peer_number_of_connections(lua_State* L) {
    int active = 0;
    for (int i=0; i<MAX_PEERS; i++) {
        if (g_peers[i].is_active) active++;
    }
    if (g_lua_pushinteger) g_lua_pushinteger(L, active);
    return 1;
}

static int l_raknet_peer_get_my_guid(lua_State* L) {
    char guid_str[32];
    snprintf(guid_str, sizeof(guid_str), "SREGUID_%d", g_sre_raknet_peer_instance.sockfd);
    if (g_lua_pushstring) g_lua_pushstring(L, guid_str);
    return 1;
}

static int l_raknet_peer_get_internal_id(lua_State* L) {
    char id_str[64];
    snprintf(id_str, sizeof(id_str), "127.0.0.1|%d", (int)g_sre_raknet_peer_instance.port);
    if (g_lua_pushstring) g_lua_pushstring(L, id_str);
    return 1;
}

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

    const char ping_pkt[1] = { 0x00 };
    ssize_t sent = sendto(g_sre_raknet_peer_instance.sockfd, ping_pkt, 1, 0,
                          (struct sockaddr*)&target, sizeof(target));
    if (g_lua_pushboolean) g_lua_pushboolean(L, sent > 0 ? 1 : 0);
    return 1;
}

static int l_raknet_peer_close_connection(lua_State* L) {
    (void)L;
    if (g_sre_raknet_peer_instance.sockfd >= 0) {
        const char disc_pkt[1] = { SRE_NET_DISCONNECT };
        for (int i=0; i<MAX_PEERS; i++) {
            if (g_peers[i].is_active) {
                sendto(g_sre_raknet_peer_instance.sockfd, disc_pkt, 1, 0,
                       (struct sockaddr*)&g_peers[i].addr, sizeof(g_peers[i].addr));
                g_peers[i].is_active = false;
                g_peers[i].hero_ghost = NULL;
            }
        }
    }
    return 0;
}

static int l_raknet_get_instance(lua_State* L) {
    if (!g_lua_createtable || !g_lua_pushcclosure || !g_lua_setfield) return 0;
    g_lua_createtable(L, 0, 18);

    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_startup, 0); g_lua_setfield(L, -2, "Startup");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_set_max_incoming, 0); g_lua_setfield(L, -2, "SetMaximumIncomingConnections");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_connect, 0); g_lua_setfield(L, -2, "Connect");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_send, 0); g_lua_setfield(L, -2, "Send");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_receive, 0); g_lua_setfield(L, -2, "Receive");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_deallocate_packet, 0); g_lua_setfield(L, -2, "DeallocatePacket");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_shutdown, 0); g_lua_setfield(L, -2, "Shutdown");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_is_connected, 0); g_lua_setfield(L, -2, "IsConnected");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_system_address, 0); g_lua_setfield(L, -2, "GetSystemAddress");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_guid, 0); g_lua_setfield(L, -2, "GetGUID");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_statistics, 0); g_lua_setfield(L, -2, "GetStatistics");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_number_of_connections, 0); g_lua_setfield(L, -2, "NumberOfConnections");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_my_guid, 0); g_lua_setfield(L, -2, "GetMyGUID");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_get_internal_id, 0); g_lua_setfield(L, -2, "GetInternalID");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_ping, 0); g_lua_setfield(L, -2, "Ping");
    g_lua_pushcclosure(L, (lua_CFunction)l_raknet_peer_close_connection, 0); g_lua_setfield(L, -2, "CloseConnection");

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

void sre_raknet_lan_sync_update(void* game_scene_view) {
    if (!g_sre_lan_sync_active || g_sre_raknet_peer_instance.sockfd < 0) return;

    static uint32_t frame_counter = 0;
    frame_counter++;

    /* 0. Remote-ghost interpolation (Item 6 — doc 04 §3).
     * State packets arrive at ~30 Hz but we run every frame, so ease each remote
     * ghost's visible position toward its last-received authoritative target with
     * framerate-independent exponential smoothing. alpha = 1 - exp(-rate * dt);
     * we approximate dt with 1/60s per tick (the sync loop is frame-driven) and use
     * a smoothing rate that reaches the target within ~2 packets, which keeps ghosts
     * responsive without visible teleport-jitter. Discrete state (facing/HP) is
     * applied immediately in the receive handler, not interpolated. */
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!g_peers[i].is_active || !g_peers[i].hero_ghost || !g_peers[i].have_target) continue;
        float cx = 0, cy = 0, cz = 0;
        caver_getPosition_impl(g_peers[i].hero_ghost, &cx, &cy, &cz);
        const float alpha = 0.35f; /* per-frame lerp factor (~1-exp(-25*(1/60))) */
        float nx = cx + (g_peers[i].target_x - cx) * alpha;
        float ny = cy + (g_peers[i].target_y - cy) * alpha;
        float nz = cz + (g_peers[i].target_z - cz) * alpha;
        /* Snap the last fraction so the ghost settles exactly and doesn't creep. */
        float dx = g_peers[i].target_x - nx, dy = g_peers[i].target_y - ny, dz = g_peers[i].target_z - nz;
        if ((dx*dx + dy*dy + dz*dz) < 0.0004f) {
            nx = g_peers[i].target_x; ny = g_peers[i].target_y; nz = g_peers[i].target_z;
        }
        caver_setPosition_impl(g_peers[i].hero_ghost, nx, ny, nz);
    }

    /* 1. Track scene changes and broadcast SCENE_CHANGE */
    static char last_local_scene[128] = {0};
    if (strcmp(last_local_scene, g_sre_current_scene_name) != 0) {
        printf("[SRE-Net] Local scene changed from '%s' to '%s'. Resetting ghosts.\n", last_local_scene, g_sre_current_scene_name);
        
        // Reset all remote ghosts as engine destroys them on load
        for (int i=0; i<MAX_PEERS; i++) {
            g_peers[i].hero_ghost = NULL;
        }
        g_remote_hero_ghost = NULL;
        strncpy(last_local_scene, g_sre_current_scene_name, sizeof(last_local_scene));
        
        // Host forces clients to follow
        if (g_sre_lan_is_host && g_sre_current_scene_name[0]) {
            char pkt[129];
            pkt[0] = SRE_NET_SCENE_CHANGE;
            strncpy(pkt + 1, g_sre_current_scene_name, 128);
            for (int i=0; i<MAX_PEERS; i++) {
                if (g_peers[i].is_active) {
                    sendto(g_sre_raknet_peer_instance.sockfd, pkt, sizeof(pkt), 0,
                           (struct sockaddr*)&g_peers[i].addr, sizeof(g_peers[i].addr));
                }
            }
        } else if (!g_sre_lan_is_host && g_sre_current_scene_name[0]) {
            // Client informs host of scene arrival to enable sync
            char pkt[129];
            pkt[0] = SRE_NET_SCENE_CHANGE;
            strncpy(pkt + 1, g_sre_current_scene_name, 128);
            for (int i=0; i<MAX_PEERS; i++) {
                if (g_peers[i].is_active) {
                    sendto(g_sre_raknet_peer_instance.sockfd, pkt, sizeof(pkt), 0,
                           (struct sockaddr*)&g_peers[i].addr, sizeof(g_peers[i].addr));
                }
            }
        }
    }

    /* 2. Keepalives and Timeout processing (1 Hz) */
    if (frame_counter % 60 == 0) {
        uint8_t keepalive = SRE_NET_KEEPALIVE;
        for (int i=0; i<MAX_PEERS; i++) {
            if (g_peers[i].is_active) {
                // Send keepalive
                sendto(g_sre_raknet_peer_instance.sockfd, &keepalive, 1, 0,
                       (struct sockaddr*)&g_peers[i].addr, sizeof(g_peers[i].addr));
                       
                // 5s silence disconnect (300 frames)
                if (frame_counter - g_peers[i].last_recv_time > 300) {
                    printf("[SRE-Net] Peer %s:%d timed out.\n", inet_ntoa(g_peers[i].addr.sin_addr), ntohs(g_peers[i].addr.sin_port));
                    g_peers[i].is_active = false;
                    g_peers[i].hero_ghost = NULL;
                }
            }
        }
        
        // Re-assign global ghost for compatibility
        g_remote_hero_ghost = NULL;
        for (int i=0; i<MAX_PEERS; i++) {
            if (g_peers[i].is_active && g_peers[i].hero_ghost) {
                g_remote_hero_ghost = g_peers[i].hero_ghost;
                break;
            }
        }
    }

    /* 3. Send local player state (30 Hz) */
    if (frame_counter % 2 == 0 && sre_raknet_is_connected_impl()) {
        void* local_hero = caver_getHero_from_view(game_scene_view);
        if (!local_hero) local_hero = caver_getHero_impl();
        
        if (local_hero) {
            float x = 0, y = 0, z = 0;
            caver_getPosition_impl(local_hero, &x, &y, &z);

            if (x != 0.0f || y != 0.0f) {
                int facing_dir = 1;
                if (g_SceneObject_ComponentWithInterface && EntityComponent_Interface) {
                    void* comp = g_SceneObject_ComponentWithInterface(local_hero, EntityComponent_Interface);
                    if (comp) facing_dir = *(int*)((char*)comp + 0x70);
                }

                uint8_t hp = 100;
                if (g_SceneObject_ComponentWithInterface && HealthComponent_Interface) {
                    void* comp = g_SceneObject_ComponentWithInterface(local_hero, HealthComponent_Interface);
                    if (comp) hp = (uint8_t)sre_health_get_hp(comp);
                }

                char pkt_buf[18];
                pkt_buf[0] = SRE_NET_PLAYER_SYNC;
                static uint8_t local_seq = 0;
                pkt_buf[1] = local_seq++;
                memcpy(pkt_buf + 2,  &x, 4);
                memcpy(pkt_buf + 6,  &y, 4);
                memcpy(pkt_buf + 10,  &z, 4);
                pkt_buf[14] = (uint8_t)facing_dir;
                pkt_buf[15] = hp;
                pkt_buf[16] = 0;
                pkt_buf[17] = 0;

                for (int i=0; i<MAX_PEERS; i++) {
                    if (g_peers[i].is_active && strcmp(g_peers[i].scene_name, g_sre_current_scene_name) == 0) {
                        ssize_t sent = sendto(g_sre_raknet_peer_instance.sockfd, pkt_buf, 18, 0,
                                              (struct sockaddr*)&g_peers[i].addr, sizeof(g_peers[i].addr));
                        sre_raknet_note_send(sent);
                    }
                }
            }
        }
    }

    /* 4. Receive Remote Packets */
    char recv_buf[512];
    struct sockaddr_in src_addr;
    int max_recv = 64; /* cap per-frame to avoid stalling on packet flood */

    while (max_recv-- > 0) {
        socklen_t addr_len = sizeof(src_addr); /* must reset each iteration */
        ssize_t n = recvfrom(g_sre_raknet_peer_instance.sockfd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr*)&src_addr, &addr_len);
        if (n <= 0) break;

        sre_raknet_note_receive(n);

        int peer_idx = find_or_add_peer(&src_addr);
        if (peer_idx < 0) continue;
        
        g_peers[peer_idx].last_recv_time = frame_counter;

        uint8_t pkt_id = (uint8_t)recv_buf[0];

        if (pkt_id == SRE_NET_CONNECTION_REQUEST) {
            sre_raknet_accept_client(&src_addr);
        } else if (pkt_id == SRE_NET_CONNECTION_ACCEPTED) {
            if (!g_sre_lan_is_host) {
                printf("[SRE-Net] Connected to host %s:%d\n", inet_ntoa(src_addr.sin_addr), ntohs(src_addr.sin_port));
                g_sre_raknet_peer_instance.is_connected = true;
                if (g_sre_current_scene_name[0]) {
                    char pkt[129];
                    pkt[0] = SRE_NET_SCENE_CHANGE;
                    strncpy(pkt + 1, g_sre_current_scene_name, 128);
                    sendto(g_sre_raknet_peer_instance.sockfd, pkt, sizeof(pkt), 0,
                           (struct sockaddr*)&src_addr, sizeof(src_addr));
                }
            }
        } else if (pkt_id == SRE_NET_DISCONNECT) {
            printf("[SRE-Net] Peer %d disconnected.\n", peer_idx);
            g_peers[peer_idx].is_active = false;
            g_peers[peer_idx].hero_ghost = NULL;
        } else if (pkt_id == SRE_NET_SCENE_CHANGE && n >= 129) {
            char new_scene[128];
            memcpy(new_scene, recv_buf + 1, 128);
            new_scene[127] = '\0';
            
            strncpy(g_peers[peer_idx].scene_name, new_scene, 128);
            
            if (!g_sre_lan_is_host && strcmp(g_sre_current_scene_name, new_scene) != 0) {
                printf("[SRE-Net] Host entered scene '%s'. Transitioning to follow...\n", new_scene);
                g_sre_scene_shift_pending = 1;
                strncpy((char*)g_sre_scene_shift_target, new_scene, sizeof(g_sre_scene_shift_target));
                strncpy((char*)g_sre_scene_shift_spawn, "start", sizeof(g_sre_scene_shift_spawn));
            } else {
                printf("[SRE-Net] Peer %d entered scene '%s'.\n", peer_idx, new_scene);
            }
            if (strcmp(g_sre_current_scene_name, new_scene) != 0) {
                /* If peer is no longer in our scene, their ghost is invalid */
                g_peers[peer_idx].hero_ghost = NULL;
            }
        } else if (pkt_id == SRE_NET_PLAYER_SYNC && n >= 18) {
            // Only process state sync if peer is in same scene
            if (strcmp(g_peers[peer_idx].scene_name, g_sre_current_scene_name) == 0) {
                uint8_t seq = (uint8_t)recv_buf[1];
                if ((int8_t)(seq - g_peers[peer_idx].last_sequence) > 0) {
                    g_peers[peer_idx].last_sequence = seq;
                    
                    float rx = 0, ry = 0, rz = 0;
                    memcpy(&rx, recv_buf + 2, 4);
                    memcpy(&ry, recv_buf + 6, 4);
                    memcpy(&rz, recv_buf + 10, 4);
                    uint8_t facing = (uint8_t)recv_buf[14];
                    uint8_t hp = (uint8_t)recv_buf[15];

                    if (rx != 0.0f || ry != 0.0f) {
                        if (!g_sre_CreateHeroObjectAt) {
                            extern uint64_t g_swordigo_base;
                            g_sre_CreateHeroObjectAt = (void*)(g_swordigo_base + 0x348e94);
                        }
                        if (!g_peers[peer_idx].hero_ghost && g_sre_CreateHeroObjectAt && game_scene_view) {
                            void* sc = *(void**)((char*)game_scene_view + 0xF0);
                            if (sc) {
                                void* main_hero = *(void**)((char*)sc + 0xd8);
                                float spawn_pos[3] = { rx, ry, rz };
                                g_sre_CreateHeroObjectAt(sc, spawn_pos, (int)facing, 1);
                                g_peers[peer_idx].hero_ghost = *(void**)((char*)sc + 0xd8);
                                *(void**)((char*)sc + 0xd8) = main_hero;

                                printf("[SRE-Net] Spawned remote hero for peer %d at (x=%.2f, y=%.2f, z=%.2f).\n", peer_idx, rx, ry, rz);
                            }
                        }

                        if (g_peers[peer_idx].hero_ghost) {
                            /* Item 6 (doc 04 §3 — interpolation): do NOT snap the ghost to
                             * the received position. State arrives at 30 Hz but we render at
                             * frame rate, so snapping looks jittery. Store the authoritative
                             * transform as an interpolation target; the per-frame smoothing
                             * pass at the top of this function eases the visible ghost toward
                             * it. If this is the first target for a freshly spawned ghost,
                             * snap once so it doesn't lerp from a stale origin. */
                            if (!g_peers[peer_idx].have_target) {
                                caver_setPosition_impl(g_peers[peer_idx].hero_ghost, rx, ry, rz);
                            }
                            g_peers[peer_idx].target_x = rx;
                            g_peers[peer_idx].target_y = ry;
                            g_peers[peer_idx].target_z = rz;
                            g_peers[peer_idx].target_facing = (int)facing;
                            g_peers[peer_idx].have_target = true;

                            /* Facing and HP are discrete state — apply them immediately. */
                            if (g_SceneObject_ComponentWithInterface) {
                                if (EntityComponent_Interface) {
                                    void* comp = g_SceneObject_ComponentWithInterface(g_peers[peer_idx].hero_ghost, EntityComponent_Interface);
                                    if (comp) *(int*)((char*)comp + 0x70) = (int)facing;
                                }
                                if (HealthComponent_Interface) {
                                    void* comp = g_SceneObject_ComponentWithInterface(g_peers[peer_idx].hero_ghost, HealthComponent_Interface);
                                    if (comp) sre_health_set_hp(comp, (float)hp);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

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
        printf("[SRE-Net] Swd.Net.Host: bind() failed on port %d\n", (int)port);
        return -1;
    }

    g_sre_raknet_peer_instance.sockfd       = fd;
    g_sre_raknet_peer_instance.port         = port;
    g_sre_raknet_peer_instance.max_incoming = 32;
    g_sre_raknet_peer_instance.is_started   = true;
    g_sre_raknet_peer_instance.is_connected = false;
    memset(&g_sre_raknet_peer_instance.dest_addr, 0, sizeof(g_sre_raknet_peer_instance.dest_addr));
    memset(g_peers, 0, sizeof(g_peers));
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

    memset(&g_sre_raknet_peer_instance.dest_addr, 0, sizeof(g_sre_raknet_peer_instance.dest_addr));
    g_sre_raknet_peer_instance.dest_addr.sin_family = AF_INET;
    g_sre_raknet_peer_instance.dest_addr.sin_port   = htons(port);
    inet_pton(AF_INET, host, &g_sre_raknet_peer_instance.dest_addr.sin_addr);
    
    memset(g_peers, 0, sizeof(g_peers));
    g_sre_raknet_peer_instance.is_connected = false;
    g_sre_lan_sync_active = 1;
    g_sre_lan_is_host     = 0;

    const uint8_t ping_pkt = SRE_NET_CONNECTION_REQUEST;
    ssize_t sent = sendto(g_sre_raknet_peer_instance.sockfd, &ping_pkt, sizeof(ping_pkt), 0,
                          (struct sockaddr*)&g_sre_raknet_peer_instance.dest_addr,
                          sizeof(g_sre_raknet_peer_instance.dest_addr));
    sre_raknet_note_send(sent);

    printf("\n========================================================\n");
    printf("[SRE-Net] Swd.Net.Join: connecting to %s:%d\n", host, (int)port);
    printf("[SRE-Net] 60 Hz LAN sync engine ACTIVE\n");
    printf("========================================================\n\n");
    return 0;
}

void sre_raknet_shutdown_impl(void) {
    if (g_sre_raknet_peer_instance.sockfd >= 0) {
        const char disc_pkt[1] = { SRE_NET_DISCONNECT };
        for (int i=0; i<MAX_PEERS; i++) {
            if (g_peers[i].is_active) {
                sendto(g_sre_raknet_peer_instance.sockfd, disc_pkt, 1, 0,
                       (struct sockaddr*)&g_peers[i].addr, sizeof(g_peers[i].addr));
            }
        }
        close(g_sre_raknet_peer_instance.sockfd);
        g_sre_raknet_peer_instance.sockfd = -1;
    }
    g_sre_raknet_peer_instance.is_started   = false;
    g_sre_raknet_peer_instance.is_connected = false;
    g_sre_raknet_peer_instance.port         = 0;
    memset(g_peers, 0, sizeof(g_peers));
    g_sre_lan_sync_active = 0;
    g_sre_lan_is_host     = 0;
    g_remote_hero_ghost   = NULL;
    printf("[SRE-Net] Peer socket closed — LAN sync stopped.\n");
}
