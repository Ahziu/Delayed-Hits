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

namespace caching {

class BaseCacheSet {
protected:
    const size_t kNumEntries; // The number of cache entries in this set
    std::unordered_set<std::string> occupied_entries_set_; // Set of currently
                                                           // cached flow IDs.
public:
    BaseCacheSet(const size_t num_entries) : kNumEntries(num_entries) {}
    virtual ~BaseCacheSet() {}

    bool contains(const std::string& flow_id) const {
        return (occupied_entries_set_.find(flow_id) != occupied_entries_set_.end());
    }

    size_t getNumEntries() const { return kNumEntries; }

    virtual void recordPacketArrival(const utils::Packet& packet, size_t latency) {
        SUPPRESS_UNUSED_WARNING(packet);
        SUPPRESS_UNUSED_WARNING(latency);
    }

    /**
     * Simulates a cache write.
     *
     * @param key The key corresponding to this write request.
     * @param packet The packet corresponding to this write request.
     * @return The written CacheEntry instance.
     */
    virtual CacheEntry
    write(const std::string& key, const utils::Packet& packet) = 0;

    /**
     * Simulates a sequence of cache writes for a particular flow's packet queue.
     * Invoking this method should be functionally equivalent to invoking write()
     * on every queued packet; this simply presents an optimization opportunity
     * for policies which do not distinguish between single/multiple writes.
     *
     * @param queue The queued write requests.
     * @return The written CacheEntry instance.
     */
    virtual CacheEntry
    writeq(const std::list<utils::Packet>& queue) {
        CacheEntry written_entry;
        for (const auto& packet : queue) {
            written_entry = write(packet.getFlowId(), packet);
        }
        return written_entry;
    }
};

class MTBaseCache {
public:
    const size_t kCacheMissLatency;         // Cost (in cycles) of an L1 cache miss
    const size_t kMaxNumCacheSets;          // Maximum number of sets in the L1 cache
    const size_t kMaxNumCacheEntries;       // Maximum number of entries in the L1 cache
    const size_t kCacheSetAssociativity;    // Set-associativity of the L1 cache
    const size_t kIsPenalizeInsertions;     // Whether insertions should incur an L1 cache miss
    const HashFamily kHashFamily;           // A HashFamily instance

    int tier_index;
    MTBaseCache* next; // points to the next tier
    MTBaseCache* prev; // points to the prev tier
    size_t kCacheHitLatency; // Time to transmit answer to previous tier
    size_t clk_ = 0; // Time in clock cycles
    size_t total_latency_ = 0; // Total packet latency
    std::vector<BaseCacheSet*> cache_sets_; // Fixed-sized array of CacheSet instances
    std::unordered_set<std::string> memory_entries_; // Set of keys in the global store
    boost::bimap<size_t, std::string> completed_reads_; // A dictionary mapping clk values to the keys

    std::unordered_map<std::string, std::list<utils::Packet>> packet_queues_; 
    boost::bimap<size_t, std::string> memory_; 
    boost::bimap<size_t, std::string> request_queue_;
    boost::bimap<size_t, std::string> response_queue_;
    virtual std::string name() const { return "MTBaseCache"; }

    void set_next_tier(MTBaseCache* nt) {
        next = nt;
    }

    void set_prev_tier(MTBaseCache* pt) {
        prev = pt;
        kCacheHitLatency = pt->getCacheMissLatency();
    }

    /** Records arrival of a new packet. */
    virtual void recordPacketArrival(const utils::Packet& packet, size_t latency) {
        SUPPRESS_UNUSED_WARNING(packet);
        SUPPRESS_UNUSED_WARNING(latency);
    }

    /** Returns the cache miss latency. */
    size_t getCacheMissLatency() const { return kCacheMissLatency; }
    /** Returns the number of entries in the memory at any instant.*/
    size_t getNumMemoryEntries() const { return memory_entries_.size(); }
    /** Returns the current time in clock cycles.*/
    size_t clk() const { return clk_; }
    /** Returns the total packet latency for this simulation.*/
    size_t getTotalLatency() const { return total_latency_; }
    /** Returns the cache index corresponding to the given key. */
    size_t getCacheIndex(const std::string& key) const {
        return (kMaxNumCacheSets == 1) ? 0 : kHashFamily.hash(0, key) % kMaxNumCacheSets;
    }
    /** Increments the internal clock.*/
    void incrementClk() { clk_++; }


    /** Constructor without tier config. */
    MTBaseCache(const size_t miss_latency, const size_t cache_set_associativity, const size_t
                num_cache_sets, const bool penalize_insertions, const caching::HashType hash_type,
                int tier) :
                kCacheMissLatency(miss_latency), kMaxNumCacheSets(num_cache_sets),
                kMaxNumCacheEntries(num_cache_sets * cache_set_associativity),
                kCacheSetAssociativity(cache_set_associativity),
                kIsPenalizeInsertions(penalize_insertions),
                kHashFamily(1, hash_type),
                tier_index(tier), next(nullptr), prev(nullptr), kCacheHitLatency(0) {}
    /** Constructor with tier config. */
    MTBaseCache(TierConfig t, int index, const bool penalize_insertions, const caching::HashType hash_type):
                kCacheMissLatency(t.getMiss()), kMaxNumCacheSets(t.getNumSets()),
                kMaxNumCacheEntries(t.getNumSets() * t.getAssociativity()),
                kCacheSetAssociativity(t.getAssociativity()),
                kIsPenalizeInsertions(penalize_insertions),
                kHashFamily(1, hash_type),
                tier_index(index), next(nullptr), prev(nullptr), kCacheHitLatency(0) {}
    /** Destructor. */
    virtual ~MTBaseCache() {
        assert(cache_sets_.size() == kMaxNumCacheSets);
        for (size_t idx = 0; idx < kMaxNumCacheSets; idx++) { delete(cache_sets_[idx]); }
    }

    // Tier - 0 Only.
    void final_reply(std::list<utils::Packet>& processed_packets) {
        assert(tier_index == 0);
        // Step 1: update completed_reads_
        if (next != nullptr) {
            //auto ready_read = next->response_queue_.left.find(clk());
            //if (ready_read != next->response_queue_.left.end()) {
                //const std::string& kkey = ready_read->second;
                //caching::BaseCacheSet& cset = *cache_sets_[getCacheIndex(kkey)];
                //cset.write(kkey, (const utils::Packet)0);
                //next->response_queue_.erase(ready_read);
                //completed_reads_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, kkey));
            //}
        }

        auto completed_read = completed_reads_.left.find(clk());
        if (completed_read != completed_reads_.left.end()) {
            const std::string& key = completed_read->second;
            std::list<utils::Packet>& queue = packet_queues_.at(key);
            assert(!queue.empty()); // Sanity check: Queue may not be empty

            // Fetch the cache set corresponding to this key
            size_t cache_idx = getCacheIndex(key);
            caching::BaseCacheSet& cache_set = *cache_sets_[cache_idx];
            assert(!cache_set.contains(key));

            size_t headLatency = clk() - queue.begin()->getArrivalClock();
            for (std::list<utils::Packet>::iterator it = queue.begin(); 
                it != queue.end(); it++) {
                    it->addLatency(headLatency - 2 * (it->getTotalLatency()));
                    total_latency_ += it->getTotalLatency();
                    // It's used for aggregated cases to update cost
                    recordPacketArrival(*it, it->getTotalLatency());
                    cache_set.recordPacketArrival(*it, it->getTotalLatency());
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
    void feed_packet(utils::Packet& packet, std::list<utils::Packet>& processed_packets) {

            // Step 1: Record the Arrival clock.
            assert(tier_index == 0);
            packet.setArrivalClock(clk());

            // Step 2: Retrieve information - key/request_queue/cache_set.
            const std::string& key = packet.getFlowId();
            auto queue_iter = packet_queues_.find(key); // unordered_map: string -> list<utils::Packet>
            BaseCacheSet& cache_set = *cache_sets_.at(getCacheIndex(key));

            // Step 3: Simulate the memory_entries_. Compulsory miss if not found.
            if (next == nullptr && memory_entries_.find(key) == memory_entries_.end()) {
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
            responseq = &memory_;
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
        cache_set.write(key, (const utils::Packet)0);
        responseq->left.erase(completed_read);
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

        // Simulate the memory_entries_. Compulsory miss if not found.
        if (next == nullptr && memory_entries_.find(key) == memory_entries_.end()) {
            assert(!cache_set.contains(key));
            memory_entries_.insert(key);
            if (!kIsPenalizeInsertions) {
                cache_set.write(key, (const utils::Packet)0);
            }
        }
        
        // Serve the ready request
        request_queue_.left.erase(ready_req);

        // If hit
        if (cache_set.contains(key)) {
            // Update the cache set status given eviction policy
            cache_set.write(key, (const utils::Packet)0);
            response_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheHitLatency, key));
        }
        // If miss
        else {
            if (next == nullptr) {
                // This tier gets response from memory
                memory_.insert(boost::bimap<size_t, std::string>::value_type(clk() + 2 * kCacheMissLatency, key));
                return;
            }
            next->request_queue_.insert(boost::bimap<size_t, std::string>::value_type(clk() + kCacheMissLatency, key));
        }
    }

    void handle_request(utils::Packet& packet, std::list<utils::Packet>& processed_packets) {
        if (tier_index == 0) {
            feed_packet(packet, processed_packets);
        } else {
            process_requestq();
        }
    }

    void handle_response(std::list<utils::Packet>& processed_packets) {
        if (tier_index == 0) {
            final_reply(processed_packets);
        } else {
            process_responseq();
        }
    }
    /** Indicates completion of the warmup period. */
    void warmupComplete() {
        total_latency_ = 0;
        packet_queues_.clear();
        completed_reads_.clear();
    }

    /** Indicates completion of the simulation. */
    void teardown(std::list<utils::Packet>& processed_packets) {
        while (!packet_queues_.empty()) {
            final_reply(processed_packets);
        }
    }

    /** Save the raw packet data to file. */
    static void savePackets(std::list<utils::Packet>& packets,
                            const std::string& packets_fp) {
        if (!packets_fp.empty()) {
            std::ofstream file(packets_fp, std::ios::out |
                                           std::ios::app);
            // Save the raw packets to file
            for (const utils::Packet& packet : packets) {
                file << packet.getFlowId() << ";"
                     << static_cast<size_t>(packet.getTotalLatency()) << ";"
                     << static_cast<size_t>(packet.getQueueingDelay()) << std::endl;
            }
        }
        packets.clear();
    }

};

} // namespace caching
#endif