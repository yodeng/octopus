//
//  read_filters.h
//  Octopus
//
//  Created by Daniel Cooke on 07/03/2015.
//  Copyright (c) 2015 Oxford University. All rights reserved.
//

#ifndef __Octopus__read_filters__
#define __Octopus__read_filters__

#include <string>
#include <utility>
#include <algorithm>
#include <iterator>

#include "aligned_read.hpp"
#include "cigar_string.hpp"

namespace Octopus { namespace ReadFilters {

// All filters are nameable

class Nameable
{
    std::string name_;
    
public:
    Nameable() = delete;
    Nameable(std::string name) : name_ {std::move(name)} {}
    
    const std::string& name() const noexcept
    {
        return name_;
    }
};

// Basic filters

class BasicReadFilter : public Nameable
{
public:
    BasicReadFilter() = delete;
    
    virtual ~BasicReadFilter() = default;
    
    bool operator()(const AlignedRead& read) const noexcept
    {
        return passes(read);
    }
    
protected:
    BasicReadFilter(std::string name) : Nameable {std::move(name)} {};
    
private:
    virtual bool passes(const AlignedRead&) const noexcept = 0;
};

struct IsNotSecondaryAlignment : BasicReadFilter
{
    IsNotSecondaryAlignment();
    explicit IsNotSecondaryAlignment(std::string name);
    
    bool passes(const AlignedRead& read) const noexcept override;
};

struct IsNotSupplementaryAlignment : BasicReadFilter
{
    IsNotSupplementaryAlignment();
    IsNotSupplementaryAlignment(std::string name);
    
    bool passes(const AlignedRead& read) const noexcept override;
};

struct IsGoodMappingQuality : BasicReadFilter
{
    using QualityType = AlignedRead::QualityType;
    
    IsGoodMappingQuality() = delete;
    
    explicit IsGoodMappingQuality(QualityType good_mapping_quality);
    explicit IsGoodMappingQuality(std::string name, QualityType good_mapping_quality);
    
    bool passes(const AlignedRead& read) const noexcept override;
    
private:
    QualityType good_mapping_quality_;
};

struct HasSufficientGoodBaseFraction : BasicReadFilter
{
    using QualityType = AlignedRead::QualityType;
    
    HasSufficientGoodBaseFraction() = delete;
    explicit HasSufficientGoodBaseFraction(QualityType good_base_quality,
                                           double min_good_base_fraction);
    explicit HasSufficientGoodBaseFraction(std::string name,
                                           QualityType good_base_quality,
                                           double min_good_base_fraction);
    
    bool passes(const AlignedRead& read) const noexcept override;
    
private:
    QualityType good_base_quality_;
    double min_good_base_fraction_;
};

struct HasSufficientGoodQualityBases : BasicReadFilter
{
    using QualityType = AlignedRead::QualityType;
    
    HasSufficientGoodQualityBases() = delete;
    explicit HasSufficientGoodQualityBases(QualityType good_base_quality,
                                           unsigned min_good_bases);
    explicit HasSufficientGoodQualityBases(std::string name,
                                           QualityType good_base_quality,
                                           unsigned min_good_bases);
    
    bool passes(const AlignedRead& read) const noexcept override;
    
private:
    QualityType good_base_quality_;
    unsigned min_good_bases_;
};

struct IsMapped : BasicReadFilter
{
    IsMapped();
    explicit IsMapped(std::string name);
    
    bool passes(const AlignedRead& read) const noexcept override;
};

struct IsNotChimeric : BasicReadFilter
{
    IsNotChimeric();
    explicit IsNotChimeric(std::string name);
    
    bool passes(const AlignedRead& read) const noexcept override;
};

struct IsNextSegmentMapped : BasicReadFilter
{
    IsNextSegmentMapped();
    explicit IsNextSegmentMapped(std::string name);
    
    bool passes(const AlignedRead& read) const noexcept override;
};

struct IsNotMarkedDuplicate : BasicReadFilter
{
    IsNotMarkedDuplicate();
    explicit IsNotMarkedDuplicate(std::string name);
    
    bool passes(const AlignedRead& read) const noexcept override;
};

struct IsShort : BasicReadFilter
{
    using SizeType = AlignedRead::SizeType;
    
    IsShort() = delete;
    explicit IsShort(SizeType max_length);
    explicit IsShort(std::string name, SizeType max_length);
    
    bool passes(const AlignedRead& read) const noexcept override;

private:
    SizeType max_length_;
};

struct IsLong : BasicReadFilter
{
    using SizeType = AlignedRead::SizeType;
    
    IsLong() = delete;
    explicit IsLong(SizeType min_length);
    explicit IsLong(std::string name, SizeType min_length);
    
    bool passes(const AlignedRead& read) const noexcept override;
    
private:
    SizeType min_length_;
};

struct IsNotContaminated : BasicReadFilter
{
    IsNotContaminated();
    explicit IsNotContaminated(std::string name);
    
    bool passes(const AlignedRead& read) const noexcept override;
};

struct IsNotMarkedQcFail : BasicReadFilter
{
    IsNotMarkedQcFail();
    explicit IsNotMarkedQcFail(std::string name);
    
    bool passes(const AlignedRead& read) const noexcept override;
};

// Context filters

template <typename BidirIt>
class ContextReadFilter : public Nameable
{
public:
    ContextReadFilter() = delete;
    
    virtual ~ContextReadFilter() = default;
    
    BidirIt remove(BidirIt first, BidirIt last) const
    {
        return do_remove(first, last);
    }
    
    BidirIt partition(BidirIt first, BidirIt last) const
    {
        return do_partition(first, last);
    }
    
protected:
    ContextReadFilter(std::string name) : Nameable {std::move(name)} {};
    
private:
    virtual BidirIt do_remove(BidirIt first, BidirIt last) const = 0;
    virtual BidirIt do_partition(BidirIt first, BidirIt last) const = 0;
};

template <typename ForwardIt>
struct IsNotDuplicate : ContextReadFilter<ForwardIt>
{
    IsNotDuplicate() : ContextReadFilter<ForwardIt> {"IsNotOctopusDuplicate"} {}
    explicit IsNotDuplicate(std::string name)
    : ContextReadFilter<ForwardIt> {std::move(name)} {}
    
    ForwardIt do_remove(ForwardIt first, ForwardIt last) const override
    {
        return std::unique(first, last, IsDuplicate {});
    }
    
    ForwardIt do_partition(ForwardIt first, ForwardIt last) const override
    {
        // TODO: we need a clever stable_partition_unique implementation.
        // See my question:
        // http://stackoverflow.com/questions/36888033/implementing-partition-unique-and-stable-partition-unique-algorithms
        return last;
    }
};

} // namespace ReadFilters
} // namespace Octopus

#endif /* defined(__Octopus__read_filters__) */
