/*
 * One logical 10 MiB read: TCP bulk (10x1MiB) vs PGAS slot (64B x N).
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../include/cxl_backend.h"
#include "../include/cxl_wire_protocol.h"

#define MIB (10u * 1024u * 1024u)
#define CHUNK (1u << 20)

static uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static uint64_t tcp_bulk_10mb(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }

    uint64_t t0 = nsec_now();
    for (int i = 0; i < 10; i++) {
        struct CXLBulkRequestHeader req = {
            .op_type = CXL_OP_BULK_READ, .addr = (uint64_t)i * CHUNK, .size = CHUNK};
        if (send(fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) {
            close(fd);
            return 0;
        }
        struct CXLBulkResponseHeader resp;
        if (recv_all(fd, &resp, sizeof(resp)) < 0 || resp.status != 0) {
            close(fd);
            return 0;
        }
        uint8_t *buf = malloc(CHUNK);
        if (!buf || recv_all(fd, buf, CHUNK) < 0) {
            free(buf);
            close(fd);
            return 0;
        }
        free(buf);
    }
    uint64_t dt = nsec_now() - t0;
    close(fd);
    return dt;
}

static int pgas_bulk_one(cxl_shm_header_t *hdr, uint8_t *pool, uint32_t entry_size, uint64_t addr,
                         size_t size, int is_read, uint8_t *buf) {
    cxl_shm_slot_t *slot = &hdr->slots[0];
    uint32_t req = is_read ? CXL_SHM_REQ_BULK_READ : CXL_SHM_REQ_BULK_WRITE;

    for (int spin = 0; spin < 100000000; spin++) {
        if (__atomic_load_n(&slot->req_type, __ATOMIC_ACQUIRE) == CXL_SHM_REQ_NONE) {
            break;
        }
        cxl_cpu_pause();
    }

    if (!is_read) {
        cxl_pgas_copy_to_pool(pool, entry_size, addr, buf, size, hdr->memory_size);
    }

    slot->addr = addr;
    slot->size = size;
    slot->timestamp = nsec_now();
    __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&slot->req_type, req, __ATOMIC_RELEASE);

    for (int spin = 0; spin < 100000000; spin++) {
        uint32_t st = __atomic_load_n(&slot->resp_status, __ATOMIC_ACQUIRE);
        if (st != CXL_SHM_RESP_NONE) {
            if (st != CXL_SHM_RESP_OK) {
                return 0;
            }
            break;
        }
        cxl_cpu_pause();
    }

    if (is_read) {
        cxl_pgas_copy_from_pool(pool, entry_size, addr, buf, size, hdr->memory_size);
    }

    __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
    return 1;
}

static uint64_t pgas_bulk_10mb(cxl_shm_header_t *hdr, uint8_t *pool, uint32_t entry_size) {
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        return 0;
    }
    uint64_t t0 = nsec_now();
    for (int i = 0; i < 10; i++) {
        if (!pgas_bulk_one(hdr, pool, entry_size, (uint64_t)i * CHUNK, CHUNK, 1, buf)) {
            free(buf);
            return 0;
        }
    }
    uint64_t dt = nsec_now() - t0;
    free(buf);
    return dt;
}

static uint64_t pgas_slot_10mb(cxl_shm_header_t *hdr, int slot_id) {
    cxl_shm_slot_t *slot = &hdr->slots[slot_id];
    const size_t ops = MIB / CXL_SHM_CACHELINE_SIZE;
    uint64_t t0 = nsec_now();

    for (size_t i = 0; i < ops; i++) {
        uint64_t addr = (i * 64) % (hdr->memory_size - 64);

        for (int spin = 0; spin < 100000000; spin++) {
            if (__atomic_load_n(&slot->req_type, __ATOMIC_ACQUIRE) == CXL_SHM_REQ_NONE) break;
            cxl_cpu_pause();
        }

        slot->addr = addr;
        slot->size = CXL_SHM_CACHELINE_SIZE;
        __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_READ, __ATOMIC_RELEASE);

        for (int spin = 0; spin < 100000000; spin++) {
            uint32_t st = __atomic_load_n(&slot->resp_status, __ATOMIC_ACQUIRE);
            if (st != CXL_SHM_RESP_NONE) {
                if (st != CXL_SHM_RESP_OK) return 0;
                break;
            }
            cxl_cpu_pause();
        }

        __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
    }
    return nsec_now() - t0;
}

static uint64_t pgas_mmap_10mb(cxl_shm_header_t *hdr, uint8_t *pool, uint32_t entry_size) {
    uint8_t *buf = malloc(MIB);
    if (!buf) return 0;
    uint64_t t0 = nsec_now();
    for (size_t i = 0; i < MIB / 64; i++) {
        size_t ent = i * entry_size;
        memcpy(buf + i * 64, pool + ent, 64);
    }
    uint64_t dt = nsec_now() - t0;
    free(buf);
    (void)hdr;
    return dt;
}

static void summarize(const char *name, uint64_t *samples, int n) {
    if (n == 0) {
        printf("%s: no samples\n", name);
        return;
    }
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (samples[j] < samples[i]) {
                uint64_t t = samples[i];
                samples[i] = samples[j];
                samples[j] = t;
            }
        }
    }
    double avg = 0;
    for (int i = 0; i < n; i++) avg += samples[i];
    avg /= n;
    double mib_s = (double)MIB / (avg / 1e9) / (1024.0 * 1024.0);
    printf("%s: n=%d p50_ms=%.3f avg_ms=%.3f mib_s=%.1f\n", name, n,
           samples[n / 2] / 1e6, avg / 1e6, mib_s);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "all";
    const char *host = "127.0.0.1";
    int port = 9999;
    const char *shm_name = "/cxlmemsim_pgas_bench10";
    int samples = 8, warmup = 2;

    if (argc > 2) host = argv[2];
    if (argc > 3) port = atoi(argv[3]);
    if (argc > 4) shm_name = argv[4];
    if (argc > 5) samples = atoi(argv[5]);
    if (argc > 6) warmup = atoi(argv[6]);

    uint64_t tcp_samples[32], pgas_bulk_samples[32], pgas_slot_samples[32], mmap_samples[32];
    int nt = 0, npb = 0, nps = 0, nm = 0;
    int run_pgas_slot = (getenv("PGAS_BENCH_SLOT") != NULL);

    if (!strcmp(mode, "tcp") || !strcmp(mode, "all")) {
        for (int w = 0; w < warmup; w++) tcp_bulk_10mb(host, port);
        for (int i = 0; i < samples && i < 32; i++) {
            uint64_t dt = tcp_bulk_10mb(host, port);
            if (dt) tcp_samples[nt++] = dt;
        }
        summarize("tcp_bulk_10mb", tcp_samples, nt);
    }

    if (!strcmp(mode, "pgas") || !strcmp(mode, "all")) {
        char path[256];
        snprintf(path, sizeof(path), "/dev/shm%s", shm_name);
        int fd = open(path, O_RDWR);
        if (fd < 0) {
            perror(path);
            return 1;
        }
        struct stat st;
        fstat(fd, &st);
        void *mapped = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
        cxl_shm_header_t *hdr = (cxl_shm_header_t *)mapped;
        size_t hdr_sz = CXL_SHM_HEADER_SIZE(hdr->num_slots);
        uint8_t *pool = (uint8_t *)mapped + hdr_sz;
        uint32_t ent = hdr->entry_size ? hdr->entry_size : 128;

        for (int i = 0; i < 5000000; i++) {
            if (__atomic_load_n(&hdr->server_ready, __ATOMIC_ACQUIRE)) break;
            cxl_cpu_pause();
        }

        for (int w = 0; w < warmup; w++) pgas_bulk_10mb(hdr, pool, ent);
        for (int i = 0; i < samples && i < 32; i++) {
            uint64_t dt = pgas_bulk_10mb(hdr, pool, ent);
            if (dt) pgas_bulk_samples[npb++] = dt;
        }
        summarize("pgas_bulk_10mb", pgas_bulk_samples, npb);

        if (run_pgas_slot) {
            for (int w = 0; w < warmup; w++) pgas_slot_10mb(hdr, 0);
            for (int i = 0; i < samples && i < 32; i++) {
                uint64_t dt = pgas_slot_10mb(hdr, 0);
                if (dt) pgas_slot_samples[nps++] = dt;
            }
            summarize("pgas_slot_10mb", pgas_slot_samples, nps);
        }

        for (int w = 0; w < warmup; w++) pgas_mmap_10mb(hdr, pool, ent);
        for (int i = 0; i < samples && i < 32; i++) {
            uint64_t dt = pgas_mmap_10mb(hdr, pool, ent);
            if (dt) mmap_samples[nm++] = dt;
        }
        summarize("pgas_mmap_direct_10mb", mmap_samples, nm);

        munmap(mapped, (size_t)st.st_size);
    }

    printf("theory: 10MiB@64Gbps=%.2fms @100Gbps=%.2fms 64B@3us=%.0fms\n",
           (MIB * 8.0 / 64e9) * 1e3, (MIB * 8.0 / 100e9) * 1e3,
           (MIB / 64.0) * 3.0 / 1e3);
    return 0;
}
