#include "log.hh"
#include "bloom_filter.hh"
#include "bloom_calculations.hh"

namespace utils {
static logging::logger filterlog("bloom_filter");

filter_ptr i_filter::get_filter(long num_elements, double max_false_pos_probability) {
    if (max_false_pos_probability > 1.0) {
        throw std::invalid_argument(sprint("Invalid probability %f: must be lower than 1.0", max_false_pos_probability));
    }

    if (max_false_pos_probability == 1.0) {
        return std::make_unique<filter::always_present_filter>();
    }

    int buckets_per_element = bloom_calculations::max_buckets_per_element(num_elements);
    auto spec = bloom_calculations::compute_bloom_spec(buckets_per_element, max_false_pos_probability);
    return filter::create_filter(spec.K, num_elements, spec.buckets_per_element);
}

filter_ptr i_filter::get_filter(long num_elements, int target_buckets_per_elem) {
    int max_buckets_per_element = std::max(1, bloom_calculations::max_buckets_per_element(num_elements));
    int buckets_per_element = std::min(target_buckets_per_elem, max_buckets_per_element);

    if (buckets_per_element < target_buckets_per_elem) {
        filterlog.warn("Cannot provide an optimal bloom_filter for {} elements ({}/{} buckets per element).", num_elements, buckets_per_element, target_buckets_per_elem);
    }
    auto spec = bloom_calculations::compute_bloom_spec(buckets_per_element);
    return filter::create_filter(spec.K, num_elements, spec.buckets_per_element);
}
}
