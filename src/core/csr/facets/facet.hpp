// Copyright (c) 2015-2018 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#ifndef facet_hpp
#define facet_hpp

#include <string>
#include <functional>
#include <memory>
#include <unordered_map>

#include <boost/variant.hpp>

#include "concepts/equitable.hpp"
#include "config/common.hpp"
#include "core/types/haplotype.hpp"
#include "core/tools/read_assigner.hpp"
#include "basics/ploidy_map.hpp"
#include "basics/pedigree.hpp"

namespace octopus { namespace csr {

class Facet : public Equitable<Facet>
{
public:
    using GenotypeMap = std::unordered_map<SampleName, MappableFlatSet<Genotype<Haplotype>>>;
    using SampleSupportMap = std::unordered_map<SampleName, HaplotypeSupportMap>;
    using SampleAmbiguityMap = std::unordered_map<SampleName, AmbiguousReadList>;
    using LocalPloidyMap = std::unordered_map<SampleName, unsigned>;
    
    struct SupportMaps
    {
        SampleSupportMap support;
        SampleAmbiguityMap ambiguous;
    };
    
    using ResultType = boost::variant<std::reference_wrapper<const ReadMap>,
                                      std::reference_wrapper<const SupportMaps>,
                                      std::reference_wrapper<const std::string>,
                                      std::reference_wrapper<const std::vector<std::string>>,
                                      std::reference_wrapper<const Haplotype>,
                                      std::reference_wrapper<const GenotypeMap>,
                                      std::reference_wrapper<const LocalPloidyMap>,
                                      std::reference_wrapper<const octopus::Pedigree>
                                     >;
    
    Facet() = default;
    
    Facet(const Facet&)            = default;
    Facet& operator=(const Facet&) = default;
    Facet(Facet&&)                 = default;
    Facet& operator=(Facet&&)      = default;
    
    virtual ~Facet() = default;
    
    const std::string& name() const noexcept { return do_name(); }
    ResultType get() const { return do_get(); }
    
private:
    virtual const std::string& do_name() const noexcept = 0;
    virtual ResultType do_get() const = 0;
};

bool operator==(const Facet& lhs, const Facet& rhs) noexcept;

class FacetWrapper : public Equitable<FacetWrapper>
{
public:
    FacetWrapper() = delete;
    
    FacetWrapper(std::unique_ptr<Facet> facet) : facet_ {std::move(facet)} {}
    
    FacetWrapper(const FacetWrapper&)            = delete;
    FacetWrapper& operator=(const FacetWrapper&) = delete;
    FacetWrapper(FacetWrapper&&)                 = default;
    FacetWrapper& operator=(FacetWrapper&&)      = default;
    
    ~FacetWrapper() = default;
    
    const Facet* base() const noexcept { return facet_.get(); }
    const std::string& name() const noexcept { return facet_->name(); }
    Facet::ResultType get() const { return facet_->get(); }

private:
    std::unique_ptr<Facet> facet_;
};

bool operator==(const FacetWrapper& lhs, const FacetWrapper& rhs) noexcept;

namespace detail {

template <typename T>
const T& get_value(std::reference_wrapper<const T> value) noexcept
{
    return value.get();
}

template <typename T>
T get_value(const T& value)
{
    return value;
}

} // namespace detail

template <typename F>
decltype(auto) get_value(const FacetWrapper& facet)
{
    return detail::get_value(boost::get<typename F::ResultType>(facet.get()));
}

} // namespace csr
} // namespace octopus

namespace std {

template <> struct hash<octopus::csr::Facet>
{
    size_t operator()(const octopus::csr::Facet& facet) const noexcept
    {
        return hash<std::string>{}(facet.name());
    }
};

template <> struct hash<octopus::csr::FacetWrapper>
{
    size_t operator()(const octopus::csr::FacetWrapper& facet) const noexcept
    {
        return hash<octopus::csr::Facet>{}(*facet.base());
    }
};

} // namespace std

#endif
