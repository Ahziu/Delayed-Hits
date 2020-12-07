#ifndef mtcache_base_h
#define mtcache_base_h

// STD headers
#include <assert.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Boost headers
#include <boost/bimap.hpp>
#include <boost/program_options.hpp>

// Libconfig headers
#include <libconfig.h++>

// Custom headers
#include "utils.hpp"
#include "cache_config.hpp"
#include "cache_common.hpp"

class MTBaseCache : BaseCache {
private：
    MTBaseCache* next; // points to the next tier
    MTBaseCache* prev; // points to the prev tier
    int tier_index;
    size_t kCacheHitLatency; // Time to transmit answer to previous tier
    boost::bimap<size_t, std::string> memory; 
public:
    // Adds two variables: 
    // Variable 1: pointer to next tier
    // Variable 2: tier index
    MTBaseCache(const size_t miss_latency, const size_t cache_set_associativity, const size_t
                num_cache_sets, const bool penalize_insertions, const HashType hash_type,
                int tier) :
                BaseCache(miss_latency, cache_set_associativity, num_cache_sets, penalize_insertions, hash_type),
                tier_index(tier), next(nullptr), prev(nullptr), kCacheHitLatency(0) {}

    MTBaseCache(TierConfig t, int index, const bool penalize_insertions, const HashType hash_type):
                BaseCache(t.getMiss(), t.getAssociativity(), t.getNumSets(), penalize_insertions, hash_type),
                tier_index(index), next(nullptr), prev(nullptr), kCacheHitLatency(0) {}

    virtual ~MTBaseCache() {}
    virtual std::string name() const override { return "MTBaseCache"; }

    std::unordered_map<std::string, utils::Packet> key_to_packet;
    boost::bimap<size_t, std::string> request_queue_; // A bi-directional mapping between target_clk and the keys
    boost::bimap<size_t, std::string> response_queue_; 
    // Pass in the pointer to next tier
    void set_next_tier(MTBaseCache* nt) {
        next = nt;
    }
    // Pass in the pointer to prev tier
    void set_prev_tier(MTBaseCache* pt) {
        prev = pt;
        kCacheHitLatency = pt->getCacheMissLatency();
    }

    // Tier - 0 Only.
    void processAll(std::list<utils::Packet>& processed_packets) {
        assert(tier_index == 0);
        // Step 1: update completed_reads_
        if (next != nullptr) {
            auto ready_read = next->response_queue_.left.find(clk());
            if (ready_read != next->response_queue_.left.end()) {
                const std::string& kkey = ready_read->second;
                BaseCacheSet& cset = *cache_sets_[getCacheIndex(kkey)];
                cset.write(kkey, 0);
                next->response_queue_.erase(ready_read);
                completed_reads_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, kkey));
            }
        }

        auto completed_read = completed_reads_.left.find(clk());
        if (completed_read != completed_reads_.left.end()) {
            const std::string& key = completed_read->second;
            std::list<utils::Packet>& queue = packet_queues_.at(key);
            assert(!queue.empty()); // Sanity check: Queue may not be empty

            // Fetch the cache set corresponding to this key
            size_t cache_idx = getCacheIndex(key);
            BaseCacheSet& cache_set = *cache_sets_[cache_idx];
            assert(!cache_set.contains(key));

            size_t headLatency = clk() - queue_head.getArrivalClock();
            for (std::list<utils::Packet>::iterator it = queue.begin(); 
                it != queue.end(); it++) {
                    it->addLatency(headLatency - 2 * (it->getTotalLatency()));
                    total_latency_ += it->getTotalLatency();
                    // It's used for aggregated cases to update cost
                    cache_set.recordPacketArrival(*it, headLatency);
            }

            cache_set.writeq(queue);
            processed_packets.insert(processed_packets.end(),
                                     queue.begin(), queue.end());
            queue.clear();
            packet_queues_.erase(key);
            completed_reads_.left.erase(completed_read);
        }
    }

    // For tier-0 Only, feed requests into the cache system.
    void process(utils::Packet& packet, std::list<utils::Packet>& processed_packets) override {

            // Step 1: Record the Arrival clock.
            assert(tier_index == 0);
            packet.setArrivalClock(clk());

            // Step 2: Retrieve information - key/request_queue/cache_set.
            const std::string& key = packet.getFlowId();
            auto queue_iter = packet_queues_.find(key); // maps to list<utils::Packet>
            BaseCacheSet& cache_set = *cache_sets_.at(getCacheIndex(key));

            // Step 3: Simulate the memory_entries_. Compulsory miss if not found.
            if (memory_entries_.find(key) == memory_entries_.end()) {
                assert(!cache_set.contains(key));
                memory_entries_.insert(key);
                if (!kIsPenalizeInsertions) {
                    cache_set.write(key, packet);
                }
            }

            // Step 4: Handle the packet
            if (cache_set.contains(key)) { // If hit, add to `processed_packets`
                assert(queue_iter == packet_queues_.end());
                cache_set.write(key, packet); // Update the cache set status given eviction policy
                packet.finalize();
                processed_packets.push_back(packet);
            } else { // If miss
                if (queue_iter == packet_queues_.end()) { // Case 1: If key not in packet_queues_
                    size_t target_clk = clk() + kCacheMissLatency; // when next-tier receives this request
                    assert(completed_reads_.right.find(key) == completed_reads_.right.end());
                    assert(completed_reads_.left.find(target_clk) == completed_reads_.left.end());
                    packet.finalize();
                    // Step 1： Allocate one queue for this key
                    packet_queues_[key].push_back(packet);
                    // Step 2: If this is the last tier
                    if (next == nullptr) {
                        completed_reads_.insert(boost::bimap<size_t, std::string>::
                                        value_type(target_clk + kCacheMissLatency, key)); // 2 - direction latency
                    } else {
                        // Step 3: Insert this queue to next tier's request queue
                        next->request_queue_.insert(boost::bimap<size_t, std::string>::value_type(target_clk, key));
                    }
                }
                else { // Case 2: If key is already in packet_queues
                    packet.setQueueingDelay(queue_iter->second.size());
                    // Record the relative latency compared to the queue head
                    packet.addLatency(queue_iter->second.front().getArrivalClock() - packet.getArrivalClock());
                    packet.finalize();
                    // Add this packet to the existing flow queue
                    queue_iter->second.push_back(packet);
                }
            }
    }

    // For higher tiers, retrieve response from upper tiers and update current tier
    void process_responseq() {
        boost::bimap<size_t, std::string> *responseq;
        if (next == nullptr) { // memory tier
            responseq = &memory;
        } else {
            responseq = &(next->response_queue_);
        }

        auto completed_read = responseq->left.find(clk());
        if (completed_read == responseq->left.end()) {
            return;
        }
        const std::string& key = completed_read->second;

        // Fetch the cache set corresponding to this key
        size_t cache_idx = getCacheIndex(key);
        BaseCacheSet& cache_set = *cache_sets_[cache_idx];

        // Sanity checks
        assert(!cache_set.contains(key));
        cache_set.write(key, 0);
        responseq->erase(completed_read);
        response_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, key));
    }

    // For higher-tiers
    void process_requestq() {
        assert(tier_index > 0);
        // Step 1: Check its request queue for target clock
        auto ready_req = request_queue_.left.find(clk());
        if (ready_req == request_queue_.left.end()) {
            return;
        }

        const std::string& key = ready_req->second;
        BaseCacheSet& cache_set = *cache_sets_.at(getCacheIndex(key));

        // Serve the ready request
        request_queue_.erase(ready_req);

        // If hit
        if (cache_set.contains(key)) {
            // Update the cache set status given eviction policy
            cache_set.write(key, 0);
            response_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, key));
        }
        // If miss
        else {
            if (next == nullptr) {
                // This tier gets response from memory
                memory.insert(boost::bimap<size_t, std::string>::value_type(clk() + 2 * kCacheMissLatency, key));
                return;
            }
            next->request_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheMissLatency, key));
        }
    }

    void handle_request(utils::Packet& packet, std::list<utils::Packet>& processed_packets) {
        if (tier_index == 0) {
            if (packet) process(packet, processed_packets);
        } else {
            process_requestq();
        }
    }

    void handle_response(std::list<utils::Packet>& processed_packets) {
        if (tier_index == 0) {
            processAll(processed_packets);
        } else {
            process_responseq();
        }
    }

    static void benchmark(GlobalConfig& config, const std::string& trace_fp, const std::
                          string& packets_fp, const size_t num_warmup_cycles) {
        // Set up tiers based on the configuration.
        size_t tiers_counts = config.get_tiers_size();
        std::vector<MTBaseCache*> tiers;

        // Step 1: Create instances of all tiers.
        for (int i = 0; i < tiers_counts; i++) {
            TierConfig t = config.get_tier(i);
            if (t.getPolicy() == "LRU") {
                tiers.push_back(new MTLRUCache(t, i, true, HashType::MURMUR_HASH));
            } else {
                tiers.push_back(new MTBaseCache(t, i, true, HashType::MURMUR_HASH));
            }
        }

        // Step 2: Set the next tier.
        for (int i = 0; i < tiers_counts - 1; i++) {
            tiers[i]->set_next_tier(tiers[i + 1]);
        }

        // Step 3: Set the previous tier.
        for (int i = 1; i < tiers_counts; i++) {
            tiers[i]->set_prev_tier(tiers[i - 1]);
        }
        // Finish setting up tiers.

        MTBaseCache* model = tiers[0];
        std::list<utils::Packet> packets; // List of processed packets
        size_t num_total_packets = 0; // Total packet count
        if (!packets_fp.empty()) {
            std::ofstream file(packets_fp, std::ios::out |
                                           std::ios::trunc);
            // Write the header
            file << model->name() << ";" << model->kCacheSetAssociativity << ";"
                 << model->kMaxNumCacheSets << ";" << model->kMaxNumCacheEntries
                 << std::endl;
        }
        // Step 4: Process the trace
        std::string line;
        std::ifstream trace_ifs(trace_fp);
        while (std::getline(trace_ifs, line)) {
            std::string timestamp, flow_id;

            /****************************************
             * Important note: Currently, we ignore *
             * packet timestamps and inject at most *
             * 1 packet into the system each cycle. *
             ****************************************/

            // Nonempty packet
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
                    savePackets(packets, packets_fp);
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
        savePackets(packets, packets_fp);

        // Simulations results
        size_t total_latency = model->getTotalLatency();
        double average_latency = (
            (num_total_packets == 0) ? 0 :
            static_cast<double>(total_latency) / num_total_packets);

        // Debug: Print trace and simulation statistics
        std::cout << std::endl;
        std::cout << "Total number of packets: " << num_total_packets << std::endl;
        std::cout << "Total latency is: " << model->getTotalLatency() << std::endl;
        std::cout << "Average latency is: " << std::fixed << std::setprecision(2)
                  << average_latency << std::endl << std::endl;

        // Free allocated spaces
        for (auto tier: tiers) {
            delete(tier);
        }
    }

    static void defaultBenchmark(int argc, char** argv) {
        using namespace boost::program_options;
        // Parameters
        //double c_scale;
        std::string trace_fp;
        std::string config_fp;
        std::string packets_fp;
        //size_t set_associativity;
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
                // ("cscale",      value<double>(&c_scale)->required(),                  "Parameter: Cache size (%Concurrent Flows)")
                ("packets",     value<std::string>(&packets_fp)->default_value(""),   "[Optional] Output packets file path")
                //("csa",         value<size_t>(&set_associativity)->default_value(0),  "[Optional] Parameter: Cache set-associativity")
                ("warmup",      value<size_t>(&num_warmup_cycles)->default_value(0),  "[Optional] Parameter: Number of cache warm-up cycles");

            // Parse model parameters
            store(parse_command_line(argc, argv, desc), variables);

            // Handle help flag
            if (variables.count("help")) {
                std::cout << desc << std::endl;
                return;
            }
            store(command_line_parser(argc, argv).options(
                desc).allow_unregistered().run(), variables);
            notify(variables);
        }
        // Flag argument errors
        catch(const required_option& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return;
        }
        catch(...) {
            std::cerr << "Unknown Error." << std::endl;
            return;
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

        //const auto flow_counts = utils::getFlowCounts(trace_fp);
        //size_t num_total_flows = flow_counts.num_total_flows;
        //size_t num_cfs = flow_counts.num_concurrent_flows;

        MTBaseCache::benchmark(config, trace_fp, packets_fp, num_warmup_cycles);
    }
};

#endif