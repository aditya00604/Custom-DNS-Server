#include "dns_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <chrono>

using namespace std;

bool FastDNSCache::get(const string& domain, string& ip) {
    auto& shard = shards[get_shard_index(domain)];
    lock_guard<mutex> lock(shard.mtx);
    
    shard.cleanup_expired();
    
    auto it = shard.entries.find(domain);
    if (it != shard.entries.end() && it->second.is_valid()) {
        ip = it->second.ip;
        it->second.hits.fetch_add(1, memory_order_relaxed);
        shard.hits.fetch_add(1, memory_order_relaxed);
        
        shard.touch_lru(domain);
        return true;
    }
    
    if (it != shard.entries.end()) {
        auto lru_it = shard.lru_map.find(it->first);
        if (lru_it != shard.lru_map.end()) {
            shard.lru_list.erase(lru_it->second);
            shard.lru_map.erase(lru_it);
        }
        shard.entries.erase(it);
    }
    
    shard.misses.fetch_add(1, memory_order_relaxed);
    return false;
}

void FastDNSCache::set(const string& domain, const string& ip, uint32_t ttl) {
    auto& shard = shards[get_shard_index(domain)];
    lock_guard<mutex> lock(shard.mtx);
    
    shard.cleanup_expired();
    
    shard.evict_lru();
    
    shard.entries[domain] = CacheEntry(ip, ttl);
    
    shard.touch_lru(domain);
}

void FastDNSCache::cleanup_expired() {
    for (auto& shard : shards) {
        lock_guard<mutex> lock(shard.mtx);
        shard.cleanup_expired();
    }
}

FastDNSCache::Stats FastDNSCache::get_stats() const {
    Stats stats;
    
    for (const auto& shard : shards) {
        lock_guard<mutex> lock(shard.mtx);
        stats.hits += shard.hits.load(memory_order_relaxed);
        stats.misses += shard.misses.load(memory_order_relaxed);
        stats.evictions += shard.evictions.load(memory_order_relaxed);
        stats.size += shard.entries.size();
    }
    
    return stats;
}

void PrecompiledResponses::add_local_domain(const string& domain, const string& ip) {
    vector<uint8_t> response;
    
    response.resize(12);
    uint16_t* header = reinterpret_cast<uint16_t*>(response.data());
    header[0] = 0;
    header[1] = htons(0x8180);
    header[2] = htons(1);
    header[3] = htons(1);
    header[4] = htons(0);
    header[5] = htons(0);
    
    istringstream iss(domain);
    string label;
    while (getline(iss, label, '.')) {
        response.push_back(static_cast<uint8_t>(label.length()));
        response.insert(response.end(), label.begin(), label.end());
    }
    response.push_back(0);
    
    uint16_t qtype = htons(1);
    uint16_t qclass = htons(1);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&qtype), reinterpret_cast<uint8_t*>(&qtype) + 2);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&qclass), reinterpret_cast<uint8_t*>(&qclass) + 2);
    
    response.push_back(0xC0);
    response.push_back(0x0C);
    
    uint16_t atype = htons(1);
    uint16_t aclass = htons(1);
    uint32_t ttl = htonl(300);
    uint16_t rdlength = htons(4);
    
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&atype), reinterpret_cast<uint8_t*>(&atype) + 2);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&aclass), reinterpret_cast<uint8_t*>(&aclass) + 2);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&ttl), reinterpret_cast<uint8_t*>(&ttl) + 4);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&rdlength), reinterpret_cast<uint8_t*>(&rdlength) + 2);
    
    struct in_addr addr;
    if (inet_aton(ip.c_str(), &addr)) {
        response.insert(response.end(), reinterpret_cast<uint8_t*>(&addr), reinterpret_cast<uint8_t*>(&addr) + 4);
    }
    
    responses[domain] = move(response);
}

bool PrecompiledResponses::get_response(const string& domain, uint16_t query_id, vector<uint8_t>& response) {
    auto it = responses.find(domain);
    if (it == responses.end()) {
        return false;
    }
    
    response = it->second;
    *reinterpret_cast<uint16_t*>(response.data()) = query_id;
    
    return true;
}

DNSServer::DNSServer(uint16_t port) : socket_fd(-1), running(false) {
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        throw runtime_error("Failed to create socket");
    }
    
    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    int buffer_size = 1024 * 1024;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(socket_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(socket_fd);
        throw runtime_error("Failed to bind socket to port " + to_string(port));
    }
}

DNSServer::~DNSServer() {
    stop();
    if (socket_fd >= 0) {
        close(socket_fd);
    }
}

bool DNSServer::start() {
    if (running) {
        return false;
    }
    
    running = true;
    
    size_t num_threads = thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    
    for (size_t i = 0; i < num_threads; ++i) {
        worker_threads.emplace_back(&DNSServer::worker_thread, this);
    }
    
    cout << "DNS Server started with " << num_threads << " worker threads" << endl;
    return true;
}

void DNSServer::stop() {
    if (!running) {
        return;
    }
    
    running = false;
    
    // Wait for all threads to finish
    for (auto& thread : worker_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    worker_threads.clear();
    cout << "DNS Server stopped" << endl;
}

void DNSServer::add_upstream_resolver(const string& ip, uint16_t port) {
    upstream_resolvers.emplace_back(ip, port);
}

void DNSServer::add_local_domain(const string& domain, const string& ip) {
    precompiled.add_local_domain(domain, ip);
}

void DNSServer::worker_thread() {
    uint8_t buffer[512];  // DNS messages are typically <= 512 bytes
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (running) {
        ssize_t len = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                              reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        
        if (len > 0) {
            handle_query(buffer, len, client_addr);
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // Error occurred
            if (running) {
                std::cerr << "Error receiving data: " << strerror(errno) << std::endl;
            }
        }
    }
}

void DNSServer::handle_query(const uint8_t* data, size_t len, const sockaddr_in& client_addr) {
    auto start_time = std::chrono::high_resolution_clock::now();
    total_queries.fetch_add(1, std::memory_order_relaxed);
    
    DNSHeader header;
    if (!parse_dns_header(data, len, header)) {
        return;  // Invalid header
    }
    
    if (header.qdcount != 1) {
        return;  // We only handle single questions
    }
    
    size_t offset = 12;  // Skip header
    DNSQuestion question;
    if (!parse_dns_question(data, len, offset, question)) {
        return;  // Invalid question
    }
    
    if (question.qtype != 1) {  // Only handle A records
        auto error_response = build_error_response(header.id, 4);  // NOTIMP
        sendto(socket_fd, error_response.data(), error_response.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&client_addr), sizeof(client_addr));
        return;
    }
    
    std::string domain = question.qname;
    std::transform(domain.begin(), domain.end(), domain.begin(), ::tolower);
    
    // FAST PATH 1: Pre-compiled local domain response (target: <50μs)
    std::vector<uint8_t> response;
    if (precompiled.get_response(domain, header.id, response)) {
        sendto(socket_fd, response.data(), response.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&client_addr), sizeof(client_addr));
        local_domain_hits.fetch_add(1, std::memory_order_relaxed);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        
        // Log only if unusually slow for local domain
        if (duration.count() > 100) {
            std::cout << "Local domain " << domain << " served in " << duration.count() << "μs" << std::endl;
        }
        return;
    }
    
    // FAST PATH 2: Cache hit (target: <200μs)
    std::string cached_ip;
    if (cache.get(domain, cached_ip)) {
        auto dns_response = build_dns_response(header.id, question.qname, cached_ip);
        sendto(socket_fd, dns_response.data(), dns_response.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&client_addr), sizeof(client_addr));
        cache_hits.fetch_add(1, std::memory_order_relaxed);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        
        // Track response times for statistics
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            response_times.push_back(duration.count() / 1000.0);  // Convert to milliseconds
            if (response_times.size() > 10000) {
                response_times.erase(response_times.begin(), response_times.begin() + 5000);  // Keep last 5000
            }
        }
        
        return;
    }
    
    // SLOW PATH: Upstream resolution
    std::string resolved_ip;
    if (resolve_upstream(domain, resolved_ip)) {
        cache.set(domain, resolved_ip, 300);  // Cache for 5 minutes
        auto dns_response = build_dns_response(header.id, question.qname, resolved_ip);
        sendto(socket_fd, dns_response.data(), dns_response.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&client_addr), sizeof(client_addr));
    } else {
        auto error_response = build_error_response(header.id);
        sendto(socket_fd, error_response.data(), error_response.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&client_addr), sizeof(client_addr));
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Track upstream resolution times
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        response_times.push_back(duration.count());
        if (response_times.size() > 10000) {
            response_times.erase(response_times.begin(), response_times.begin() + 5000);
        }
    }
}

bool DNSServer::parse_dns_header(const uint8_t* data, size_t len, DNSHeader& header) {
    if (len < 12) {
        return false;
    }
    
    const uint16_t* ptr = reinterpret_cast<const uint16_t*>(data);
    header.id = ptr[0];
    header.flags = ntohs(ptr[1]);
    header.qdcount = ntohs(ptr[2]);
    header.ancount = ntohs(ptr[3]);
    header.nscount = ntohs(ptr[4]);
    header.arcount = ntohs(ptr[5]);
    
    return true;
}

bool DNSServer::parse_dns_question(const uint8_t* data, size_t len, size_t& offset, DNSQuestion& question) {
    question.qname = parse_domain_name(data, len, offset);
    if (question.qname.empty() || offset + 4 > len) {
        return false;
    }
    
    const uint16_t* ptr = reinterpret_cast<const uint16_t*>(data + offset);
    question.qtype = ntohs(ptr[0]);
    question.qclass = ntohs(ptr[1]);
    offset += 4;
    
    return true;
}

std::string DNSServer::parse_domain_name(const uint8_t* data, size_t len, size_t& offset) {
    std::string result;
    bool jumped = false;
    size_t original_offset = offset;
    
    while (offset < len) {
        uint8_t label_len = data[offset];
        
        if (label_len == 0) {
            offset++;
            break;
        }
        
        if ((label_len & 0xC0) == 0xC0) {  // Compression pointer
            if (!jumped) {
                original_offset = offset + 2;
                jumped = true;
            }
            offset = ((label_len & 0x3F) << 8) | data[offset + 1];
            continue;
        }
        
        if (offset + 1 + label_len > len) {
            return "";  // Invalid
        }
        
        if (!result.empty()) {
            result += ".";
        }
        
        result.append(reinterpret_cast<const char*>(data + offset + 1), label_len);
        offset += 1 + label_len;
    }
    
    if (jumped) {
        offset = original_offset;
    }
    
    return result;
}

std::vector<uint8_t> DNSServer::build_dns_response(uint16_t query_id, const std::string& domain, const std::string& ip) {
    std::vector<uint8_t> response;
    
    // DNS Header
    response.resize(12);
    uint16_t* header = reinterpret_cast<uint16_t*>(response.data());
    header[0] = query_id;
    header[1] = htons(0x8180);  // Standard response flags
    header[2] = htons(1);  // 1 question
    header[3] = htons(1);  // 1 answer
    header[4] = htons(0);  // 0 authority
    header[5] = htons(0);  // 0 additional
    
    // Question section
    encode_domain_name(domain, response);
    uint16_t qtype = htons(1);   // A record
    uint16_t qclass = htons(1);  // IN class
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&qtype), reinterpret_cast<uint8_t*>(&qtype) + 2);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&qclass), reinterpret_cast<uint8_t*>(&qclass) + 2);
    
    // Answer section (compression pointer)
    response.push_back(0xC0);
    response.push_back(0x0C);
    
    uint16_t atype = htons(1);
    uint16_t aclass = htons(1);
    uint32_t ttl = htonl(300);
    uint16_t rdlength = htons(4);
    
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&atype), reinterpret_cast<uint8_t*>(&atype) + 2);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&aclass), reinterpret_cast<uint8_t*>(&aclass) + 2);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&ttl), reinterpret_cast<uint8_t*>(&ttl) + 4);
    response.insert(response.end(), reinterpret_cast<uint8_t*>(&rdlength), reinterpret_cast<uint8_t*>(&rdlength) + 2);
    
    // IP address
    struct in_addr addr;
    if (inet_aton(ip.c_str(), &addr)) {
        response.insert(response.end(), reinterpret_cast<uint8_t*>(&addr), reinterpret_cast<uint8_t*>(&addr) + 4);
    }
    
    return response;
}

std::vector<uint8_t> DNSServer::build_error_response(uint16_t query_id, uint16_t rcode) {
    std::vector<uint8_t> response(12);
    uint16_t* header = reinterpret_cast<uint16_t*>(response.data());
    
    header[0] = query_id;
    header[1] = htons(0x8180 | rcode);  // Error response
    header[2] = htons(0);  // 0 questions
    header[3] = htons(0);  // 0 answers
    header[4] = htons(0);  // 0 authority
    header[5] = htons(0);  // 0 additional
    
    return response;
}

void DNSServer::encode_domain_name(const std::string& domain, std::vector<uint8_t>& buffer) {
    std::istringstream iss(domain);
    std::string label;
    
    while (std::getline(iss, label, '.')) {
        buffer.push_back(static_cast<uint8_t>(label.length()));
        buffer.insert(buffer.end(), label.begin(), label.end());
    }
    buffer.push_back(0);  // End of domain name
}

bool DNSServer::resolve_upstream(const std::string& domain, std::string& ip) {
    // Simple upstream resolution using system resolver
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // IPv4 only
    hints.ai_socktype = SOCK_DGRAM;
    
    int status = getaddrinfo(domain.c_str(), nullptr, &hints, &result);
    if (status != 0) {
        return false;
    }
    
    if (result && result->ai_family == AF_INET) {
        struct sockaddr_in* addr_in = (struct sockaddr_in*)result->ai_addr;
        char ip_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &(addr_in->sin_addr), ip_str, INET_ADDRSTRLEN)) {
            ip = std::string(ip_str);
            freeaddrinfo(result);
            return true;
        }
    }
    
    freeaddrinfo(result);
    return false;
}

DNSServer::PerformanceStats DNSServer::get_performance_stats() const {
    PerformanceStats stats;
    
    stats.total_queries = total_queries.load(std::memory_order_relaxed);
    stats.cache_hits = cache_hits.load(std::memory_order_relaxed);
    stats.local_domain_hits = local_domain_hits.load(std::memory_order_relaxed);
    
    if (stats.total_queries > 0) {
        stats.cache_hit_ratio = static_cast<double>(stats.cache_hits + stats.local_domain_hits) / stats.total_queries;
    }
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        if (!response_times.empty()) {
            auto sorted_times = response_times;
            std::sort(sorted_times.begin(), sorted_times.end());
            
            double sum = 0;
            for (double time : sorted_times) {
                sum += time;
            }
            stats.avg_response_time_ms = sum / sorted_times.size();
            
            size_t p95_idx = static_cast<size_t>(sorted_times.size() * 0.95);
            size_t p99_idx = static_cast<size_t>(sorted_times.size() * 0.99);
            
            stats.p95_response_time_ms = sorted_times[std::min(p95_idx, sorted_times.size() - 1)];
            stats.p99_response_time_ms = sorted_times[std::min(p99_idx, sorted_times.size() - 1)];
        }
    }
    
    return stats;
}
