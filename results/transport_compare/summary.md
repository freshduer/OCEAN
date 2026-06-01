# CXLMemSim transport latency comparison

Cacheline READ RTT (client-measured, includes server simulated latency in response).

```
transport        n  mean_us  p50_us  p90_us  p99_us  min_us  max_us
      TCP 20000.00    39.31   36.46   49.03   59.71   27.43  213.58
 PGAS-SHM 20000.00     0.49    0.43    0.65    1.02    0.35   12.79
 Ring-SHM 20000.00    25.36   26.02   30.37   36.86   13.60  649.11
```

## Relative to TCP p50

- **TCP**: p50 = 36.46 µs (1.00× TCP)
- **PGAS-SHM**: p50 = 0.43 µs (0.01× TCP)
- **Ring-SHM**: p50 = 26.02 µs (0.71× TCP)
