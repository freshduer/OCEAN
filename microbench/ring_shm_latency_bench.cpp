/*
 * Ring-buffer SHM client RTT benchmark (pairs with cxlmemsim_server --comm-mode shm).
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */
#include "../include/shm_communication.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static uint64_t now_ns() {
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
}

static void usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --shm NAME     (default /cxlmemsim_comm_bench)\n"
                 "  --samples N    (default 20000)\n"
                 "  --warmup N     (default 2000)\n"
                 "  --out FILE.csv (required)\n",
                 prog);
}

int main(int argc, char** argv) {
    const char* shm_name = "/cxlmemsim_comm_bench";
    const char* out_path = nullptr;
    size_t samples = 20000;
    size_t warmup = 2000;

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--shm") && i + 1 < argc) {
            shm_name = argv[++i];
        } else if (!std::strcmp(argv[i], "--samples") && i + 1 < argc) {
            samples = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--warmup") && i + 1 < argc) {
            warmup = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!out_path) {
        usage(argv[0]);
        return 1;
    }

    ShmCommunicationManager mgr(shm_name, false);
    if (!mgr.initialize()) {
        std::fprintf(stderr, "ring-shm: failed to open %s (is server --comm-mode shm running?)\n", shm_name);
        return 1;
    }

    uint32_t client_id = 0;
    if (!mgr.connect(client_id)) {
        std::fprintf(stderr, "ring-shm: connect failed (no free client slot)\n");
        return 1;
    }

    FILE* out = std::fopen(out_path, "w");
    if (!out) {
        std::perror(out_path);
        mgr.disconnect();
        return 1;
    }
    std::fprintf(out, "index,rtt_ns,server_latency_ns,addr\n");

    constexpr uint64_t kCacheline = 64;
    const uint64_t max_addr = 1024ULL * 1024 * 1024 - kCacheline;
    const size_t total = warmup + samples;
    size_t written = 0;

    for (size_t i = 0; i < total; i++) {
        const uint64_t addr =
            static_cast<uint64_t>((i * 9973ULL) % (max_addr / kCacheline)) * kCacheline;

        ShmRequest req{};
        req.op_type = SHM_OP_READ;
        req.addr = addr;
        req.size = kCacheline;
        req.timestamp = now_ns();

        const uint64_t t0 = now_ns();
        if (!mgr.send_request(req)) {
            std::fprintf(stderr, "ring-shm: send_request failed at sample %zu\n", i);
            continue;
        }

        ShmResponse resp{};
        if (!mgr.wait_for_response(resp, 30000)) {
            std::fprintf(stderr, "ring-shm: response timeout at sample %zu\n", i);
            continue;
        }
        const uint64_t t1 = now_ns();

        if (resp.status != 0) {
            std::fprintf(stderr, "ring-shm: server error status=%u at sample %zu\n",
                         static_cast<unsigned>(resp.status), i);
            continue;
        }

        if (i >= warmup) {
            std::fprintf(out, "%zu,%" PRIu64 ",%" PRIu64 ",0x%" PRIx64 "\n", written++, t1 - t0,
                         resp.latency_ns, addr);
        }
    }

    std::fclose(out);
    mgr.disconnect();

    std::printf("ring-shm: wrote %zu samples to %s (client_id=%u)\n", written, out_path, client_id);
    return written == samples ? 0 : 1;
}
