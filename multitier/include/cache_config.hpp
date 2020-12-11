#ifndef cache_config_h
#define cache_config_h

// STD headers
#include <assert.h>
#include <string>
#include <stdexcept>
#include <stdlib.h>
#include <vector>

// Libconfig
#include <libconfig.h++>

/**
 * Represents the configuration for a single cache tier.
 */
class TierConfig {
private:
    const std::string kPolicy; // Replacement policy
    const uint kNumSets; // Number of cache sets in this tier
    const uint kAssociativity; // Set-associativity for this tier
    const uint missLatency; 

    TierConfig() = delete;
    explicit TierConfig(const std::string policy, const uint num_sets,
                        const uint associativity, const uint miss) : kPolicy(policy),
                        kNumSets(num_sets), kAssociativity(associativity),
                        missLatency(miss) {}
public:
    // Factory method
    static TierConfig from(const libconfig::Setting &tier) {
        // Parameters to parse
        uint num_sets = 0;
        std::string policy;
        uint associativity = 0;
        uint miss = 0;

        if (!tier.lookupValue("policy", policy) ||
            !tier.lookupValue("miss_latency", miss) ||
            !tier.lookupValue("num_sets", num_sets) ||
            !tier.lookupValue("associativity", associativity)) {
            throw std::runtime_error("Bad configuration file.");
        }
        return TierConfig(policy, num_sets, associativity, miss);
    }

    // Accessors
    uint getNumSets() const { return kNumSets; }
    const std::string& getPolicy() const { return kPolicy; }
    uint getAssociativity() const { return kAssociativity; }
    uint getMiss() const { return missLatency; }
};

/**
 * Represents the global configuration for a multi-tier cache.
 */
class GlobalConfig {
public:
    enum class InclusionPolicy {
        INCLUSIVE = 0, EXCLUSIVE, NINE };

private:
    std::vector<TierConfig> tiers_; // Cache tiers
    std::vector<uint> latencies_; // Inter-tier latencies
    InclusionPolicy inclusion_policy_; // Cache inclusion policy

    /**
     * Set the inclusion policy.
     */
    void setInclusionPolicy(const std::string& policy_name) {
        if (policy_name == "Inclusive") {
            inclusion_policy_ = InclusionPolicy::INCLUSIVE;
        }
        else if (policy_name == "Exclusive") {
            inclusion_policy_ = InclusionPolicy::EXCLUSIVE;
        }
        else if (policy_name == "NINE") {
            inclusion_policy_ = InclusionPolicy::NINE;
        }
        else {
            throw std::invalid_argument(
                "Unknown cache inclusion policy");
        }
    }

    // Validation method
    void validate() const {
        assert(tiers_.size() > 0);
        assert(latencies_.size() == tiers_.size());
    }

    // Private constructor
    GlobalConfig() {}

public:
    // Factory method
    static GlobalConfig from(const libconfig::Config& cfg) {
        const libconfig::Setting& root = cfg.getRoot();
        GlobalConfig global_config; // Return value

        // Set the inclusion policy
        std::string inclusion_policy;
        if (!root.lookupValue("inclusion_policy", inclusion_policy)) {
            throw std::runtime_error("Bad configuration file.");
        }
        global_config.setInclusionPolicy(
            inclusion_policy);

        // Populate the cache tiers
        const libconfig::Setting& tiers = root["tiers"];
        for (int idx = 0; idx < tiers.getLength(); idx++) {
            global_config.tiers_.push_back(TierConfig::from(tiers[idx]));
        }

        // Populate the inter-tier latencies
        const libconfig::Setting& latencies = root["latencies"];
        for (int idx = 0; idx < latencies.getLength(); idx++) {
            global_config.latencies_.push_back(latencies[idx]);
        }
        global_config.validate();
        return global_config;
    }

    size_t get_tiers_size() {
        return tiers_.size();
    }

    const TierConfig get_tier(size_t index) const {
        assert(index >= 0 && index < tiers_.size());
        return tiers_[index];
    }
};

#endif // cache_config_h
