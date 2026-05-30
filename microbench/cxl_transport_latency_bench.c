#define _POSIX_C_SOURCE 200809L
/*
 * Measure client round-trip latency for CXLMemSim transports:
 *   tcp      - socket RPC (same path as QEMU CXL_TRANSPORT_MODE=tcp)
 *   pgas-shm - PGAS shared-memory slots (QEMU CXL_TRANSPORT_MODE=shm)
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <unistd.h>

#include "../include/cxl_backend.h"

#define OP_READ 0
#define CACHELINE 64

struct __attribute__((packed)) bench_tcp_request {
    uint8_t op_type;
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint64_t value;
    uint64_t expected;
    uint8_t data[CACHELINE];
};

struct __attribute__((packed)) bench_tcp_response {
    uint8_t status;
    uint64_t latency_ns;
    uint64_t old_value;
    uint8_t data[CACHELINE];
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

static int tcp_read_sample(int fd, uint64_t addr, uint64_t *rtt_ns, uint64_t *server_lat_ns) {
    struct bench_tcp_request req = {
        .op_type = OP_READ,
        .addr = addr,
        .size = CACHELINE,
        .timestamp = now_ns(),
    };
    struct bench_tcp_response tcp_resp;

    uint64_t t0 = now_ns();
    if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) != (ssize_t)sizeof(req)) {
        return -1;
    }
    if (recv(fd, &tcp_resp, sizeof(tcp_resp), MSG_WAITALL) != (ssize_t)sizeof(tcp_resp)) {
        return -1;
    }
    uint64_t t1 = now_ns();

    if (tcp_resp.status != 0) {
        return -1;
    }
    *rtt_ns = t1 - t0;
    *server_lat_ns = tcp_resp.latency_ns;
    return 0;
}

typedef struct {
    cxl_shm_header_t *hdr;
    size_t map_size;
} pgas_ctx_t;

static int pgas_open(const char *shm_name, pgas_ctx_t *ctx) {
    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return -1;
    }

    void *mapped = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    ctx->hdr = (cxl_shm_header_t *)mapped;
    ctx->map_size = (size_t)st.st_size;

    for (int i = 0; i < 5000000; i++) {
        if (__atomic_load_n(&ctx->hdr->server_ready, __ATOMIC_ACQUIRE) != 0) {
            return 0;
        }
        cxl_cpu_pause();
    }
    fprintf(stderr, "timeout waiting for PGAS server_ready\n");
    return -1;
}

static int pgas_read_sample(pgas_ctx_t *ctx, uint64_t addr, uint64_t *rtt_ns, uint64_t *server_lat_ns) {
    cxl_shm_slot_t *slot = &ctx->hdr->slots[0];

    for (int i = 0; i < 10000000; i++) {
        if (__atomic_load_n(&slot->req_type, __ATOMIC_ACQUIRE) == CXL_SHM_REQ_NONE) {
            break;
        }
        cxl_cpu_pause();
        if (i == 9999999) {
            return -1;
        }
    }

    slot->addr = addr;
    slot->size = CACHELINE;
    slot->timestamp = now_ns();
    slot->value = 0;
    slot->expected = 0;
    __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    uint64_t t0 = now_ns();
    __atomic_store_n(&slot->req_type, CXL_SHM_REQ_READ, __ATOMIC_RELEASE);

    for (int i = 0; i < 10000000; i++) {
        uint32_t st = __atomic_load_n(&slot->resp_status, __ATOMIC_ACQUIRE);
        if (st != CXL_SHM_RESP_NONE) {
            uint64_t t1 = now_ns();
            if (st != CXL_SHM_RESP_OK) {
                __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
                __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
                return -1;
            }
            *rtt_ns = t1 - t0;
            *server_lat_ns = slot->latency_ns;
            __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
            __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
            return 0;
        }
        cxl_cpu_pause();
    }
    __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
    return -1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --mode tcp|pgas-shm [options]\n"
            "  --host HOST       (tcp, default 127.0.0.1)\n"
            "  --port PORT       (tcp, default 9999)\n"
            "  --shm NAME        (pgas-shm, default /cxlmemsim_pgas_latency)\n"
            "  --samples N       (default 20000)\n"
            "  --warmup N        (default 2000)\n"
            "  --out FILE.csv    (required)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *mode = NULL;
    const char *host = "127.0.0.1";
    int port = 9999;
    const char *shm_name = "/cxlmemsim_pgas_latency";
    const char *out_path = NULL;
    size_t samples = 20000;
    size_t warmup = 2000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            mode = argv[++i];
        } else if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--shm") && i + 1 < argc) {
            shm_name = argv[++i];
        } else if (!strcmp(argv[i], "--samples") && i + 1 < argc) {
            samples = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) {
            warmup = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!mode || !out_path) {
        usage(argv[0]);
        return 1;
    }

    FILE *out = fopen(out_path, "w");
    if (!out) {
        perror(out_path);
        return 1;
    }
    fprintf(out, "index,rtt_ns,server_latency_ns,addr\n");

    int tcp_fd = -1;
    pgas_ctx_t pgas = {0};

    if (!strcmp(mode, "tcp")) {
        tcp_fd = tcp_connect(host, port);
        if (tcp_fd < 0) {
            return 1;
        }
    } else if (!strcmp(mode, "pgas-shm")) {
        if (pgas_open(shm_name, &pgas) < 0) {
            return 1;
        }
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 1;
    }

    uint64_t max_addr = 1024ULL * 1024 * 1024 - CACHELINE;
    size_t total = warmup + samples;
    size_t written = 0;

    for (size_t i = 0; i < total; i++) {
        uint64_t addr = (uint64_t)((i * 9973ULL) % (max_addr / CACHELINE)) * CACHELINE;
        uint64_t rtt_ns = 0, server_lat_ns = 0;
        int rc;

        if (!strcmp(mode, "tcp")) {
            rc = tcp_read_sample(tcp_fd, addr, &rtt_ns, &server_lat_ns);
        } else {
            rc = pgas_read_sample(&pgas, addr, &rtt_ns, &server_lat_ns);
        }

        if (rc < 0) {
            fprintf(stderr, "sample %" PRIu64 " failed\n", (uint64_t)i);
            continue;
        }

        if (i >= warmup) {
            fprintf(out, "%zu,%" PRIu64 ",%" PRIu64 ",0x%" PRIx64 "\n",
                    written++, rtt_ns, server_lat_ns, addr);
        }
    }

    fclose(out);
    if (tcp_fd >= 0) {
        close(tcp_fd);
    }
    if (pgas.hdr && pgas.map_size) {
        munmap(pgas.hdr, pgas.map_size);
    }

    printf("%s: wrote %zu samples to %s\n", mode, written, out_path);
    return written == samples ? 0 : 1;
}
