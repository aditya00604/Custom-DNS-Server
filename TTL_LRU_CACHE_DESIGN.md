# TTL + LRU Hybrid Cache Implementation for DNS Server

## Overview
The DNS server now implements a **TTL + LRU hybrid cache eviction strategy** that combines time-based expiration with least-recently-used eviction for optimal performance and memory management.

## Algorithm Details

### Two-Stage Eviction Strategy

1. **Primary: TTL-based Expiration**
   - Entries are automatically expired when their TTL (Time To Live) expires
   - Expired entries are cleaned up during cache operations
   - Prevents stale DNS records from being served

2. **Secondary: LRU Eviction**
   - When cache reaches maximum capacity, least recently used entries are evicted
   - Maintains cache within memory bounds
   - Preserves frequently accessed domains

### Implementation Features

#### Sharded Cache Design
- **16 shards** for reduced lock contention
- **512 max entries per shard** (8192 total / 16 shards)
- Hash-based domain distribution across shards

#### LRU Tracking
```cpp
struct Shard {
    std::unordered_map<std::string, CacheEntry> entries;
    std::list<std::string> lru_list;  // Most recent at front
    std::unordered_map<std::string, std::list<std::string>::iterator> lru_map;  // O(1) access
    // ... other members
};
```

#### Hybrid Eviction Logic
```cpp
bool FastDNSCache::get(const std::string& domain, std::string& ip) {
    // 1. First cleanup expired entries (TTL-based)
    shard.cleanup_expired();
    
    // 2. Check for valid entry
    if (entry found and valid) {
        // 3. Update LRU position (move to front)
        shard.touch_lru(domain);
        return true;
    }
    // ...
}

void FastDNSCache::set(const std::string& domain, const std::string& ip, uint32_t ttl) {
    // 1. Cleanup expired entries first
    shard.cleanup_expired();
    
    // 2. Check if LRU eviction needed
    shard.evict_lru();
    
    // 3. Add new entry
    shard.entries[domain] = CacheEntry(ip, ttl);
    
    // 4. Update LRU tracking
    shard.touch_lru(domain);
}
```

## Key Methods

### TTL-based Cleanup
```cpp
void cleanup_expired() {
    auto now = std::chrono::steady_clock::now();
    auto it = entries.begin();
    while (it != entries.end()) {
        if (now >= it->second.expiry) {
            // Remove from both cache and LRU tracking
            auto lru_it = lru_map.find(it->first);
            if (lru_it != lru_map.end()) {
                lru_list.erase(lru_it->second);
                lru_map.erase(lru_it);
            }
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
}
```

### LRU Eviction
```cpp
void evict_lru() {
    if (entries.size() >= MAX_ENTRIES_PER_SHARD && !lru_list.empty()) {
        const std::string& oldest = lru_list.back();
        entries.erase(oldest);
        lru_map.erase(oldest);
        lru_list.pop_back();
        evictions.fetch_add(1, std::memory_order_relaxed);
    }
}
```

### LRU Position Update
```cpp
void touch_lru(const std::string& domain) {
    auto lru_it = lru_map.find(domain);
    if (lru_it != lru_map.end()) {
        // Move existing entry to front
        lru_list.erase(lru_it->second);
    }
    lru_list.push_front(domain);
    lru_map[domain] = lru_list.begin();
}
```

## Performance Characteristics

### Time Complexity
- **Cache Get**: O(1) average, O(log n) worst case
- **Cache Set**: O(1) average, O(log n) worst case  
- **LRU Update**: O(1)
- **TTL Cleanup**: O(expired_entries)

### Memory Management
- **Bounded memory usage**: Maximum 8192 entries
- **Automatic cleanup**: Expired entries removed proactively
- **Efficient eviction**: LRU removes least useful entries

### Statistics Tracking
```cpp
struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;  // New: tracks LRU evictions
    size_t size = 0;
    double hit_ratio() const;
};
```

## Benefits

1. **Memory Safety**: Cache size is bounded by LRU eviction
2. **Data Freshness**: TTL ensures stale records are removed
3. **Performance**: Frequently accessed domains stay cached longer
4. **Scalability**: Sharded design reduces lock contention
5. **Observability**: Comprehensive statistics for monitoring

## Usage Example

```cpp
FastDNSCache cache;

// Set entries with different TTLs
cache.set("google.com", "8.8.8.8", 300);      // 5 min TTL
cache.set("temp.com", "1.2.3.4", 60);         // 1 min TTL

// Get entries (updates LRU position)
std::string ip;
if (cache.get("google.com", ip)) {
    // Cache hit - google.com moved to front of LRU
}

// Monitor cache performance
auto stats = cache.get_stats();
std::cout << "Hit ratio: " << stats.hit_ratio() * 100 << "%" << std::endl;
std::cout << "Evictions: " << stats.evictions << std::endl;
```

This implementation provides optimal DNS caching with both time-based and space-based eviction strategies working together for maximum efficiency.
