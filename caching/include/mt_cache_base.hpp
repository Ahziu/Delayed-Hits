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

// Custom headers
#include "utils.hpp"
#include "cache_common.hpp"

class MTBaseCache : BaseCache {
private：
    void* next = nullptr; // points to the next tier
    int tier_index = 1;
    size_t kCacheHitLatency = 0; // Time to transmit answer to previous tier
public:
    // Adds two variables: 
    // Variable 1: pointer to next tier
    // Variable 2: tier index
    MTBaseCache(const size_t miss_latency, const size_t cache_set_associativity, const size_t
                num_cache_sets, const bool penalize_insertions, const HashType hash_type,
                int tier, void* nextp, size_t hitLatency) :
                BaseCache(miss_latency, cache_set_associativity, num_cache_sets, penalize_insertions, hash_type),
                tier_index(tier), next(nextp, kCacheHitLatency(hitLatency)) {}
    virtual ~MTBaseCache() {}
    virtual std::string name() const override { return "MTBaseCache"; }

    std::unordered_map<std::string, utils::Packet> key_to_packet;
    boost::bimap<size_t, std::string> request_queue_; // A bi-directional mapping between target_clk and the keys
    boost::bimap<size_t, std::string> response_queue_; 
    // Pass in the pointer to next tier
    void set_next_tier(void* nt) {
        next = nt;
    }

    // Tier-1
    void processAll(std::list<utils::Packet>& processed_packets) {
        if (next != nullptr) { 
            auto ready_read = next->response_queue_.left.find(clk());
            if (ready_read != next->response_queue_.left.end()) {
                const std::string& kkey = completed_read->second;
                BaseCacheSet& cset = *cache_sets_[getCacheIndex(kkey)];
                cset.write(kkey, 0);
                next->response_queue_.erase(ready_read);
                completed_reads_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, kkey));
            }
        }

        auto completed_read = completed_reads_.left.find(clk()); // maps to key that is ready
        if (completed_read != completed_reads_.left.end()) {
            const std::string& key = completed_read->second;
            std::list<utils::Packet>& queue = packet_queues_.at(key);
            assert(!queue.empty()); // Sanity check: Queue may not be empty

            // Fetch the cache set corresponding to this key
            size_t cache_idx = getCacheIndex(key);
            BaseCacheSet& cache_set = *cache_sets_[cache_idx];

            // Sanity checks
            assert(!cache_set.contains(key));
            //assert(queue.front().getTotalLatency() == kCacheMissLatency);

            size_t headLatency = clk() + queue_head.getArrivalClock();
            for (std::list<utils::Packet>::iterator it = queue.begin(); 
                it != queue.end(); it++) {
                    it.addLatency(headLatency - 2 * (it->getTotalLatency()));
                    total_latency_ += packet.getTotalLatency();
                    // It's used for aggregated cases to update cost
                    cache_set.recordPacketArrival(*it, headLatency);
            }
            // Commit the queued entries
            cache_set.writeq(queue);
            processed_packets.insert(processed_packets.end(),
                                     queue.begin(), queue.end());

            // Purge the queue, as well as the bidict mapping
            queue.clear();
            packet_queues_.erase(key);
            completed_reads_.left.erase(completed_read);
        }
    }

    // For tier-1
    void process(utils::Packet& packet, std::list<utils::Packet>& processed_packets) override {

            assert(tier_index == 1);

            // Arrival clock is recorded only when it enters tier 1
            packet.setArrivalClock(clk());

            const std::string& key = packet.getFlowId();
            auto queue_iter = packet_queues_.find(key);  // maps to list<utils::Packet>
            BaseCacheSet& cache_set = *cache_sets_.at(getCacheIndex(key));

            // Simulate the memory_entries_, if not found, compulsory miss
            if (memory_entries_.find(key) == memory_entries_.end()) {
                assert(!cache_set.contains(key));
                memory_entries_.insert(key);
                if (!kIsPenalizeInsertions) {
                    cache_set.write(key, packet);
                }
            }

            // If hit, add to `processed_packets`
            if (cache_set.contains(key)) {
                assert(queue_iter == packet_queues_.end());
                // Update the cache set status given eviction policy
                cache_set.write(key, packet);
                packet.finalize();
                processed_packets.push_back(packet);
            } else { 
                // If miss
                // Case 1: If key not in packet_queues_
                if (queue_iter == packet_queues_.end()) {
                    // target_clk: if found next-tier
                    size_t target_clk = clk() + kCacheMissLatency;
                    assert(completed_reads_.right.find(key) == completed_reads_.right.end());
                    assert(completed_reads_.left.find(target_clk) == completed_reads_.left.end());
                    packet.finalize();
                    // Step 1： Allocate one queue for this key
                    packet_queues_[key].push_back(packet);
                    // Step 2: If this is the last tier
                    if (next == nullptr) {
                        completed_reads_.insert(boost::bimap<size_t, std::string>::
                                        value_type(target_clk + kCacheHitLatency, key));
                    } else {
                        // Step 3: Insert this queue to next tier's request queue
                        next->request_queue_.insert(boost::bimap<size_t, std::string>::value_type(target_clk, key));
                    }
                }
                // Case 2: If key is already in packet_queues
                else {
                    // queue_iter->second.size() how many packets are waiting in this queue before this packet
                    packet.setQueueingDelay(queue_iter->second.size());
                    // Relative latency compared to the queue head
                    packet.addLatency(queue_iter->second.front().getArrivalClock() - clk());
                    packet.finalize();
                    // Add this packet to the existing flow queue
                    queue_iter->second.push_back(packet);
                }
            }
    }

    // For higher tiers, retrieve response from upper tiers and update current tier
    void process_responseq() {
        if (next == nullptr) { // memory tier
            return;
        }

        auto completed_read = next->response_queue_.left.find(clk());
        if (completed_read == next->response_queue_.left.end()) {
            return;
        }
        const std::string& key = completed_read->second;

        // Fetch the cache set corresponding to this key
        size_t cache_idx = getCacheIndex(key);
        BaseCacheSet& cache_set = *cache_sets_[cache_idx];

        // Sanity checks
        assert(!cache_set.contains(key));
        cache_set.write(key, 0);
        next->response_queue_.erase(completed_read);
        response_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, key));
    }

    // For higher-tiers
    void process_requestq() {
        assert(tier_index > 1);
        // Step 1: Check its request queue for target clock
        auto ready_req = request_queue_.left.find(clk());
        if (ready_req == request_queue_.left.end()) {
            return;
        }

        const std::string& key = ready_req->second;
        BaseCacheSet& cache_set = *cache_sets_.at(getCacheIndex(key));

        // Serve the ready request
        request_queue_.erase(ready_req);

        if (next == nullptr) {
            // This tier simulates memory, hence always hit
            response_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, key));
            return;
        }

        // If hit
        if (cache_set.contains(key)) {
            // Update the cache set status given eviction policy
            cache_set.write(key, 0);
            response_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, key));
        }
        // If miss
        else {
            next->request_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheMissLatency, key));
        }
    }

    void handle_request(utils::Packet& packet, std::list<utils::Packet>& processed_packets) {
        if (tier_index == 1) {
            if (packet) process(packet, processed_packets);
        } else {
            process_requestq();
        }
    }

    void handle_response(std::list<utils::Packet>& processed_packets) {
        if (tier_index == 1) {
            processAll(processed_packets);
        } else {
            process_responseq();
        }
        incrementClk();
    }


#endif