//
//  caching_fasta.h
//  Octopus
//
//  Created by Daniel Cooke on 22/07/2015.
//  Copyright (c) 2015 Oxford University. All rights reserved.
//

#ifndef __Octopus__caching_fasta__
#define __Octopus__caching_fasta__

#include <unordered_map>
#include <map>
#include <list>
#include <cstddef>
#include <boost/filesystem/path.hpp>

#include "i_reference_genome_impl.h"
#include "fasta.h"

namespace fs = boost::filesystem;

class GenomicRegion;

class CachingFasta : public IReferenceGenomeImpl
{
public:
    using SequenceType = IReferenceGenomeImpl::SequenceType;
    using SizeType     = IReferenceGenomeImpl::SizeType;
    
    CachingFasta() = delete;
    explicit CachingFasta(fs::path fasta_path);
    explicit CachingFasta(fs::path fasta_path, SizeType max_cache_size);
    explicit CachingFasta(fs::path fasta_path, fs::path fasta_index_path);
    explicit CachingFasta(fs::path fasta_path, fs::path fasta_index_path, SizeType max_cache_size);
    ~CachingFasta() noexcept override = default;
    
    CachingFasta(const CachingFasta&)            = default;
    CachingFasta& operator=(const CachingFasta&) = default;
    CachingFasta(CachingFasta&&)                 = default;
    CachingFasta& operator=(CachingFasta&&)      = default;
    
    std::string get_reference_name() const override;
    std::vector<std::string> get_contig_names() override;
    SizeType get_contig_size(const std::string& contig_name) override;
    SequenceType get_sequence(const GenomicRegion& region) override;
    
private:
    Fasta fasta_;
    
    using ContigSequenceCache = std::map<GenomicRegion, SequenceType>;
    using CacheIterator       = ContigSequenceCache::const_iterator;
    
    std::unordered_map<std::string, SizeType> contig_size_cache_;
    std::unordered_map<std::string, ContigSequenceCache> sequence_cache_;
    std::list<GenomicRegion> recently_used_regions_;
    
    SizeType used_cache_size_ = 0;
    const SizeType max_cache_size_ = 1000000;
    
    void setup_cache();
    
    GenomicRegion region_to_fetch(const GenomicRegion& requested_region) const;
    
    bool is_region_cached(const GenomicRegion& region) const;
    void add_sequence_to_cache(const SequenceType& sequence, const GenomicRegion& region);
    void update_cache_position(const GenomicRegion& region);
    CacheIterator get_cache_iterator(const GenomicRegion& requested_region) const;
    std::pair<CacheIterator, CacheIterator> overlap_range(const GenomicRegion& region) const;
    void remove_from_cache(const GenomicRegion& region);
    void recache_overlapped_regions(const SequenceType& sequence, const GenomicRegion& region);
    
    SequenceType get_subsequence(const GenomicRegion& requested_region,
                                 const GenomicRegion& sequence_region,
                                 const SequenceType& sequence) const;
};

#endif /* defined(__Octopus__caching_fasta__) */
