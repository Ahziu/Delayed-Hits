#include <assert.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Libconfig headers
#include <libconfig.h++>

// Custom headers
#include "utils.hpp"
#include "cache_config.hpp"
#include "cache_common.hpp"
#include "mt_cache_lru.hpp"
#include "mt_cache_lhd.hpp"
#include "mt_cache_arc.hpp"
#include "mt_cache_lru_aggdelay.hpp"
#include "mt_cache_lhd_aggdelay.hpp"
/** Implements a multi-tiered Cache system. */
class MTCacheSystem {
protected:
    GlobalConfig config;
    MTBaseCache *model;
    size_t tiers_counts;
    std::vector<MTBaseCache*> tiers;
public:
    MTCacheSystem(GlobalConfig globalconfig): config(globalconfig) {}
    virtual ~MTCacheSystem() {
         for (size_t i = 0; i < tiers_counts; i++) {
            delete tiers[i];
        }
    }

    void setup() {
        this->tiers_counts = config.get_tiers_size();

        // Step 1: Create instances of all tiers.
        for (size_t i = 0; i < tiers_counts; i++) {
            TierConfig t = config.get_tier(i);
            if (t.getPolicy() == "LRU") {
                tiers.push_back(new MTLRUCache(t, i, true, HashType::MURMUR_HASH));
            } else if (t.getPolicy() == "LRUAGG") {
                tiers.push_back(new MTLRUAggregateDelayCache(t, i, true, HashType::MURMUR_HASH));
            } else if (t.getPolicy() == "LHDAGG") {
                tiers.push_back(new MTLHDAggregateDelayCache(t, i, true, HashType::MURMUR_HASH));
            } else if (t.getPolicy() == "LHD") {
                tiers.push_back(new MTLHDCache(t, i, true, HashType::MURMUR_HASH));
            } else if (t.getPolicy() == "ARC") {
                tiers.push_back(new MTARCCache(t, i, true, HashType::MURMUR_HASH));
            } else {
                tiers.push_back(new MTBaseCache(t, i, true, HashType::MURMUR_HASH));
            }
        }
        // Step 2: Set the next tier.
        for (size_t i = 0; i < tiers_counts - 1; i++) {
            tiers[i]->set_next_tier(tiers[i + 1]);
        }

        // Step 3: Set the previous tier.
        for (size_t i = 1; i < tiers_counts; i++) {
            tiers[i]->set_prev_tier(tiers[i - 1]);
        }
        model = tiers[0];
    }

    MTBaseCache* get_model() {
        return this->model;
    }

    void benchmark(const std::string& trace_fp, const std::string& packets_fp, const size_t num_warmup_cycles);
};

void MTCacheSystem::benchmark(const std::string& trace_fp, const std::string& packets_fp, const size_t num_warmup_cycles) {

    std::list<utils::Packet> packets; // List of processed packets
    size_t num_total_packets = 0; // Total packet count
    if (!packets_fp.empty()) {
        std::ofstream file(packets_fp, std::ios::out | std::ios::trunc);
        // Write the header
        file << model->name() << ";" << model->kCacheSetAssociativity << ";"
                << model->kMaxNumCacheSets << ";" << model->kMaxNumCacheEntries
                << std::endl;
    }
    // Step 1: Process the trace
    std::string line;
    std::ifstream trace_ifs(trace_fp);
    while (std::getline(trace_ifs, line)) {
        std::string timestamp, flow_id;
        if (!line.empty()) {
            std::stringstream linestream(line);
            // Parse the packet's flow ID and timestamp
            std::getline(linestream, timestamp, ';');
            std::getline(linestream, flow_id, ';');
        }
        // Cache warmup completed
        if (num_total_packets == num_warmup_cycles) {
            model->warmupComplete(); packets.clear();
            std::cout << "> Warmup complete after "
                        << num_warmup_cycles
                        << " cycles." << std::endl;
        }
        // Periodically save packets to file
        if (num_total_packets > 0 &&
            num_total_packets % 5000000 == 0) {
            if (num_total_packets >= num_warmup_cycles) {
                MTBaseCache::savePackets(packets, packets_fp);
            }
            std::cout << "On packet: " << num_total_packets
                        << ", latency: " << model->getTotalLatency() << std::endl;
        }
        // Process the packet
        if (!flow_id.empty()) {
            utils::Packet packet(flow_id);
            for (auto tier: tiers) {
                tier->handle_request(packet, packets);
            }
            for (auto tier: tiers) {
                tier->handle_response(packets);
            }
            for (auto tier: tiers) {
                tier->incrementClk();
            }
        }
        else {
            for (auto tier: tiers) {
                tier->handle_response(packets);
            }
            for (auto tier: tiers) {
                tier->incrementClk();
            }
        }
        num_total_packets++;
    }

    // Perform teardown
    model->teardown(packets);
    MTBaseCache::savePackets(packets, packets_fp);

    // Simulations results
    size_t total_latency = model->getTotalLatency();
    double average_latency = ((num_total_packets == 0) ? 0 :
        static_cast<double>(total_latency) / num_total_packets);

    // Debug: Print trace and simulation statistics
    std::cout << std::endl;
    std::cout << "Total number of packets: " << num_total_packets << std::endl;
    std::cout << "Total latency is: " << model->getTotalLatency() << std::endl;
    std::cout << "Average latency is: " << std::fixed << std::setprecision(2)
                << average_latency << std::endl << std::endl;

}

int main(int argc, char** argv) {
    using namespace boost::program_options;
    // Parameters
    std::string trace_fp;
    std::string config_fp;
    std::string packets_fp;

    size_t num_warmup_cycles;

    // Program options
    variables_map variables;
    options_description desc{"A caching simulator that models Delayed Hits"};

    try {
        // Command-line arguments
        desc.add_options()
            ("help",        "Prints this message")
            ("trace",       value<std::string>(&trace_fp)->required(),            "Input trace file path")
            ("config",      value<std::string>(&config_fp)->required(),           "Cache config file path")
            ("packets",     value<std::string>(&packets_fp)->default_value(""),   "[Optional] Output packets file path")
            ("warmup",      value<size_t>(&num_warmup_cycles)->default_value(0),  "[Optional] Parameter: Number of cache warm-up cycles");

        // Parse model parameters
        store(parse_command_line(argc, argv, desc), variables);

        // Handle help flag
        if (variables.count("help")) {
            std::cout << desc << std::endl;
            return 0;
        }
        store(command_line_parser(argc, argv).options(desc).allow_unregistered().run(), variables);
        notify(variables);
    } catch(const required_option& e) { // Flag argument errors
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
    catch(...) {
        std::cerr << "Unknown Error." << std::endl;
        return -1;
    }

    // Parse the config file
    libconfig::Config cfg;
    try { cfg.readFile(config_fp.c_str()); }
    catch(const libconfig::FileIOException &fioex) {
        std::cerr << "I/O error while reading config file." << std::endl;
    }
    catch(const libconfig::ParseException &pex) {
        std::cerr << "Parse error at " << pex.getFile()
                    << ":" << pex.getLine() << " - "
                    << pex.getError() << std::endl;
    }
    GlobalConfig config = GlobalConfig::from(cfg);

    MTCacheSystem cache(config);
    cache.setup();
    cache.benchmark(trace_fp, packets_fp, num_warmup_cycles);
    return 0;
}
