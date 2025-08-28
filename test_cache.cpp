#include "dns_server.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace std;

void test_ttl_lru_hybrid() {
    FastDNSCache cache;
    
    cout << "=== Testing TTL + LRU Hybrid Cache ===" << endl;
    
    // Test 1: Fill cache beyond per-shard limit to trigger LRU eviction
    std::cout << "\n1. Testing LRU eviction when cache is full..." << std::endl;
    
    // Add entries that all go to the same shard (for testing)
    // We'll add more than MAX_ENTRIES_PER_SHARD (8192/16 = 512)
    for (int i = 0; i < 600; ++i) {
        std::string domain = "test" + std::to_string(i) + ".com";
        std::string ip = "192.168.1." + std::to_string((i % 254) + 1);
        cache.set(domain, ip, 300);  // 5 minute TTL
    }
    
    auto stats = cache.get_stats();
    std::cout << "Cache size after adding 600 entries: " << stats.size << std::endl;
    std::cout << "LRU evictions triggered: " << stats.evictions << std::endl;
    
    // Test 2: Check that early entries were evicted by LRU
    std::string ip;
    bool found_early = cache.get("test0.com", ip);
    bool found_late = cache.get("test599.com", ip);
    
    std::cout << "Early entry (test0.com) found: " << (found_early ? "Yes" : "No") << std::endl;
    std::cout << "Late entry (test599.com) found: " << (found_late ? "Yes" : "No") << std::endl;
    
    // Test 3: TTL-based expiration
    std::cout << "\n2. Testing TTL-based expiration..." << std::endl;
    
    // Add entries with short TTL
    cache.set("short-ttl.com", "10.0.0.1", 1);  // 1 second TTL
    cache.set("long-ttl.com", "10.0.0.2", 300); // 5 minute TTL
    
    // Check immediately
    bool short_found_now = cache.get("short-ttl.com", ip);
    bool long_found_now = cache.get("long-ttl.com", ip);
    
    std::cout << "Short TTL entry found immediately: " << (short_found_now ? "Yes" : "No") << std::endl;
    std::cout << "Long TTL entry found immediately: " << (long_found_now ? "Yes" : "No") << std::endl;
    
    // Wait for TTL expiration
    std::cout << "Waiting 2 seconds for TTL expiration..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Check after TTL expiration
    bool short_found_after = cache.get("short-ttl.com", ip);
    bool long_found_after = cache.get("long-ttl.com", ip);
    
    std::cout << "Short TTL entry found after 2s: " << (short_found_after ? "Yes" : "No") << std::endl;
    std::cout << "Long TTL entry found after 2s: " << (long_found_after ? "Yes" : "No") << std::endl;
    
    // Test 4: LRU ordering
    std::cout << "\n3. Testing LRU ordering..." << std::endl;
    
    // Clear and add some test entries
    cache.cleanup_expired();
    
    cache.set("lru1.com", "1.1.1.1", 300);
    cache.set("lru2.com", "2.2.2.2", 300);
    cache.set("lru3.com", "3.3.3.3", 300);
    
    // Access lru1 to make it most recently used
    cache.get("lru1.com", ip);
    
    // Fill cache to trigger eviction
    for (int i = 0; i < 600; ++i) {
        std::string domain = "filler" + std::to_string(i) + ".com";
        cache.set(domain, "192.168.1.1", 300);
    }
    
    // Check which entries survived
    bool lru1_survived = cache.get("lru1.com", ip);
    bool lru2_survived = cache.get("lru2.com", ip);
    bool lru3_survived = cache.get("lru3.com", ip);
    
    std::cout << "lru1.com (accessed recently) survived: " << (lru1_survived ? "Yes" : "No") << std::endl;
    std::cout << "lru2.com (not accessed) survived: " << (lru2_survived ? "Yes" : "No") << std::endl;
    std::cout << "lru3.com (not accessed) survived: " << (lru3_survived ? "Yes" : "No") << std::endl;
    
    // Final stats
    stats = cache.get_stats();
    std::cout << "\n=== Final Cache Statistics ===" << std::endl;
    std::cout << "Total size: " << stats.size << std::endl;
    std::cout << "Total hits: " << stats.hits << std::endl;
    std::cout << "Total misses: " << stats.misses << std::endl;
    std::cout << "Total evictions: " << stats.evictions << std::endl;
    std::cout << "Hit ratio: " << (stats.hit_ratio() * 100) << "%" << std::endl;
}

int main() {
    test_ttl_lru_hybrid();
    return 0;
}
