// Copyright (c) 2016 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#include "read_manager.hpp"

#include <iterator>
#include <algorithm>
#include <utility>
#include <deque>
#include <numeric>
#include <cassert>

#include <boost/filesystem/operations.hpp>

#include "basics/aligned_read.hpp"
#include "utils/append.hpp"
#include "utils/coverage_tracker.hpp"

namespace octopus { namespace io {

ReadManager::ReadManager(std::vector<Path> read_file_paths, unsigned max_open_files)
: max_open_files_ {max_open_files}
, num_files_ {static_cast<unsigned>(read_file_paths.size())}
, closed_readers_ {
    std::make_move_iterator(std::begin(read_file_paths)),
    std::make_move_iterator(std::end(read_file_paths))}
, open_readers_ {FileSizeCompare {}}
, reader_paths_containing_sample_ {}
, possible_regions_in_readers_ {}
, samples_ {}
{
    setup_reader_samples_and_regions();
    open_initial_files();
    samples_.reserve(reader_paths_containing_sample_.size());
    for (const auto& pair : reader_paths_containing_sample_) {
        samples_.emplace_back(pair.first);
    }
    std::sort(std::begin(samples_), std::end(samples_)); // just for consistency
}

ReadManager::ReadManager(std::initializer_list<Path> read_file_paths)
: ReadManager {std::vector<Path> {read_file_paths}
, static_cast<unsigned>(read_file_paths.size())}
{}

ReadManager::ReadManager(ReadManager&& other)
:
num_files_ {std::move(other.num_files_)}
{
    std::lock_guard<std::mutex> lock {other.mutex_};
    closed_readers_                 = std::move(other.closed_readers_);
    open_readers_                   = std::move(other.open_readers_);
    reader_paths_containing_sample_ = std::move(other.reader_paths_containing_sample_);
    possible_regions_in_readers_    = std::move(other.possible_regions_in_readers_);
    samples_                        = std::move(other.samples_);
}

void swap(ReadManager& lhs, ReadManager& rhs) noexcept
{
    if (&lhs == &rhs) return;
    std::lock(lhs.mutex_, rhs.mutex_);
    std::lock_guard<std::mutex> lock_lhs {lhs.mutex_, std::adopt_lock}, lock_rhs {rhs.mutex_, std::adopt_lock};
    using std::swap;
    swap(lhs.closed_readers_,                 rhs.closed_readers_);
    swap(lhs.open_readers_,                   rhs.open_readers_);
    swap(lhs.reader_paths_containing_sample_, rhs.reader_paths_containing_sample_);
    swap(lhs.possible_regions_in_readers_,    rhs.possible_regions_in_readers_);
    swap(lhs.samples_, rhs.samples_);
}

bool ReadManager::good() const noexcept
{
    return std::all_of(std::cbegin(open_readers_), std::cend(open_readers_),
                       [] (const auto& p) { return p.second.is_open(); });
}

unsigned ReadManager::num_files() const noexcept
{
    return static_cast<unsigned>(closed_readers_.size() + open_readers_.size());
}

unsigned ReadManager::num_samples() const noexcept
{
    return static_cast<unsigned>(samples_.size());
}

const std::vector<ReadManager::SampleName>& ReadManager::samples() const
{
    return samples_;
}

bool ReadManager::has_reads(const SampleName& sample, const GenomicRegion& region) const
{
    return has_reads(std::vector<SampleName> {sample}, region);
}

bool ReadManager::has_reads(const std::vector<SampleName>& samples, const GenomicRegion& region) const
{
    if (all_readers_are_open()) {
        return std::any_of(std::cbegin(open_readers_), std::cend(open_readers_),
                           [&] (const auto& p) { return p.second.has_reads(samples, region); });
    } else {
        std::lock_guard<std::mutex> lock {mutex_};
        auto reader_paths = get_reader_paths_containing_samples(samples);
        auto reader_itr = partition_open(reader_paths);
        while (!reader_paths.empty()) {
            if (std::any_of(reader_itr, end(reader_paths),
                            [this, &region, &samples] (const auto& reader_path) {
                                return open_readers_.at(reader_path).has_reads(samples, region);
                            })) {
                return true;
            }
            reader_paths.erase(reader_itr, end(reader_paths));
            reader_itr = open_readers(begin(reader_paths), end(reader_paths));
        }
        return false;
    }
}

bool ReadManager::has_reads(const GenomicRegion& region) const
{
    if (all_readers_are_open()) {
        return std::any_of(std::cbegin(open_readers_), std::cend(open_readers_),
                           [&] (const auto& p) { return p.second.has_reads(region); });
    } else {
        std::lock_guard<std::mutex> lock {mutex_};
        auto reader_paths = get_reader_paths_containing_samples(samples());
        auto reader_itr = partition_open(reader_paths);
        while (!reader_paths.empty()) {
            if (std::any_of(reader_itr, end(reader_paths),
                            [this, &region] (const auto& reader_path) {
                                return open_readers_.at(reader_path).has_reads(region);
                            })) {
                return true;
            }
            reader_paths.erase(reader_itr, end(reader_paths));
            reader_itr = open_readers(begin(reader_paths), end(reader_paths));
        }
        return false;
    }
}

std::size_t ReadManager::count_reads(const SampleName& sample, const GenomicRegion& region) const
{
    if (all_readers_are_open()) {
        return std::accumulate(std::cbegin(open_readers_), std::cend(open_readers_), std::size_t {0},
                               [&] (std::size_t curr, const auto& p) {
                                   return curr + p.second.count_reads(sample, region);
                               });
    } else {
        std::lock_guard<std::mutex> lock {mutex_};
        auto reader_paths = get_possible_reader_paths({sample}, region);
        auto reader_itr = partition_open(reader_paths);
        std::size_t result {0};
        while (!reader_paths.empty()) {
            using std::begin; using std::end; using std::for_each;
            for_each(reader_itr, end(reader_paths), [this, &sample, &region, &result] (const auto& reader_path) {
                result += open_readers_.at(reader_path).count_reads(sample, region);
            });
            reader_paths.erase(reader_itr, end(reader_paths));
            reader_itr = open_readers(begin(reader_paths), end(reader_paths));
        }
        return result;
    }
}

std::size_t ReadManager::count_reads(const std::vector<SampleName>& samples, const GenomicRegion& region) const
{
    if (all_readers_are_open()) {
        return std::accumulate(std::cbegin(open_readers_), std::cend(open_readers_), std::size_t {0},
                               [&] (std::size_t curr, const auto& p) {
                                   return curr + p.second.count_reads(samples, region);
                               });
    } else {
        std::lock_guard<std::mutex> lock {mutex_};
        auto reader_paths = get_possible_reader_paths(samples, region);
        auto reader_itr = partition_open(reader_paths);
        std::size_t result {0};
        while (!reader_paths.empty()) {
            using std::begin; using std::end; using std::for_each;
            for_each(reader_itr, end(reader_paths), [this, &samples, &region, &result] (const auto& reader_path) {
                result += open_readers_.at(reader_path).count_reads(samples, region);
            });
            reader_paths.erase(reader_itr, end(reader_paths));
            reader_itr = open_readers(begin(reader_paths), end(reader_paths));
        }
        return result;
    }
}

std::size_t ReadManager::count_reads(const GenomicRegion& region) const
{
    return count_reads(samples(), region);
}

GenomicRegion ReadManager::find_covered_subregion(const SampleName& sample, const GenomicRegion& region,
                                                  const std::size_t max_reads) const
{
    return find_covered_subregion(std::vector<SampleName> {sample}, region, max_reads);
}

namespace {

void add(GenomicRegion::Position p, CoverageTracker<ContigRegion>& position_tracker)
{
    position_tracker.add(ContigRegion {p, p + 1});
}

auto max_head_region(const CoverageTracker<ContigRegion>& position_tracker, const GenomicRegion& region)
{
    const auto tracker_region = position_tracker.encompassing_region();
    if (tracker_region) {
        if (is_before(*tracker_region, region.contig_region()) || ends_before(region.contig_region(), *tracker_region)) {
            return region;
        } else {
            return GenomicRegion {region.contig_name(), closed_region(region.contig_region(), *tracker_region)};
        }
    } else {
        return region;
    }
}

auto max_head_region(const CoverageTracker<ContigRegion>& position_tracker,
                     const GenomicRegion& region, const std::size_t max_coverage)
{
    if (position_tracker.is_empty()) return region;
    const auto max_region = max_head_region(position_tracker, region);
    if (position_tracker.total_coverage(max_region.contig_region()) <= max_coverage) return max_region;
    auto position_coverage = position_tracker.coverage(max_region.contig_region());
    std::partial_sum(std::begin(position_coverage), std::end(position_coverage), std::begin(position_coverage));
    const auto last_position = std::upper_bound(std::cbegin(position_coverage), std::cend(position_coverage), max_coverage);
    return expand_rhs(max_region, std::distance(std::cbegin(position_coverage), last_position));
}

} // namespace

GenomicRegion ReadManager::find_covered_subregion(const std::vector<SampleName>& samples, const GenomicRegion& region,
                                                  const std::size_t max_reads) const
{
    if (samples.empty() || is_empty(region)) return region;
    CoverageTracker<ContigRegion> position_tracker {};
    if (all_readers_are_open()) {
        bool added_tail_position {false};
        for (const auto& p : open_readers_) {
            // Request one more than the max so we can determine if the entire request region can be included
            const auto positions = p.second.extract_read_positions(samples, region, max_reads + 1);
            for (auto position : positions) {
                add(position, position_tracker);
            }
            if (positions.size() <= max_reads && !added_tail_position) {
                position_tracker.add(tail_position(region).contig_region());
                added_tail_position = true;
            }
        }
    } else {
        std::lock_guard<std::mutex> lock {mutex_};
        auto reader_paths = get_possible_reader_paths(samples, region);
        auto reader_itr = partition_open(reader_paths);
        while (!reader_paths.empty()) {
            bool added_tail_position {false};
            std::for_each(reader_itr, end(reader_paths), [&] (const auto& reader_path) {
                // Request one more than the max so we can determine if the entire request region can be included
                const auto positions = open_readers_.at(reader_path).extract_read_positions(samples, region, max_reads + 1);
                for (auto position : positions) {
                    add(position, position_tracker);
                }
                if (positions.size() <= max_reads && !added_tail_position) {
                    position_tracker.add(tail_position(region).contig_region());
                    added_tail_position = true;
                }
            });
            reader_paths.erase(reader_itr, end(reader_paths));
            reader_itr = open_readers(begin(reader_paths), end(reader_paths));
        }
    }
    return max_head_region(position_tracker, region, max_reads);
}

GenomicRegion ReadManager::find_covered_subregion(const GenomicRegion& region, const std::size_t max_reads) const
{
    return find_covered_subregion(samples(), region, max_reads);
}

namespace {

template <typename Container>
void merge_insert(Container&& src, Container& dst)
{
    auto itr = octopus::utils::append(std::move(src), dst);
    std::inplace_merge(std::begin(dst), itr, std::end(dst));
}

} // namespace

ReadManager::ReadContainer ReadManager::fetch_reads(const SampleName& sample, const GenomicRegion& region) const
{
    ReadContainer result {};
    if (all_readers_are_open()) {
        for (const auto& p : open_readers_) {
            merge_insert(p.second.fetch_reads(sample, region), result);
        }
    } else {
        std::lock_guard<std::mutex> lock {mutex_};
        auto reader_paths = get_possible_reader_paths({sample}, region);
        auto reader_itr = partition_open(reader_paths);
        while (!reader_paths.empty()) {
            using std::begin; using std::end; using std::make_move_iterator; using std::for_each;
            for_each(reader_itr, end(reader_paths), [&] (const auto& reader_path) {
                merge_insert(open_readers_.at(reader_path).fetch_reads(sample, region), result);
            });
            reader_paths.erase(reader_itr, end(reader_paths));
            reader_itr = open_readers(begin(reader_paths), end(reader_paths));
        }
    }
    return result;
}

ReadManager::SampleReadMap ReadManager::fetch_reads(const std::vector<SampleName>& samples, const GenomicRegion& region) const
{
    SampleReadMap result {samples.size()};
    // Populate here so we can do unchcked access
    for (const auto& sample : samples) {
        result.emplace(std::piecewise_construct, std::forward_as_tuple(sample), std::forward_as_tuple());
    }
    if (all_readers_are_open()) {
        for (const auto& p : open_readers_) {
            auto reads = p.second.fetch_reads(samples, region);
            for (auto&& r : reads) {
                merge_insert(std::move(r.second), result.at(r.first));
                r.second.clear();
                r.second.shrink_to_fit();
            }
        }
    } else {
        std::lock_guard<std::mutex> lock {mutex_};
        auto reader_paths = get_possible_reader_paths(samples, region);
        auto reader_itr = partition_open(reader_paths);
        while (!reader_paths.empty()) {
            using std::begin; using std::end; using std::make_move_iterator; using std::for_each;
            for_each(reader_itr, end(reader_paths), [&] (const auto& reader_path) {
                auto reads = open_readers_.at(reader_path).fetch_reads(samples, region);
                for (auto&& r : reads) {
                    merge_insert(std::move(r.second), result.at(r.first));
                    r.second.clear();
                    r.second.shrink_to_fit();
                }
            });
            reader_paths.erase(reader_itr, end(reader_paths));
            reader_itr = open_readers(begin(reader_paths), end(reader_paths));
        }
    }
    return result;
}

ReadManager::SampleReadMap ReadManager::fetch_reads(const GenomicRegion& region) const
{
    return fetch_reads(samples(), region);
}

// Private methods

bool ReadManager::FileSizeCompare::operator()(const Path& lhs, const Path& rhs) const
{
    return boost::filesystem::file_size(lhs) < boost::filesystem::file_size(rhs);
}

namespace {

auto extract_spanning_regions(std::vector<GenomicRegion::ContigName> contigs,
                              const ReadReader& reader)
{
    std::vector<GenomicRegion> result {};
    result.reserve(contigs.size());
    for (auto&& contig : contigs) {
        result.emplace_back(std::move(contig), 0, reader.reference_size(contig));
    }
    return result;
}

} // namespace

void ReadManager::setup_reader_samples_and_regions()
{
    for (const auto& reader_path : closed_readers_) {
        auto reader = make_reader(reader_path);
        auto possible_reader_regions = reader.mapped_regions();
        if (possible_reader_regions) {
            add_possible_regions_to_reader_map(reader_path, *possible_reader_regions);
        } else {
            auto possible_reader_contigs = reader.mapped_contigs();
            if (possible_reader_contigs) {
                add_possible_regions_to_reader_map(reader_path, extract_spanning_regions(*possible_reader_contigs, reader));
            } else {
                add_possible_regions_to_reader_map(reader_path, extract_spanning_regions(reader.reference_contigs(), reader));
            }
        }
        add_reader_to_sample_map(reader_path, reader.extract_samples());
    }
}

void ReadManager::open_initial_files()
{
    using std::begin; using std::end; using std::cbegin; using std::cend;
    std::vector<Path> reader_paths {cbegin(closed_readers_), cend(closed_readers_)};
    auto num_files_to_open = std::min(max_open_files_, static_cast<unsigned>(closed_readers_.size()));
    std::nth_element(begin(reader_paths), begin(reader_paths) + num_files_to_open, end(reader_paths),
                     FileSizeCompare {});
    open_readers(begin(reader_paths), begin(reader_paths) + num_files_to_open);
}

ReadReader ReadManager::make_reader(const Path& reader_path) const
{
    return ReadReader {reader_path};
}

bool ReadManager::all_readers_are_open() const noexcept
{
    assert(open_readers_.size() == num_files_ || num_files_ > max_open_files_);
    return num_files_ <= max_open_files_;
}

bool ReadManager::is_open(const Path& reader_path) const noexcept
{
    return open_readers_.count(reader_path) == 1;
}

std::vector<ReadManager::Path>::iterator ReadManager::partition_open(std::vector<Path>& reader_paths) const
{
    return std::partition(std::begin(reader_paths), std::end(reader_paths),
                          [this] (const Path& path) { return !is_open(path); });
}

unsigned ReadManager::num_open_readers() const noexcept
{
    return static_cast<unsigned>(open_readers_.size());
}

unsigned ReadManager::num_reader_spaces() const noexcept
{
    return max_open_files_ - num_open_readers();
}

void ReadManager::open_reader(const Path& reader_path) const
{
    if (num_open_readers() == max_open_files_) { // do we need this?
        close_reader(choose_reader_to_close());
    }
    open_readers_.emplace(reader_path, make_reader(reader_path));
    closed_readers_.erase(reader_path);
}

std::vector<ReadManager::Path>::iterator
ReadManager::open_readers(std::vector<Path>::iterator first, std::vector<Path>::iterator last) const
{
    if (first == last) return first;
    auto num_available_spaces = num_reader_spaces();
    auto num_requested_spaces = static_cast<unsigned>(std::distance(first, last));
    if (num_requested_spaces <= num_available_spaces) {
        std::for_each(first, last, [this] (const Path& path) { open_reader(path); });
        return first;
    }
    auto num_readers_to_close = std::min(num_open_readers(), num_requested_spaces - num_available_spaces);
    close_readers(num_readers_to_close);
    num_available_spaces += num_readers_to_close;
    // partition range so opened readers come last
    auto first_open = std::next(first, num_requested_spaces - num_available_spaces);
    std::for_each(first_open, last, [this] (const Path& path) { open_reader(path); });
    return first_open;
}

void ReadManager::close_reader(const Path& reader_path) const
{
    // TODO: we can make use of IReadReaderImpl::close to avoid calling deconstructor on the file.
    open_readers_.erase(reader_path);
    closed_readers_.insert(reader_path);
}

ReadManager::Path ReadManager::choose_reader_to_close() const
{
    return open_readers_.begin()->first; // i.e. smallest file size
}

void ReadManager::close_readers(unsigned n) const
{
    for (; n > 0; --n) {
        close_reader(choose_reader_to_close());
    }
}

void ReadManager::add_possible_regions_to_reader_map(const Path& reader_path, const std::vector<GenomicRegion>& regions)
{
    for (const auto& region : regions) {
        possible_regions_in_readers_[reader_path][region.contig_name()].emplace(region.contig_region());
    }
}

bool ReadManager::could_reader_contain_region(const Path& reader_path, const GenomicRegion& region) const
{
    if (possible_regions_in_readers_.count(reader_path) == 0) return false;
    if (possible_regions_in_readers_.at(reader_path).count(region.contig_name()) == 0) return false;
    
    return has_overlapped(possible_regions_in_readers_.at(reader_path).at(region.contig_name()),
                          region.contig_region());
}

std::vector<ReadManager::Path>
ReadManager::get_possible_reader_paths(const GenomicRegion& region) const
{
    std::vector<Path> result {};
    result.reserve(num_files_);
    for (const auto& reader_path : closed_readers_) {
        if (could_reader_contain_region(reader_path, region)) {
            result.emplace_back(reader_path);
        }
    }
    for (const auto& reader : open_readers_) {
        if (could_reader_contain_region(reader.first, region)) {
            result.emplace_back(reader.first);
        }
    }
    return result;
}

void ReadManager::add_reader_to_sample_map(const Path& reader_path, const std::vector<SampleName>& samples_in_reader)
{
    for (const auto& sample : samples_in_reader) {
        reader_paths_containing_sample_[sample].emplace_back(reader_path);
    }
}

std::vector<ReadManager::Path>
ReadManager::get_reader_paths_containing_samples(const std::vector<SampleName>& samples) const
{
    std::unordered_set<Path, PathHash> unique_reader_paths {};
    unique_reader_paths.reserve(num_files_);
    for (const auto& sample : samples) {
        for (const auto& reader_path : reader_paths_containing_sample_.at(sample)) {
            unique_reader_paths.emplace(reader_path);
        }
    }
    return std::vector<Path> {std::begin(unique_reader_paths), std::end(unique_reader_paths)};
}

std::vector<ReadManager::Path>
ReadManager::get_possible_reader_paths(const std::vector<SampleName>& samples, const GenomicRegion& region) const
{
    auto result = get_reader_paths_containing_samples(samples);
    auto it = std::remove_if(std::begin(result), std::end(result),
                             [this, &region] (const Path& path) {
                                 return !could_reader_contain_region(path, region);
                             });
    result.erase(it, std::end(result));
    return result;
}

} // namespace io
} // namespace octopus
