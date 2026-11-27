#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint32_t cb;
    uint32_t authentication_type;
    char auth_key[136];
    uint32_t timeout;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
} FakeIOTCConnectInput;

typedef struct {
    uint32_t cb;
    int32_t iotc_session_id;
    uint32_t iotc_channel_id;
    uint32_t timeout_sec;
    const char *account_or_identity;
    const char *password_or_token;
    uint32_t resend;
    uint32_t security_mode;
    uint32_t auth_type;
    uint32_t sync_recv_data;
    const void *callback;
    uint32_t reserved[2];
} FakeAVClientStartInConfig;

typedef struct {
    uint32_t cb;
    uint32_t server_type;
    int32_t resend;
    uint32_t security_mode;
    uint32_t auth_type;
    uint32_t reserved[3];
} FakeAVClientStartOutConfig;

typedef struct {
    uint32_t len;
    uint8_t *data;
} FakeFrame;

static int g_next_sid = 1;
static int g_initialized = 0;
static FakeFrame *g_frames = NULL;
static size_t g_frame_count = 0;
static size_t g_frame_idx = 0;
static uint8_t g_codec = 78;
static int64_t g_session_start_ms = 0;
static int64_t g_next_frame_ms = 0;
static int64_t g_drop_after_ms = 0;
static int g_did_drop = 0;
static int g_keep_open = 0;
static int64_t g_next_bat_ms = 0;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static void log_line(const char *line) {
    const char *path = getenv("FAKE_TUTK_LOG");
    if (!path || !*path) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%lld %s\n", (long long)now_ms(), line);
    fclose(f);
}

static void load_config(void) {
    if (g_initialized) return;
    g_initialized = 1;
    const char *codec_env = getenv("FAKE_TUTK_CODEC");
    if (codec_env && *codec_env) g_codec = (uint8_t)atoi(codec_env);
    const char *drop_env = getenv("FAKE_TUTK_DROP_AFTER_MS");
    if (drop_env && *drop_env) g_drop_after_ms = atoll(drop_env);
    const char *keep_env = getenv("FAKE_TUTK_KEEP_OPEN");
    if (keep_env && *keep_env) g_keep_open = atoi(keep_env);

    const char *video_path = getenv("FAKE_TUTK_VIDEO");
    if (!video_path || !*video_path) return;
    FILE *f = fopen(video_path, "rb");
    if (!f) return;
    uint32_t cap = 64;
    g_frames = (FakeFrame *)calloc(cap, sizeof(FakeFrame));
    if (!g_frames) {
        fclose(f);
        return;
    }
    uint8_t len_buf[4];
    while (fread(len_buf, 1, 4, f) == 4) {
        uint32_t flen = (uint32_t)len_buf[0] | ((uint32_t)len_buf[1] << 8) |
                        ((uint32_t)len_buf[2] << 16) | ((uint32_t)len_buf[3] << 24);
        if (flen == 0 || flen > 1024u * 1024u) break;
        uint8_t *buf = (uint8_t *)malloc(flen);
        if (!buf || fread(buf, 1, flen, f) != flen) {
            free(buf);
            break;
        }
        if (g_frame_count == cap) {
            cap *= 2;
            FakeFrame *grown = (FakeFrame *)realloc(g_frames, cap * sizeof(FakeFrame));
            if (!grown) {
                free(buf);
                break;
            }
            g_frames = grown;
        }
        g_frames[g_frame_count].len = flen;
        g_frames[g_frame_count].data = buf;
        ++g_frame_count;
    }
    fclose(f);
}

static uint16_t fake_crc_x25(const uint8_t *data, size_t len, uint8_t extra) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        uint8_t tmp = (uint8_t)(data[i] ^ (crc & 0xFFu));
        tmp = (uint8_t)(tmp ^ (tmp << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)tmp << 8) ^ ((uint16_t)tmp << 3) ^ (tmp >> 4));
    }
    uint8_t tmp = (uint8_t)(extra ^ (crc & 0xFFu));
    tmp = (uint8_t)(tmp ^ (tmp << 4));
    crc = (uint16_t)((crc >> 8) ^ ((uint16_t)tmp << 8) ^ ((uint16_t)tmp << 3) ^ (tmp >> 4));
    return crc;
}

int TUTK_SDK_Set_License_Key(const char *lic) {
    (void)lic;
    load_config();
    return 0;
}

int GetLicenseKeyState(void) { return 0; }
int IOTC_Initialize2(uint16_t port) { (void)port; return 0; }
int avInitialize(int max_channels) { (void)max_channels; return 0; }
int IOTC_Get_SessionID(void) { return g_next_sid++; }

int IOTC_Connect_ByUIDEx(const char *uid, int sid, void *in) {
    (void)uid;
    (void)in;
    load_config();
    g_session_start_ms = now_ms();
    g_next_frame_ms = g_session_start_ms;
    g_next_bat_ms = g_session_start_ms;
    g_frame_idx = 0;
    log_line("connect");
    return sid;
}

void IOTC_Session_Close(int sid) {
    (void)sid;
    log_line("close");
}

int avClientStartEx(void *in, void *out) {
    (void)in;
    (void)out;
    return 1;
}

int avRecvFrameData2(int av, char *buf, int buf_len, int *out_frame_sz, int *out_info_sz,
                     char *frame_info, int frame_info_len, int *out_idx, int *out_lost) {
    (void)av;
    int64_t t = now_ms();
    if (g_drop_after_ms > 0 && !g_did_drop && t - g_session_start_ms >= g_drop_after_ms) {
        g_did_drop = 1;
        log_line("drop");
        return -20016;
    }
    if (g_frame_count == 0 || g_frame_idx >= g_frame_count) {
        (void)g_keep_open;
        return -20012;
    }
    if (t < g_next_frame_ms) return -20012;
    FakeFrame *fr = &g_frames[g_frame_idx];
    if ((int)fr->len > buf_len) return -20012;
    memcpy(buf, fr->data, fr->len);
    if (out_frame_sz) *out_frame_sz = (int)fr->len;
    if (out_info_sz) *out_info_sz = 1;
    if (frame_info && frame_info_len > 0) {
        memset(frame_info, 0, (size_t)frame_info_len);
        frame_info[0] = (char)g_codec;
    }
    if (out_idx) *out_idx = (int)g_frame_idx;
    if (out_lost) *out_lost = 0;
    char msg[64];
    snprintf(msg, sizeof(msg), "video %zu", g_frame_idx);
    log_line(msg);
    ++g_frame_idx;
    g_next_frame_ms += 100;
    if (t > g_next_frame_ms) g_next_frame_ms = t + 100;
    return (int)fr->len;
}

int avSendIOCtrl(int av, uint32_t io_type, const char *data, int len) {
    (void)av;
    (void)data;
    (void)len;
    char msg[64];
    snprintf(msg, sizeof(msg), "ioctl %x", (unsigned)io_type);
    log_line(msg);
    return 0;
}

void avClientStop(int av) { (void)av; }
int RDT_Initialize(void) { return 0; }
int RDT_Create(int sid, int timeout, int ch) { (void)sid; (void)timeout; (void)ch; return 1; }
int RDT_Destroy(int rdt) { (void)rdt; return 0; }

int RDT_Write(int rdt, const char *data, uint32_t len) {
    (void)rdt;
    float ly = 0.0f;
    float rx = 0.0f;
    if (len >= 25u && (uint8_t)data[0] == 0xFEu && (uint8_t)data[5] == 202u) {
        float neg_ly = 0.0f;
        memcpy(&neg_ly, data + 10, sizeof(float));
        memcpy(&rx, data + 14, sizeof(float));
        ly = -neg_ly;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "rdt_write %u %.3f %.3f", (unsigned)len, ly, rx);
    log_line(msg);
    return (int)len;
}

int RDT_Read(int rdt, char *buf, uint32_t max_len, uint32_t timeout) {
    (void)rdt;
    (void)timeout;
    const char *bat_env = getenv("FAKE_TUTK_BATTERY");
    if (!bat_env || !*bat_env || max_len < 26u) return 0;
    int64_t t = now_ms();
    if (t < g_next_bat_ms) return 0;
    g_next_bat_ms = t + 100;
    uint8_t pkt[26];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xFEu;
    pkt[1] = 18u;
    pkt[2] = 0u;
    pkt[5] = 207u;
    pkt[22] = 88u;
    pkt[23] = 0u;
    uint16_t crc = fake_crc_x25(pkt + 1, 23, 0u);
    pkt[24] = (uint8_t)(crc & 0xFFu);
    pkt[25] = (uint8_t)((crc >> 8) & 0xFFu);
    memcpy(buf, pkt, sizeof(pkt));
    return (int)sizeof(pkt);
}
