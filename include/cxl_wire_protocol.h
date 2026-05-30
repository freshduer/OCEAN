/*
 * CXLMemSim TCP wire protocol (cacheline RPC + bulk transfer).
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */
#ifndef CXL_WIRE_PROTOCOL_H
#define CXL_WIRE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define CXL_OP_READ 0
#define CXL_OP_WRITE 1
#define CXL_OP_GET_SHM_INFO 2
#define CXL_OP_ATOMIC_FAA 3
#define CXL_OP_ATOMIC_CAS 4
#define CXL_OP_FENCE 5
#define CXL_OP_LSA_READ 6
#define CXL_OP_LSA_WRITE 7
#define CXL_OP_BULK_READ 8
#define CXL_OP_BULK_WRITE 9

#define CXL_CACHELINE_SIZE 64
#define CXL_SERVER_REQUEST_SIZE 97
#define CXL_SERVER_RESPONSE_SIZE 81

/* Max payload per bulk RPC (1 MiB). Tune for hundreds of MiB/s on localhost TCP. */
#define CXL_BULK_MAX_SIZE (1u << 20)
#define CXL_BULK_REQ_HDR_SIZE 25
#define CXL_BULK_RESP_HDR_SIZE 9

struct __attribute__((packed)) CXLBulkRequestHeader {
    uint8_t op_type;
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
};

struct __attribute__((packed)) CXLBulkResponseHeader {
    uint8_t status;
    uint64_t latency_ns;
};

#endif /* CXL_WIRE_PROTOCOL_H */
