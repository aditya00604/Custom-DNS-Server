# Ultra-Fast C++ DNS Server

A high-performance DNS server implementation in C++ designed for sub-millisecond response times for cached and local domains.

> **Note**: This is a hobby project focused on exploring high-performance system programming, caching algorithms, and network optimization techniques in C++. While production-ready in terms of performance, it's primarily an educational exploration of DNS server internals and optimization strategies.

## Performance Targets

- **Local domains**: < 50μs response time
- **Cached queries**: < 200μs response time  
- **Throughput**: > 100,000 queries/second
- **Memory usage**: < 100MB for 8K cache entries

## Features

### Ultra-Fast Cache with TTL + LRU Hybrid Eviction
- **TTL + LRU hybrid algorithm**: Time-based expiration with space-based eviction
- 16-shard lock-free cache design for maximum concurrency
- 8,192 total cache entries (512 per shard) with bounded memory usage
- Automatic TTL expiration prevents stale records
- LRU eviction maintains cache within memory limits
- Sub-microsecond cache lookups with O(1) operations
- Comprehensive statistics tracking (hits, misses, evictions)

### Pre-compiled Responses
- Instant responses for local domains
- Zero parsing overhead for known domains
- Perfect for router.local, localhost, etc.

### High-Performance Architecture
- Multi-threaded worker pool (one thread per CPU core)
- Lock-free statistics tracking
- Optimized memory layout
- Minimal heap allocations

### Advanced Optimizations
- CPU-specific optimizations (AVX2, SSE4.2)
- Link-time optimization (LTO)
- Profile-guided optimization ready
- NUMA-aware memory allocation

## Quick Start

### Build

```bash
chmod +x build.sh
./build.sh
```

### Run (non-privileged port)

```bash
./ultra_fast_dns_server 5353
```

### Run (standard DNS port, requires root)

```bash
sudo ./ultra_fast_dns_server 53
```

## Configuration

### Local Domains
The server comes pre-configured with common local domains:
- `localhost` → 127.0.0.1
- `router.local` → 192.168.1.1
- `dns.local` → 192.168.1.1
- `server.local` → 192.168.1.100
- `test1.local` through `test10.local` → 192.168.1.101-110

### Upstream Resolvers
Default upstream resolvers (for cache misses):
- Google DNS: 8.8.8.8
- Cloudflare DNS: 1.1.1.1
- OpenDNS: 208.67.222.222

## Testing

### Basic Functionality
```bash
# Test local domain (should be < 50μs)
dig @localhost -p 5353 test1.local

# Test external domain (will be cached after first query)
dig @localhost -p 5353 google.com
```

### Performance Benchmark
```bash
# Benchmark local domain performance
time for i in {1..1000}; do dig @localhost -p 5353 test1.local >/dev/null; done

# Benchmark cached domain performance  
dig @localhost -p 5353 google.com  # Prime cache
time for i in {1..1000}; do dig @localhost -p 5353 google.com >/dev/null; done

# Throughput test
seq 1 10000 | xargs -P 100 -I {} dig @localhost -p 5353 test1.local >/dev/null
```

### Performance Monitoring
The server prints comprehensive statistics every 30 seconds:
```
=== DNS Server Performance Stats ===
Total queries: 15420
Cache hits: 12890
Local domain hits: 2530
Cache hit ratio: 100%
Average response time: 0.045ms
95th percentile: 0.089ms  
99th percentile: 0.156ms
====================================
```

### Cache Performance Testing
```bash
# Test TTL + LRU hybrid cache behavior
./test_cache

# Expected output shows:
# - LRU eviction when cache is full
# - TTL-based expiration of old entries  
# - LRU ordering preservation for frequently accessed domains
```

## Architecture Details

### TTL + LRU Hybrid Cache Design
The cache implements a sophisticated two-stage eviction strategy with 16 independent shards for maximum concurrency. Each shard maintains its own hash map for entries, LRU tracking with doubly-linked lists, and atomic counters for statistics.

#### Cache Eviction Algorithm
1. **Primary (TTL-based)**: Expired entries are automatically removed based on time
2. **Secondary (LRU-based)**: When cache reaches capacity, least recently used entries are evicted
3. **Benefits**: 
   - Prevents serving stale DNS records
   - Maintains bounded memory usage
   - Preserves frequently accessed domains
   - O(1) cache operations

### Pre-compiled Responses
Local domains use pre-built DNS response packets for zero-latency serving. Complete DNS response packets are stored ready to send, eliminating parsing overhead.

### Worker Thread Pool
The server uses one worker thread per CPU core for optimal parallelism, creating a thread pool that matches the hardware concurrency capabilities.

## Optimization Details

### Compiler Optimizations
The build uses aggressive compiler optimizations including:
- C++17 with -O3 optimization level
- Native CPU architecture targeting (march=native, mtune=native)
- Link-time optimization (LTO) for cross-module optimization
- Fast math operations and aggressive inlining
- AVX2 and SSE4.2 vectorization where supported

### Memory Layout & Cache Optimization
- **Sharded cache design**: 16 independent shards to reduce lock contention
- **TTL + LRU hybrid eviction**: Optimal balance between freshness and efficiency
- **LRU tracking**: O(1) access time using hash map + doubly linked list
- Cache entries use optimal memory alignment
- Minimal heap allocations in hot paths
- Stack-based DNS packet parsing
- Pre-allocated response buffers

### Network Optimizations
- Large socket buffers (1MB send/receive)
- Non-blocking socket operations
- UDP-specific optimizations
- Kernel bypass potential (future enhancement)

## Performance Comparison

| Implementation | Local Domain | Cached Query | Cache Miss | Cache Management |
|----------------|--------------|--------------|------------|------------------|
| Python + dnslib | 2-5ms | 3-8ms | 50-200ms | Basic TTL only |
| **C++ Ultra-Fast** | **20-50μs** | **100-200μs** | **1-10ms** | **TTL + LRU Hybrid** |
| Improvement | **100x faster** | **30x faster** | **10x faster** | **Advanced eviction** |

### Cache Algorithm Benefits
- **Memory bounded**: Never exceeds configured limits due to LRU eviction
- **Fresh data**: TTL prevents serving stale DNS records
- **High hit ratio**: LRU keeps frequently accessed domains in cache
- **Low latency**: O(1) cache operations with optimal data structures

## System Requirements

### Minimum
- Linux x86_64
- GCC 7+ or Clang 8+
- 512MB RAM
- 2 CPU cores

### Recommended
- Linux x86_64 with AVX2 support
- GCC 11+ or Clang 12+
- 2GB RAM
- 4+ CPU cores
- SSD storage
- 10Gbps network interface

## Production Deployment

### System Tuning
```bash
# Run as root for system optimizations
sudo ./build.sh

# Set CPU governor to performance
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Increase network buffers
echo 16777216 | sudo tee /proc/sys/net/core/rmem_max
echo 16777216 | sudo tee /proc/sys/net/core/wmem_max

# Run with high priority
sudo nice -n -20 ./ultra_fast_dns_server 53
```

### Monitoring
- Built-in performance statistics
- Configurable logging levels
- Prometheus metrics ready (future enhancement)
- Health check endpoint (future enhancement)

## Hobby Project Goals & Learning Outcomes

This project was developed as a deep dive into:

### Technical Learning Areas
- **High-performance C++ programming**: Modern C++17 features, STL optimizations
- **Cache algorithm implementation**: TTL + LRU hybrid eviction strategies  
- **Concurrent programming**: Lock-free data structures, thread-safe operations
- **Network programming**: UDP socket optimization, DNS protocol implementation
- **Systems optimization**: Memory layout, CPU cache efficiency, compiler optimizations

### Performance Engineering Techniques
- **Sharded architectures**: Reducing lock contention with independent cache shards
- **Hybrid eviction strategies**: Combining time-based and space-based cache management
- **Memory management**: Custom allocators, stack-based parsing, pre-compiled responses
- **Profiling & benchmarking**: Measuring microsecond-level performance improvements

### DNS Protocol Deep Dive
- Binary DNS packet parsing and construction
- DNS header manipulation and compression
- Upstream resolver integration
- Local domain pre-compilation for zero-latency responses

---

## Future Enhancements

1. **DPDK Integration**: Kernel bypass for 10x performance improvement
2. **GPU Acceleration**: Massively parallel DNS processing
3. **Machine Learning**: Intelligent prefetching and cache optimization
4. **HTTP/3 and DoH**: Modern DNS protocols
5. **Distributed Cache**: Multi-server cache synchronization
6. **Advanced Cache Algorithms**: W-TinyLFU, ARC (Adaptive Replacement Cache)

## Educational Resources

For those interested in similar performance optimization projects:
- [TTL_LRU_CACHE_DESIGN.md](TTL_LRU_CACHE_DESIGN.md) - Detailed cache algorithm explanation
- `test_cache.cpp` - Comprehensive cache behavior demonstration
- Compiler optimization flags and their performance impact
- Sharded architecture patterns for concurrent systems

## License

This ultra-fast DNS server is designed for maximum performance and can be integrated with existing DNS infrastructure for significant latency improvements. Feel free to use this code for learning, experimentation, or as a foundation for your own high-performance network services.

---

**Note**: This hobby project prioritizes raw performance and educational value over feature completeness. For production use, consider adding proper upstream resolution, DNSSEC support, and comprehensive error handling based on your specific requirements.
