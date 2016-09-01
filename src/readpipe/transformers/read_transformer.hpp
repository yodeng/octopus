// Copyright (c) 2016 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#ifndef read_transformer_hpp
#define read_transformer_hpp

#include <vector>
#include <functional>
#include <algorithm>
#include <iterator>
#include <type_traits>

#include "basics/aligned_read.hpp"

namespace octopus { namespace readpipe
{

class ReadTransformer
{
public:
    using ReadTransform = std::function<void(AlignedRead&)>;
    
    ReadTransformer() = default;
    
    ReadTransformer(const ReadTransformer&)            = default;
    ReadTransformer& operator=(const ReadTransformer&) = default;
    ReadTransformer(ReadTransformer&&)                 = default;
    ReadTransformer& operator=(ReadTransformer&&)      = default;
    
    ~ReadTransformer() = default;
    
    void register_transform(ReadTransform transform);
    
    unsigned num_transforms() const noexcept;
    
    void shrink_to_fit() noexcept;
    
    template <typename InputIt>
    void transform_reads(InputIt first, InputIt last) const;
    
private:
    std::vector<ReadTransform> transforms_;
    
    void transform_read(AlignedRead& read) const;
};

// private methods

template <typename InputIt>
void ReadTransformer::transform_reads(InputIt first, InputIt last) const
{
    std::for_each(first, last, [this] (auto& read) { transform_read(read); });
}

// non-member methods

namespace detail
{
    template <typename Container>
    void transform_reads(Container& reads, const ReadTransformer& transformer, std::true_type)
    {
        transformer.transform_reads(std::begin(reads), std::end(reads));
    }
    
    template <typename ReadMap>
    void transform_reads(ReadMap& reads, const ReadTransformer& transformer, std::false_type)
    {
        for (auto& p : reads) {
            transform_reads(p.second, transformer, std::true_type {});
        }
    }
} // namespace detail

template <typename Container>
void transform_reads(Container& reads, const ReadTransformer& transformer)
{
    detail::transform_reads(reads, transformer, std::is_same<typename Container::value_type, AlignedRead> {});
}

} // namespace readpipe
} // namespace octopus

#endif
