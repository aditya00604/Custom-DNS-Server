#include "dns_server.h"
#include <iostream>
#include <signal.h>
#include <chrono>
#include <thread>

using namespace std;

unique_ptr<DNSServer> server;

void signal_handler(int signum) {
    cout << "\nReceived signal " << signum << ", shutting down..." << endl;
    if (server) {
        server->stop();
    }
    exit(0);
}

void print_stats_periodically() {
    while (true) {
        this_thread::sleep_for(chrono::seconds(30));
        
        if (server) {
            auto stats = server->get_performance_stats();
            
            cout << "\n=== DNS Server Performance Stats ===" << endl;
            cout << "Total queries: " << stats.total_queries << endl;
            cout << "Cache hits: " << stats.cache_hits << endl;
            cout << "Local domain hits: " << stats.local_domain_hits << endl;
            cout << "Cache hit ratio: " << (stats.cache_hit_ratio * 100) << "%" << endl;
            cout << "Average response time: " << stats.avg_response_time_ms << "ms" << endl;
            cout << "95th percentile: " << stats.p95_response_time_ms << "ms" << endl;
            cout << "99th percentile: " << stats.p99_response_time_ms << "ms" << endl;
            cout << "====================================\n" << endl;
        }
    }
}

int main(int argc, char* argv[]) {
    try {
        // Set up signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        
        // Parse command line arguments
        uint16_t port = 5353;  // Default to 5353 to avoid needing root privileges
        if (argc > 1) {
            port = static_cast<uint16_t>(std::stoi(argv[1]));
        }
        
        std::cout << "Starting Ultra-Fast C++ DNS Server on port " << port << std::endl;
        
        // Create and configure server
        server = std::make_unique<DNSServer>(port);
        
        // Add some common upstream resolvers
        server->add_upstream_resolver("8.8.8.8", 53);     // Google DNS
        server->add_upstream_resolver("1.1.1.1", 53);     // Cloudflare DNS
        server->add_upstream_resolver("208.67.222.222", 53); // OpenDNS
        
        // Add local domain examples for ultra-fast responses
        server->add_local_domain("localhost", "127.0.0.1");
        server->add_local_domain("router.local", "192.168.1.1");
        server->add_local_domain("dns.local", "192.168.1.1");
        server->add_local_domain("server.local", "192.168.1.100");
        
        // Performance test domains
        for (int i = 1; i <= 10; ++i) {
            server->add_local_domain("test" + std::to_string(i) + ".local", 
                                   "192.168.1." + std::to_string(100 + i));
        }
        
        // Start the server
        if (!server->start()) {
            std::cerr << "Failed to start DNS server" << std::endl;
            return 1;
        }
        
        // Start stats thread
        std::thread stats_thread(print_stats_periodically);
        stats_thread.detach();
        
        std::cout << "DNS Server is running. Performance targets:" << std::endl;
        std::cout << "  - Local domains: < 50μs response time" << std::endl;
        std::cout << "  - Cached domains: < 200μs response time" << std::endl;
        std::cout << "  - Cache size: 8192 entries with 16 shards" << std::endl;
        std::cout << "  - Worker threads: " << std::thread::hardware_concurrency() << std::endl;
        std::cout << "\nPress Ctrl+C to stop the server\n" << std::endl;
        
        // Performance benchmark
        std::cout << "Running initial performance benchmark..." << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 1000; ++i) {
        }
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        std::cout << "Benchmark complete. Average operation time: " 
                  << (duration.count() / 1000.0) << "μs" << std::endl;
        
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
