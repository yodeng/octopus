// Copyright (c) 2016 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#include "haplotype_generator.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <numeric>
#include  <cmath>
#include <cassert>

#include <core/types/variant.hpp>
#include <core/types/haplotype.hpp>
#include <concepts/mappable.hpp>
#include <utils/mappable_algorithms.hpp>
#include <logging/logging.hpp>

#include <iostream> // DEBUG
#include "timers.hpp"

namespace octopus { namespace coretools {

// HaplotypeOverflow

HaplotypeGenerator::HaplotypeOverflow::HaplotypeOverflow(GenomicRegion region, unsigned size)
: runtime_error {"HaplotypeOverflowError"}
, region_ {std::move(region)}
, size_ {size}
, message_ {}
{}

const char* HaplotypeGenerator::HaplotypeOverflow::what() const noexcept
{
    return runtime_error::what();
}

const GenomicRegion& HaplotypeGenerator::HaplotypeOverflow::region() const noexcept
{
    return region_;
}

unsigned HaplotypeGenerator::HaplotypeOverflow::size() const noexcept
{
    return size_;
}

// HaplotypeGenerator

auto max_included(const unsigned max_haplotypes)
{
    return 2 * static_cast<unsigned>(std::max(1.0, std::log2(max_haplotypes))) - 1;
}

namespace {
    auto decompose(const MappableFlatSet<Variant>& variants)
    {
        std::deque<Allele> alleles {};
        
        for (const auto& variant : variants) {
            alleles.push_back(variant.ref_allele());
            alleles.push_back(variant.alt_allele());
        }
        
        std::sort(std::begin(alleles), std::end(alleles));
        
        return MappableFlatSet<Allele> {
            std::make_move_iterator(std::begin(alleles)),
            std::make_move_iterator(std::unique(std::begin(alleles), std::end(alleles)))
        };
    }
}

namespace debug {
    template <typename Range>
    void print_active_alleles(const Range& alleles, const GenomicRegion& active_region);
} // namespace debug

// public members

HaplotypeGenerator::HaplotypeGenerator(const ReferenceGenome& reference,
                                       const MappableFlatSet<Variant>& candidates,
                                       const ReadMap& reads,
                                       Policies policies,
                                       Haplotype::MappingDomain::Size min_flank_pad)
try
: policies_ {std::move(policies)}
, min_flank_pad_ {min_flank_pad}
, tree_ {contig_name(candidates.front()), reference}
, default_walker_ {max_included(policies_.haplotype_limits.target)}
, holdout_walker_ {
    max_included(policies_.haplotype_limits.target),
    GenomeWalker::IndicatorPolicy::includeAll
  }
, lagged_walker_ {}
, alleles_ {decompose(candidates)}
, reads_ {reads}
, next_active_region_ {}
, active_holdouts_ {}
, holdout_region_ {}
{
    rightmost_allele_ = *rightmost_mappable(alleles_);
    active_region_    = shift(head_region(alleles_.leftmost()), -1);
    
    if (policies_.lagging != Policies::Lagging::none) {
        GenomeWalker::IndicatorPolicy walker_policy;
        if (policies_.lagging == Policies::Lagging::conservative) {
            walker_policy = GenomeWalker::IndicatorPolicy::includeIfSharedWithNovelRegion;
        } else {
            walker_policy = GenomeWalker::IndicatorPolicy::includeIfLinkableToNovelRegion;
        }
        lagged_walker_ = GenomeWalker {max_included(policies_.haplotype_limits.target), walker_policy};
    }
}
catch (...) {
    if (candidates.empty()) {
        throw std::runtime_error {"HaplotypeGenerator: not supplied with any candidates"};
    }
    throw;
}

HaplotypeGenerator::HaplotypePacket HaplotypeGenerator::generate()
{
    if (alleles_.empty()) {
        return std::make_pair(std::vector<Haplotype> {}, active_region_);
    }
    if (in_holdout_mode() && can_reintroduce_holdouts()) {
        reintroduce_holdouts();
        if (tree_.num_haplotypes() > policies_.haplotype_limits.overflow) {
            throw HaplotypeOverflow {active_region_, tree_.num_haplotypes()};
        }
        active_region_ = tree_.encompassing_region();
        reset_next_active_region();
    } else {
        update_next_active_region();
        if (is_after(*next_active_region_, rightmost_allele_)) {
            // Then we are done
            return std::make_pair(std::vector<Haplotype> {}, *next_active_region_);
        }
        
        progress(*next_active_region_);
        
        auto novel_active_region = *next_active_region_;
        if (!tree_.is_empty()) {
            novel_active_region = right_overhang_region(*next_active_region_, active_region_);
        }
        
        auto novel_active_alleles = overlap_range(alleles_, novel_active_region);
        auto last_added_itr = extend_tree_until(novel_active_alleles, tree_, policies_.haplotype_limits.holdout);
        
        if (last_added_itr != std::cend(novel_active_alleles)) {
            reset_next_active_region();
            if (can_extract_holdouts(novel_active_region)) {
                extract_holdouts(novel_active_region);
                tree_.clear(novel_active_region);
                
                update_next_active_region();
                
                active_region_ = *std::move(next_active_region_);
                reset_next_active_region();
                
                const auto new_novel_alleles = overlap_range(alleles_, active_region_);
                auto it = extend_tree_until(new_novel_alleles, tree_, policies_.haplotype_limits.overflow);
                
                if (it != std::cend(new_novel_alleles)) {
                    throw HaplotypeOverflow {active_region_, tree_.num_haplotypes()}; 
                }
            } else {
                last_added_itr = extend_tree_until(last_added_itr, std::cend(novel_active_alleles), tree_,
                                                   policies_.haplotype_limits.overflow);
                active_region_ = tree_.encompassing_region();
                if (last_added_itr != std::cend(novel_active_alleles)) {
                    throw HaplotypeOverflow {active_region_, tree_.num_haplotypes()};
                }
            }
        } else {
            active_region_ = *std::move(next_active_region_);
            reset_next_active_region();
        }
    }
    auto haplotypes = tree_.extract_haplotypes(calculate_haplotype_region());
    if (!is_lagging_enabled()) tree_.clear();
    return std::make_pair(std::move(haplotypes), active_region_);
}

boost::optional<GenomicRegion> HaplotypeGenerator::peek_next_active_region() const
{
    if (in_holdout_mode()) return boost::none;
    update_next_active_region();
    return *next_active_region_;
}

void HaplotypeGenerator::clear_progress() noexcept
{
    tree_.clear();
    reset_next_active_region();
    if (in_holdout_mode()) {
        clear_holdouts();
    }
}

void HaplotypeGenerator::jump(GenomicRegion region)
{
    clear_progress();
    progress(std::move(region));
}

bool HaplotypeGenerator::removal_has_impact() const
{
    if (in_holdout_mode()) return true;
    if (!is_lagging_enabled() || contains(active_region_, rightmost_allele_)) return false;
    const auto max_lagged_region = lagged_walker_->walk(active_region_, reads_, alleles_);
    return overlaps(max_lagged_region, active_region_);
}

unsigned HaplotypeGenerator::max_removal_impact() const
{
    if (in_holdout_mode()) return tree_.num_haplotypes();
    if (!is_lagging_enabled() || contains(active_region_, rightmost_allele_)) return 0;
    
    const auto max_lagged_region = lagged_walker_->walk(active_region_, reads_, alleles_);
    
    if (!overlaps(max_lagged_region, active_region_)) return 0;
    
    const auto novel_region = right_overhang_region(max_lagged_region, active_region_);
    const auto num_novel_alleles = count_overlapped(alleles_, novel_region);
    
    if (num_novel_alleles == 0) return 0;
    
    const auto max_new_haplotypes = std::max(static_cast<unsigned>(std::exp2(num_novel_alleles / 2)), 1u);
    const auto num_leftover_haplotypes = policies_.haplotype_limits.target / max_new_haplotypes;
    const auto cur_num_haplotypes = tree_.num_haplotypes();
    
    if (cur_num_haplotypes > num_leftover_haplotypes) {
        return cur_num_haplotypes - num_leftover_haplotypes;
    }
    return cur_num_haplotypes;
}

// private methods

bool HaplotypeGenerator::is_lagging_enabled() const noexcept
{
    return lagged_walker_ != boost::none;
}

bool HaplotypeGenerator::is_active_region_lagged() const
{
    if (in_holdout_mode()) return true;
    if (!is_lagging_enabled()) return false;
    const auto next_lagged_region = lagged_walker_->walk(active_region_, reads_, alleles_);
    return overlaps(active_region_, next_lagged_region);
}

void HaplotypeGenerator::reset_next_active_region() const noexcept
{
    next_active_region_ = boost::none;
}

void HaplotypeGenerator::update_next_active_region() const
{
    if (!next_active_region_) {
        if (is_lagging_enabled() || in_holdout_mode()) {
            // If we are in holdout mode then lagging is required
            update_lagged_next_active_region();
        } else {
            next_active_region_ = default_walker_.walk(active_region_, reads_, alleles_);
        }
    }
    assert(in_holdout_mode() || active_region_ <= *next_active_region_);
}

namespace {

template <typename Range>
bool can_remove_entire_passed_region(const GenomicRegion& current_active_region,
                                     const GenomicRegion& next_active_region,
                                     const Range& passed_alleles)
{
    return passed_alleles.empty() || !overlaps(rightmost_region(passed_alleles), next_active_region);
}

template <typename Range>
bool requires_staged_removal(const Range& passed_alleles)
{
    if (passed_alleles.empty() || !is_empty_region(passed_alleles.back())) {
        return false;
    }
    const auto last = crend(passed_alleles);
    const auto& last_passed_allele = passed_alleles.back();
    const auto it = std::find_if_not(std::next(crbegin(passed_alleles)), last,
                                     [&last_passed_allele] (const auto& allele) {
                                         return is_same_region(allele, last_passed_allele);
                                     });
    return it != last && is_position(*it);
}

template <typename T>
void pop_front(std::vector<T>& v) {
    assert(!v.empty());
    v.erase(std::cbegin(v));
}

} // namespace

void HaplotypeGenerator::update_lagged_next_active_region() const
{
    if (contains(active_region_, rightmost_allele_)) {
        // Nothing more to do
        next_active_region_ = shift(tail_region(rightmost_allele_), 2);
        return;
    }
    
    GenomicRegion max_lagged_region;
    
    if (!in_holdout_mode()) {
        max_lagged_region = lagged_walker_->walk(active_region_, reads_, alleles_);
    } else {
        max_lagged_region = holdout_walker_.walk(active_region_, reads_, alleles_);
    }
    
    assert(has_contained(alleles_, max_lagged_region));
    
    if (!overlaps(active_region_, max_lagged_region)) {
        next_active_region_ = std::move(max_lagged_region);
    } else {
        HaplotypeTree test_tree {tree_}; // use a temporary tree to see how much we can lag
        
        if (begins_before(active_region_, max_lagged_region)) {
            const auto novel_region  = right_overhang_region(max_lagged_region, active_region_);
            const auto novel_alleles = overlap_range(alleles_, novel_region);
            
            const auto it = extend_tree_until(novel_alleles, test_tree, policies_.haplotype_limits.target);
            
            if (it == std::cend(novel_alleles)) {
                // Ignore the walker as we have better information here
                next_active_region_ = test_tree.encompassing_region();
                return;
            } else {
                test_tree.clear(novel_region); // undo previous extension
                
                const auto passed_region  = left_overhang_region(active_region_, max_lagged_region);
                const auto passed_alleles = overlap_range(alleles_, passed_region);
                
                if (can_remove_entire_passed_region(active_region_, max_lagged_region, passed_alleles)) {
                    test_tree.clear(passed_region);
                } else if (requires_staged_removal(passed_alleles)) {
                    const auto first_removal_region = expand_rhs(passed_region, -1);
                    test_tree.clear(first_removal_region);
                    test_tree.clear(tail_region(first_removal_region));
                } else {
                    test_tree.clear(expand_rhs(passed_region, -1));
                }
            }
        }
        
        const auto novel_region  = right_overhang_region(max_lagged_region, active_region_);
        const auto novel_alleles = overlap_range(alleles_, novel_region);
        assert(!novel_alleles.empty());
        auto mutually_exclusive_novel_regions = extract_mutually_exclusive_regions(novel_alleles);
        const auto indicator_region  = overlapped_region(active_region_, max_lagged_region);
        const auto indicator_alleles = overlap_range(alleles_, indicator_region);
        assert(!indicator_alleles.empty());
        auto mutually_exclusive_indicator_regions = extract_mutually_exclusive_regions(indicator_alleles);
        
        if (mutually_exclusive_indicator_regions.back() == mutually_exclusive_novel_regions.front()) {
            assert(is_empty(mutually_exclusive_novel_regions.front()));
            pop_front(mutually_exclusive_novel_regions);
        }
        if (!in_holdout_mode()) {
            auto it = std::find_if(std::cbegin(mutually_exclusive_indicator_regions),
                                   std::cend(mutually_exclusive_indicator_regions),
                                   [&] (const auto& region) {
                                       if (test_tree.num_haplotypes() < policies_.haplotype_limits.target) {
                                           return true;
                                       }
                                       test_tree.clear(region);
                                       return false;
                                   });
            mutually_exclusive_indicator_regions.erase(std::cbegin(mutually_exclusive_indicator_regions), it);
        }
        unsigned num_novel_regions_added {0};
        for (const auto& region : mutually_exclusive_novel_regions) {
            const auto interacting_alleles = contained_range(novel_alleles, region);
            const auto it = extend_tree_until(interacting_alleles, test_tree,
                                              policies_.haplotype_limits.overflow);
            if (it != std::cend(interacting_alleles)) {
                test_tree.clear();
                break;
            }
            ++num_novel_regions_added;
            
            if (test_tree.num_haplotypes() > policies_.haplotype_limits.target) {
                // TODO: should we be removing some indicators here?
                if (num_novel_regions_added > 1) {
                    test_tree.clear(region);
                    --num_novel_regions_added;
                    const auto& prev_novel_region = mutually_exclusive_novel_regions[num_novel_regions_added - 1];
                    if (is_empty(prev_novel_region)) {
                        // Watch out for edge case where good insertions also get cleared
                        extend_tree(contained_range(novel_alleles, prev_novel_region), test_tree);
                    }
                }
                break;
            } else if (test_tree.num_haplotypes() == policies_.haplotype_limits.target) {
                break;
            }
        }
        if (!test_tree.is_empty()) {
            assert(num_novel_regions_added > 0);
            next_active_region_ = test_tree.encompassing_region();
        } else {
            next_active_region_ = novel_region; // revert to non-lagged behaviour
        }
        if (*next_active_region_ == active_region_) {
            next_active_region_ = default_walker_.walk(active_region_, reads_, alleles_);
        }
    }
}
    
void HaplotypeGenerator::progress(GenomicRegion to)
{
    if (to == active_region_) return;
    next_active_region_ = std::move(to);
    if (!in_holdout_mode()) {
        if (begins_before(active_region_, *next_active_region_)) {
            auto passed_region = left_overhang_region(active_region_, *next_active_region_);
            const auto passed_alleles = overlap_range(alleles_, passed_region);
            
            if (passed_alleles.empty()) {
                return;
            }
            if (can_remove_entire_passed_region(active_region_, *next_active_region_, passed_alleles)) {
                alleles_.erase_overlapped(passed_region);
                tree_.clear(passed_region);
            } else if (requires_staged_removal(passed_alleles)) {
                // We need to be careful here as insertions adjacent to passed_region are
                // considered overlapped and would be wrongly erased if we erased the whole
                // region. But, we also want to clear all single base alleles left adjacent with
                // next_active_region_, as they have truly been passed.
                
                // This will erase everthing to the left of the adjacent insertion, other than
                // the single base alleles adjacent with next_active_region_.
                const auto first_removal_region = expand_rhs(passed_region, -1);
                alleles_.erase_overlapped(first_removal_region);
                tree_.clear(first_removal_region);
                
                // This will erase the remaining single base alleles in passed_region, but not the
                // insertions in next_active_region_.
                const auto second_removal_region = tail_region(first_removal_region);
                alleles_.erase_overlapped(second_removal_region);
                tree_.clear(second_removal_region);
            } else {
                const auto removal_region = expand_rhs(passed_region, -1);
                alleles_.erase_overlapped(removal_region);
                tree_.clear(removal_region);
            }
        } else if (is_after(*next_active_region_, active_region_)) {
            tree_.clear();
        }
    }
}

bool HaplotypeGenerator::in_holdout_mode() const noexcept
{
    return !active_holdouts_.empty();
}

bool HaplotypeGenerator::can_extract_holdouts(const GenomicRegion& region) const noexcept
{
    return active_holdouts_.size() < policies_.max_holdout_depth;
}

namespace debug
{
    template <typename S, typename Contianer>
    void print_new_holdouts(S&& stream, const Contianer& alleles)
    {
        stream << "Adding " << alleles.size() << " alleles to the holdout stack:" << '\n';
        for (const auto& allele : alleles) {
            stream << allele << '\n';
        }
    }
} // namespace debug

template <typename Range>
unsigned estimate_num_haplotype(const Range& alleles, const GenomicRegion& next_active_region)
{
    return std::exp2(count_overlapped(alleles, next_active_region));
}

template <typename Range>
bool require_more_holdouts(const Range& alleles, const GenomicRegion& next_active_region,
                           const unsigned haplotype_limit)
{
    return !alleles.empty() && estimate_num_haplotype(alleles, next_active_region) > haplotype_limit;
}

void HaplotypeGenerator::extract_holdouts(GenomicRegion next_active_region)
{
    assert(can_extract_holdouts(next_active_region));
    
    auto active_alleles = copy_contained(alleles_, next_active_region);
    
    assert(!active_alleles.empty());
    
    auto active_regions = extract_regions(active_alleles);
    active_regions.erase(std::unique(std::begin(active_regions), std::end(active_regions)),
                         std::end(active_regions));
    
    std::vector<std::pair<GenomicRegion, unsigned>> interaction_counts {};
    interaction_counts.reserve(active_regions.size());
    
    std::transform(std::cbegin(active_regions), std::cend(active_regions),
                   std::back_inserter(interaction_counts),
                   [&active_alleles] (const auto& region) {
                       return std::make_pair(region, count_overlapped(active_alleles, region));
                   });
    
    std::sort(std::begin(interaction_counts), std::end(interaction_counts),
              [] (const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    
    std::deque<Allele> new_holdouts {};
    
    do {
        const auto& chosen_holdout_region = interaction_counts.back().first;
        
        // it is safe to take base iterators as alleles with the same region must be adjacent
        auto contained = bases(contained_range(active_alleles, chosen_holdout_region));
        
        const auto is_holdout = [&chosen_holdout_region] (const auto& allele) {
            return is_same_region(allele, chosen_holdout_region);
        };
        
        const auto it1 = std::find_if(std::cbegin(contained), std::cend(contained), is_holdout);
        assert(it1 != std::cend(contained));
        const auto it2 = std::find_if_not(std::next(it1), std::cend(contained), is_holdout);
        
        active_holdouts_.emplace(it1, it2, chosen_holdout_region);
        new_holdouts.insert(std::end(new_holdouts), it1, it2);
        active_alleles.erase(it1, it2);
        
        next_active_region = default_walker_.walk(head_region(next_active_region), reads_, active_alleles);
        
        interaction_counts.pop_back();
    } while (require_more_holdouts(active_alleles, next_active_region, policies_.haplotype_limits.holdout));
    
    std::sort(std::begin(new_holdouts), std::end(new_holdouts));
    
    if (DEBUG_MODE) {
        logging::DebugLogger log {};
        debug::print_new_holdouts(stream(log), new_holdouts);
    }
    
    if (holdout_region_) {
        holdout_region_ = encompassing_region(*holdout_region_, encompassing_region(new_holdouts));
    } else {
        holdout_region_ = encompassing_region(new_holdouts);
    }
    
    alleles_.erase_all(std::cbegin(new_holdouts), std::cend(new_holdouts));
}

bool HaplotypeGenerator::can_reintroduce_holdouts() const noexcept
{
    return !in_holdout_mode() || !ends_before(active_region_, *holdout_region_)
        || !has_overlapped(alleles_, right_overhang_region(*holdout_region_, active_region_));
}

namespace debug
{
    template <typename S, typename Contianer>
    void print_old_holdouts(S&& stream, const Contianer& alleles)
    {
        stream << "Reintroducing " << alleles.size() << " holdout alleles:" << '\n';
        for (const auto& allele : alleles) {
            stream << allele << '\n';
        }
    }
} // namespace debug

void HaplotypeGenerator::reintroduce_holdouts()
{
    assert(!active_holdouts_.empty());
    
    if (DEBUG_MODE) {
        logging::DebugLogger log {};
        debug::print_old_holdouts(stream(log), active_holdouts_.top().alleles);
    }
    
    splice(active_holdouts_.top().alleles, tree_);
    
    if (ends_before(*holdout_region_, active_region_)) {
        auto extended_region = right_overhang_region(active_region_, *holdout_region_);
        extend_tree(contained_range(alleles_, extended_region), tree_);
    }
    
    alleles_.insert(std::make_move_iterator(std::begin(active_holdouts_.top().alleles)),
                    std::make_move_iterator(std::end(active_holdouts_.top().alleles)));
    
    active_holdouts_.pop();
    
    if (active_holdouts_.empty()) {
        holdout_region_ = boost::none;
    }
}

void HaplotypeGenerator::clear_holdouts() noexcept
{
    active_holdouts_ = decltype(active_holdouts_) {};
    holdout_region_ = boost::none;
}

template <typename Range>
auto sum_indel_sizes(const Range& alleles)
{
    return std::accumulate(std::cbegin(alleles), std::cend(alleles), std::size_t {0},
                           [] (const auto curr, const Allele& allele) {
                               if (is_insertion(allele)) {
                                   return curr + sequence_size(allele);
                               } else if (is_deletion(allele)) {
                                   return curr + region_size(allele);
                               }
                               return curr;
                           });
}

GenomicRegion HaplotypeGenerator::calculate_haplotype_region() const
{
    const auto overlapped = overlap_range(alleles_, active_region_);
    
    // We want to keep haplotypes as small as possible, while allowing sufficient flanking
    // reference sequence for full read re-mapping and alignment (i.e. the read must be
    // contained by the haplotype). Note the sum of the indel sizes may not be sufficient
    // as the candidate generator may not propopse all variation in the original reads.
    const auto additional_padding = 2 * sum_indel_sizes(overlapped) + min_flank_pad_;
    
    if (has_overlapped(reads_.get(), active_region_)) {
        const auto& lhs_read = *leftmost_overlapped(reads_.get(), active_region_);
        const auto& rhs_read = *rightmost_overlapped(reads_.get(), active_region_);
        
        const auto unpadded_region = encompassing_region(lhs_read, rhs_read);
        
        if (mapped_begin(lhs_read) < additional_padding / 2) {
            const auto lhs_padding = mapped_begin(lhs_read);
            const auto rhs_padding = additional_padding - lhs_padding;
            return expand(unpadded_region, lhs_padding, rhs_padding);
        }
        
        return expand(unpadded_region, additional_padding / 2);
    }
    
    return expand(active_region_, additional_padding / 2);
}

// Builder

HaplotypeGenerator::Builder& HaplotypeGenerator::Builder::set_lagging_policy(Policies::Lagging policy) noexcept
{
    policies_.lagging = policy;
    return *this;
}

HaplotypeGenerator::Builder& HaplotypeGenerator::Builder::set_target_limit(unsigned n) noexcept
{
    policies_.haplotype_limits.target   = n;
    if (policies_.haplotype_limits.holdout >= policies_.haplotype_limits.holdout) {
        policies_.haplotype_limits.holdout  = n + 1;
        policies_.haplotype_limits.overflow = n + 1;
    }
    return *this;
}

HaplotypeGenerator::Builder& HaplotypeGenerator::Builder::set_holdout_limit(unsigned n) noexcept
{
    policies_.haplotype_limits.holdout = n;
    return *this;
}

HaplotypeGenerator::Builder& HaplotypeGenerator::Builder::set_overflow_limit(unsigned n) noexcept
{
    policies_.haplotype_limits.overflow = n;
    return *this;
}

HaplotypeGenerator::Builder& HaplotypeGenerator::Builder::set_max_holdout_depth(unsigned n) noexcept
{
    policies_.max_holdout_depth = n;
    return *this;
}

HaplotypeGenerator::Builder& HaplotypeGenerator::Builder::set_min_flank_pad(Haplotype::MappingDomain::Size n) noexcept
{
    min_flank_pad_ = n;
    return *this;
}

HaplotypeGenerator HaplotypeGenerator::Builder::build(const ReferenceGenome& reference,
                                                      const MappableFlatSet<Variant>& candidates,
                                                      const ReadMap& reads) const
{
    return HaplotypeGenerator {reference, candidates, reads, policies_, min_flank_pad_};
}

} // namespace coretools
} // namespace octopus