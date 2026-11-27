/*
 * Direct 64-bit / 32-bit portable Kalay/TUTK P2P + MAVLink-over-RDT shim for Ori (EBO SE).
 *
 * Video-only (no audio buffer) for lowest latency:
 *   stdout: [u32 len LE][u8 kind][payload]
 *     kind = 80 (HEVC), 78 (H.264), 0xFF (inbound RDT MAVLink status bytes)
 *   control input (ORI_CONTROL_FD, default 0/stdin): [u32 len LE][u8 kind][payload]
 *     kind = 0 (RDT_Write MAVLink), 1 (avSendIOCtrl: [u16 io_type LE][data])
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint32_t cb;
    uint32_t authentication_type;
    char auth_key[136];
    uint32_t timeout;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
} IOTCConnectInput;

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
} AVClientStartInConfig;

typedef struct {
    uint32_t cb;
    uint32_t server_type;
    int32_t resend;
    uint32_t security_mode;
    uint32_t auth_type;
    uint32_t reserved[3];
} AVClientStartOutConfig;

_Static_assert(sizeof(IOTCConnectInput) == 160, "IOTCConnectInput must be 160 bytes");
_Static_assert(sizeof(AVClientStartOutConfig) == 32, "AVClientStartOutConfig must be 32 bytes");
#if defined(__LP64__) || defined(_WIN64)
_Static_assert(sizeof(AVClientStartInConfig) == 64, "AVClientStartInConfig must be 64 bytes on 64-bit");
#else
_Static_assert(sizeof(AVClientStartInConfig) == 48, "AVClientStartInConfig must be 48 bytes on 32-bit");
#endif

typedef int (*fn_set_license)(const char *);
typedef int (*fn_lic_state)(void);
typedef int (*fn_iotc_init2)(uint16_t);
typedef int (*fn_av_init)(int);
typedef int (*fn_iotc_get_sid)(void);
typedef int (*fn_iotc_connect_ex)(const char *, int, void *);
typedef void (*fn_iotc_close)(int);
typedef int (*fn_av_start_ex)(void *, void *);
typedef int (*fn_av_recv2)(int, char *, int, int *, int *, char *, int, int *, int *);
typedef int (*fn_av_send_io)(int, uint32_t, const char *, int);
typedef void (*fn_av_stop)(int);
typedef int (*fn_rdt_init)(void);
typedef int (*fn_rdt_create)(int, int, int);
typedef int (*fn_rdt_destroy)(int);
typedef int (*fn_rdt_write)(int, const char *, uint32_t);
typedef int (*fn_rdt_read)(int, char *, uint32_t, uint32_t);

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void *load_lib(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[kalay] dlopen %s failed: %s\n", path, dlerror());
        exit(2);
    }
    return h;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static uint16_t crc_x25(const uint8_t *data, size_t len, uint8_t extra) {
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

static void build_zero_motor_frame(uint8_t out[25], uint8_t seq) {
    out[0] = 0xFEu;
    out[1] = 17u;
    out[2] = seq;
    out[3] = 0u;
    out[4] = 0u;
    out[5] = 202u;
    memset(out + 6, 0, 17);
    uint16_t crc = crc_x25(out + 1, 22, 211u);
    out[23] = (uint8_t)(crc & 0xFFu);
    out[24] = (uint8_t)((crc >> 8) & 0xFFu);
}

static int emit_packet(uint8_t kind, const void *payload, uint32_t len) {
    uint8_t hdr[5];
    hdr[0] = (uint8_t)(len & 0xFFu);
    hdr[1] = (uint8_t)((len >> 8) & 0xFFu);
    hdr[2] = (uint8_t)((len >> 16) & 0xFFu);
    hdr[3] = (uint8_t)((len >> 24) & 0xFFu);
    hdr[4] = kind;
    if (write_all(STDOUT_FILENO, hdr, sizeof(hdr)) < 0) return -1;
    if (len > 0 && write_all(STDOUT_FILENO, payload, len) < 0) return -1;
    return 0;
}

static int emit_shim_status(uint8_t event, uint8_t rdt_ok, uint16_t cutoffs) {
    uint8_t p[4] = {event, rdt_ok, (uint8_t)(cutoffs & 0xFFu), (uint8_t)((cutoffs >> 8) & 0xFFu)};
    return emit_packet(0xFDu, p, (uint32_t)sizeof(p));
}

int main(void) {
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, handle_signal);

    const char *lib_dir = getenv("EBO_LIB_DIR");
    if (!lib_dir) lib_dir = "/opt/ori/lib";
    const char *lic = getenv("EBO_LICENSE");
    const char *uid = getenv("EBO_UID");
    const char *authkey = getenv("EBO_AUTHKEY");
    const char *identity = getenv("EBO_IDENTITY");
    const char *token = getenv("EBO_TOKEN");
    const char *ioctl_path = getenv("EBO_IOCTL9930");
    if (!lic || !uid || !authkey || !identity || !token) {
        fprintf(stderr, "[kalay] missing EBO_LICENSE/EBO_UID/EBO_AUTHKEY/EBO_IDENTITY/EBO_TOKEN\n");
        return 1;
    }

    int ctrl_fd = STDIN_FILENO;
    const char *ctrl_env = getenv("ORI_CONTROL_FD");
    if (ctrl_env && *ctrl_env) ctrl_fd = atoi(ctrl_env);
    int flags = fcntl(ctrl_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(ctrl_fd, F_SETFL, flags | O_NONBLOCK);

    load_lib(lib_dir, "libTUTKGlobalAPIs.so");
    load_lib(lib_dir, "libIOTCAPIs.so");
    load_lib(lib_dir, "libRDTAPIs.so");
    load_lib(lib_dir, "libAVAPIs.so");

    fn_set_license set_license = (fn_set_license)dlsym(RTLD_DEFAULT, "TUTK_SDK_Set_License_Key");
    fn_lic_state lic_state = (fn_lic_state)dlsym(RTLD_DEFAULT, "GetLicenseKeyState");
    fn_iotc_init2 iotc_init2 = (fn_iotc_init2)dlsym(RTLD_DEFAULT, "IOTC_Initialize2");
    fn_av_init av_init = (fn_av_init)dlsym(RTLD_DEFAULT, "avInitialize");
    fn_iotc_get_sid iotc_get_sid = (fn_iotc_get_sid)dlsym(RTLD_DEFAULT, "IOTC_Get_SessionID");
    fn_iotc_connect_ex iotc_connect_ex = (fn_iotc_connect_ex)dlsym(RTLD_DEFAULT, "IOTC_Connect_ByUIDEx");
    fn_iotc_close iotc_close = (fn_iotc_close)dlsym(RTLD_DEFAULT, "IOTC_Session_Close");
    fn_av_start_ex av_start_ex = (fn_av_start_ex)dlsym(RTLD_DEFAULT, "avClientStartEx");
    fn_av_recv2 av_recv2 = (fn_av_recv2)dlsym(RTLD_DEFAULT, "avRecvFrameData2");
    fn_av_send_io av_send_io = (fn_av_send_io)dlsym(RTLD_DEFAULT, "avSendIOCtrl");
    fn_av_stop av_stop = (fn_av_stop)dlsym(RTLD_DEFAULT, "avClientStop");
    fn_rdt_init rdt_init = (fn_rdt_init)dlsym(RTLD_DEFAULT, "RDT_Initialize");
    fn_rdt_create rdt_create = (fn_rdt_create)dlsym(RTLD_DEFAULT, "RDT_Create");
    fn_rdt_destroy rdt_destroy = (fn_rdt_destroy)dlsym(RTLD_DEFAULT, "RDT_Destroy");
    fn_rdt_write rdt_write = (fn_rdt_write)dlsym(RTLD_DEFAULT, "RDT_Write");
    fn_rdt_read rdt_read = (fn_rdt_read)dlsym(RTLD_DEFAULT, "RDT_Read");

    if (!set_license || !iotc_init2 || !av_init || !iotc_get_sid ||
        !iotc_connect_ex || !av_start_ex || !av_recv2 || !av_send_io || !av_stop) {
        fprintf(stderr, "[kalay] required TUTK symbols missing\n");
        return 3;
    }

    uint8_t d9930[4096];
    int l9930 = 0;
    if (ioctl_path && *ioctl_path) {
        FILE *f = fopen(ioctl_path, "rb");
        if (f) {
            l9930 = (int)fread(d9930, 1, sizeof(d9930), f);
            fclose(f);
        }
    }

    if (set_license(lic) != 0) {
        fprintf(stderr, "[kalay] license rejected\n");
        return 4;
    }
    iotc_init2(0);
    av_init(64);
    if (lic_state) {
        for (int k = 0; k < 10 && lic_state() != 0; ++k) sleep_ms(1000);
    }
    if (rdt_init) {
        int ri = rdt_init();
        fprintf(stderr, "[kalay] RDT_Initialize=%d\n", ri);
    }

    char *vbuf = (char *)malloc(1024u * 1024u);
    if (!vbuf) return 5;
    uint8_t cbuf[4096];
    int cpos = 0;
    uint16_t cutoff_count = 0;

    while (g_running) {
        int slot = iotc_get_sid();
        IOTCConnectInput cin;
        memset(&cin, 0, sizeof(cin));
        cin.cb = (uint32_t)sizeof(cin);
        snprintf(cin.auth_key, sizeof(cin.auth_key), "%s", authkey);
        cin.timeout = 15;
        int sid = iotc_connect_ex(uid, slot, &cin);
        if (sid < 0) {
            fprintf(stderr, "[kalay] IOTC connect %d, retrying...\n", sid);
            sleep_ms(2000);
            continue;
        }

        AVClientStartInConfig inc;
        AVClientStartOutConfig out;
        memset(&inc, 0, sizeof(inc));
        memset(&out, 0, sizeof(out));
        inc.cb = (uint32_t)sizeof(inc);
        inc.iotc_session_id = sid;
        inc.timeout_sec = 10;
        inc.account_or_identity = identity;
        inc.password_or_token = token;
        inc.resend = 1;
        inc.security_mode = 2;
        inc.auth_type = 1;
        out.cb = (uint32_t)sizeof(out);

        int av = av_start_ex(&inc, &out);
        if (av < 0) {
            fprintf(stderr, "[kalay] avClientStartEx %d, retrying...\n", av);
            if (iotc_close) iotc_close(sid);
            sleep_ms(2000);
            continue;
        }

        int rdt = rdt_create ? rdt_create(sid, 5000, 1) : -1;
        fprintf(stderr, "[kalay] connected sid=%d av=%d rdt=%d\n", sid, av, rdt);
        int alive = 1;
        int motor_active = 0;
        int64_t motor_deadline_ms = 0;
        uint8_t motor_seq = 0;
        if (emit_packet(0xFEu, NULL, 0) < 0 ||
            emit_shim_status(1u, rdt >= 0 ? 1u : 0u, cutoff_count) < 0) {
            alive = 0;
            g_running = 0;
        }

        char z8[8] = {0};
        av_send_io(av, 0xFFu, z8, 4);
        if (l9930 > 0) av_send_io(av, 0x9930u, (const char *)d9930, l9930);
        av_send_io(av, 0x32Au, z8, 8);
        av_send_io(av, 0x1FFu, z8, 8);
        av_send_io(av, 0x9936u, z8, 8);

        time_t last_ka = time(NULL);
        char fi[64];
        char rbuf[4096];

        while (alive && g_running) {
            int fa = 0, fb = 0, fc = 0, fd2 = 0;
            int n = av_recv2(av, vbuf, 1024 * 1024, &fa, &fb, fi, (int)sizeof(fi), &fc, &fd2);
            if (n > 0) {
                if (emit_packet((uint8_t)fi[0], vbuf, (uint32_t)n) < 0) {
                    alive = 0;
                    break;
                }
            } else if (n == -20010 || (n <= -20015 && n >= -20020)) {
                /* TUTK AV_ER session loss: -20010 INVALID_SID, -20015..-20020 CLOSE/TIMEOUT/EXIT. */
                fprintf(stderr, "[kalay] session lost (%d)\n", n);
                alive = 0;
                break;
            } else {
                if (n < 0 && n != -20012) {
                    static time_t last_err_s = 0;
                    time_t err_s = time(NULL);
                    if (err_s != last_err_s) {
                        last_err_s = err_s;
                        fprintf(stderr, "[kalay] avRecvFrameData2 error %d\n", n);
                    }
                }
                sleep_ms(2);
            }

            if (rdt >= 0 && rdt_read) {
                int rn = rdt_read(rdt, rbuf, (uint32_t)sizeof(rbuf), 0);
                if (rn > 0 && emit_packet(0xFFu, rbuf, (uint32_t)rn) < 0) {
                    alive = 0;
                    break;
                }
            }

            ssize_t cr = read(ctrl_fd, cbuf + cpos, sizeof(cbuf) - (size_t)cpos);
            if (cr == 0) {
                fprintf(stderr, "[kalay] control EOF, exiting\n");
                alive = 0;
                g_running = 0;
                break;
            }
            if (cr > 0) cpos += (int)cr;
            int off = 0;
            while (cpos - off >= 5) {
                uint32_t plen = (uint32_t)cbuf[off] | ((uint32_t)cbuf[off + 1] << 8) |
                                ((uint32_t)cbuf[off + 2] << 16) | ((uint32_t)cbuf[off + 3] << 24);
                if (plen == 0 || plen > sizeof(cbuf) - 4) {
                    off = cpos;
                    break;
                }
                if (cpos - off < (int)(4 + plen)) break;
                uint8_t kind = cbuf[off + 4];
                const char *payload = (const char *)(cbuf + off + 5);
                uint32_t paylen = plen - 1;
                if (kind == 0 && rdt >= 0 && rdt_write) {
                    rdt_write(rdt, payload, paylen);
                    if (paylen >= 25u && (uint8_t)payload[0] == 0xFEu && (uint8_t)payload[5] == 202u) {
                        float axes[4];
                        memcpy(axes, payload + 6, sizeof(axes));
                        int moving = 0;
                        for (int k = 0; k < 4; ++k) {
                            if (axes[k] > 1e-4f || axes[k] < -1e-4f) {
                                moving = 1;
                                break;
                            }
                        }
                        if (moving) {
                            motor_active = 1;
                            motor_seq = (uint8_t)((uint8_t)payload[2] + 1u);
                            motor_deadline_ms = monotonic_ms() + 300;
                        } else if (motor_active) {
                            motor_active = 0;
                            if (emit_shim_status(4u, 1u, cutoff_count) < 0) {
                                alive = 0;
                                g_running = 0;
                                break;
                            }
                        }
                    }
                } else if (kind == 1 && paylen >= 2) {
                    uint16_t iotype = (uint16_t)((uint8_t)payload[0] | ((uint16_t)(uint8_t)payload[1] << 8));
                    av_send_io(av, iotype, payload + 2, (int)(paylen - 2));
                }
                off += (int)(4 + plen);
            }
            if (off > 0) {
                memmove(cbuf, cbuf + off, (size_t)(cpos - off));
                cpos -= off;
            }
            if (!alive) break;

            if (motor_active && rdt >= 0 && rdt_write && monotonic_ms() >= motor_deadline_ms) {
                uint8_t stop_pkt[25];
                for (int k = 0; k < 2; ++k) {
                    build_zero_motor_frame(stop_pkt, motor_seq++);
                    rdt_write(rdt, (const char *)stop_pkt, (uint32_t)sizeof(stop_pkt));
                }
                motor_active = 0;
                ++cutoff_count;
                if (emit_shim_status(3u, 1u, cutoff_count) < 0) {
                    alive = 0;
                    break;
                }
            }

            time_t now_s = time(NULL);
            if (now_s - last_ka >= 10) {
                if (emit_shim_status(2u, rdt >= 0 ? 1u : 0u, cutoff_count) < 0) {
                    alive = 0;
                    break;
                }
                last_ka = now_s;
            }
        }

        if (motor_active && rdt >= 0 && rdt_write) {
            uint8_t stop_pkt[25];
            for (int k = 0; k < 2; ++k) {
                build_zero_motor_frame(stop_pkt, motor_seq++);
                rdt_write(rdt, (const char *)stop_pkt, (uint32_t)sizeof(stop_pkt));
            }
            motor_active = 0;
        }
        if (rdt >= 0 && rdt_destroy) rdt_destroy(rdt);
        av_stop(av);
        if (iotc_close) iotc_close(sid);
    }

    free(vbuf);
    return 0;
}
