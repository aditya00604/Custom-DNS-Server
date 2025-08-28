#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <array>
#include <sstream>
#include <list>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>

using namespace std;

struct DNSHeader {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

struct DNSQuestion {
    string qname;
    uint16_t qtype;
    uint16_t qclass;
};

struct DNSAnswer {
    string name;
    uint16_t type;
    uint16_t rclass;
    uint32_t ttl;
    uint16_t rdlength;
    string rdata;
};

struct CacheEntry {
    string ip;
    chrono::steady_clock::time_point expiry;
    atomic<uint32_t> hits{0};
    
    CacheEntry() = default;
    CacheEntry(const string& ip_addr, uint32_t ttl_seconds) 
        : ip(ip_addr), expiry(chrono::steady_clock::now() + chrono::seconds(ttl_seconds)) {}
    
    CacheEntry(const CacheEntry& other) 
        : ip(other.ip), expiry(other.expiry), hits(other.hits.load()) {}
    
    CacheEntry& operator=(const CacheEntry& other) {
        if (this != &other) {
            ip = other.ip;
            expiry = other.expiry;
            hits.store(other.hits.load());
        }
        return *this;
    }
    
    CacheEntry(CacheEntry&& other) noexcept
        : ip(move(other.ip)), expiry(other.expiry), hits(other.hits.load()) {}
    
    CacheEntry& operator=(CacheEntry&& other) noexcept {
        if (this != &other) {
            ip = move(other.ip);
            expiry = other.expiry;
            hits.store(other.hits.load());
        }
        return *this;
    }
    
    bool is_valid() const {
        return chrono::steady_clock::now() < expiry;
    }
};

class FastDNSCache {
private:
    static constexpr size_t CACHE_SIZE = 8192;
    static constexpr size_t CACHE_MASK = CACHE_SIZE - 1;
    static constexpr size_t NUM_SHARDS = 16;
    static constexpr size_t MAX_ENTRIES_PER_SHARD = CACHE_SIZE / NUM_SHARDS;
    
    struct Shard {
        unordered_map<string, CacheEntry> entries;
        list<string> lru_list;
        unordered_map<string, list<string>::iterator> lru_map;
        mutable mutex mtx;
        atomic<uint64_t> hits{0};
        atomic<uint64_t> misses{0};
        atomic<uint64_t> evictions{0};
        
        void cleanup_expired() {
            auto now = chrono::steady_clock::now();
            auto it = entries.begin();
            while (it != entries.end()) {
                if (now >= it->second.expiry) {
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
        
        void evict_lru() {
            if (entries.size() >= MAX_ENTRIES_PER_SHARD && !lru_list.empty()) {
                const string& oldest = lru_list.back();
                entries.erase(oldest);
                lru_map.erase(oldest);
                lru_list.pop_back();
                evictions.fetch_add(1, memory_order_relaxed);
            }
        }
        
        void touch_lru(const string& domain) {
            auto lru_it = lru_map.find(domain);
            if (lru_it != lru_map.end()) {
                lru_list.erase(lru_it->second);
            }
            lru_list.push_front(domain);
            lru_map[domain] = lru_list.begin();
        }
    };
    
    array<Shard, NUM_SHARDS> shards;
    
    size_t get_shard_index(const string& domain) const {
        return hash<string>{}(domain) & (NUM_SHARDS - 1);
    }
    
public:
    bool get(const string& domain, string& ip);
    void set(const string& domain, const string& ip, uint32_t ttl = 300);
    void cleanup_expired();
    
    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        size_t size = 0;
        double hit_ratio() const { return hits + misses > 0 ? static_cast<double>(hits) / (hits + misses) : 0.0; }
    };
    
    Stats get_stats() const;
};

class PrecompiledResponses {
private:
    unordered_map<string, vector<uint8_t>> responses;
    
public:
    void add_local_domain(const string& domain, const string& ip);
    bool get_response(const string& domain, uint16_t query_id, vector<uint8_t>& response);
};

class DNSServer {
private:
    int socket_fd;
    bool running;
    vector<thread> worker_threads;
    FastDNSCache cache;
    PrecompiledResponses precompiled;
    
    vector<pair<string, uint16_t>> upstream_resolvers;
    
    atomic<uint64_t> total_queries{0};
    atomic<uint64_t> cache_hits{0};
    atomic<uint64_t> local_domain_hits{0};
    
    mutable mutex stats_mutex;
    vector<double> response_times;
    
public:
    DNSServer(uint16_t port = 53);
    ~DNSServer();
    
    bool start();
    void stop();
    void add_upstream_resolver(const string& ip, uint16_t port = 53);
    void add_local_domain(const string& domain, const string& ip);
    
    struct PerformanceStats {
        uint64_t total_queries;
        uint64_t cache_hits;
        uint64_t local_domain_hits;
        double cache_hit_ratio;
        double avg_response_time_ms;
        double p95_response_time_ms;
        double p99_response_time_ms;
    };
    
    PerformanceStats get_performance_stats() const;
    
private:
    void worker_thread();
    void handle_query(const uint8_t* data, size_t len, const sockaddr_in& client_addr);
    
    bool parse_dns_header(const uint8_t* data, size_t len, DNSHeader& header);
    bool parse_dns_question(const uint8_t* data, size_t len, size_t& offset, DNSQuestion& question);
    vector<uint8_t> build_dns_response(uint16_t query_id, const string& domain, const string& ip);
    vector<uint8_t> build_error_response(uint16_t query_id, uint16_t rcode = 2);
    
    bool resolve_upstream(const string& domain, string& ip);
    
    string parse_domain_name(const uint8_t* data, size_t len, size_t& offset);
    void encode_domain_name(const string& domain, vector<uint8_t>& buffer);
};

#endif // DNS_SERVER_H
