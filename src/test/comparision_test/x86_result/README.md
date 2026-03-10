# Test Environment - x86 Platform

## CPU Information

| Item | Specification |
|------|---------------|
| **Model** | 13th Gen Intel(R) Core(TM) i9-13900K |
| **Architecture** | x86_64 |
| **Physical Cores** | 16 cores |
| **Logical Threads** | 32 threads (2 threads/core) |
| **Sockets** | 1 |
| **Base Frequency** | 2995 MHz (~3.0 GHz) |
| **Address Size** | 46 bits physical, 48 bits virtual |

## CPU Cache

| Cache Level | Size | Instances |
|-------------|------|-----------|
| **L1 Data Cache** | 48 KiB per core | 16 (768 KiB total) |
| **L1 Instruction Cache** | 32 KiB per core | 16 (512 KiB total) |
| **L2 Cache** | 2 MiB per core | 16 (32 MiB total) |
| **L3 Cache** | 36 MiB | 1 (shared) |

## Memory

| Item | Size |
|------|------|
| **Total Memory** | 62 GiB (65,716,316 KB) |

## Trade Off:
1. more memory usage (larger node): rbtree is about 30% (1M datas, 4 bytes each value) more than skiplist. but as the value size increases, the disadvantage of node size would decreases.
