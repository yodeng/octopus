// Copyright (c) 2015-2018 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#ifndef hiseq_snv_error_model_hpp
#define hiseq_snv_error_model_hpp

#include <vector>
#include <array>
#include <cstdint>

#include "snv_error_model.hpp"

namespace octopus {

class Haplotype;

class HiSeqSnvErrorModel : public SnvErrorModel
{
public:
    using SnvErrorModel::MutationVector;
    using SnvErrorModel::PenaltyType;
    using SnvErrorModel::PenaltyVector;
    
    HiSeqSnvErrorModel() = default;
    
    HiSeqSnvErrorModel(const HiSeqSnvErrorModel&)            = default;
    HiSeqSnvErrorModel& operator=(const HiSeqSnvErrorModel&) = default;
    HiSeqSnvErrorModel(HiSeqSnvErrorModel&&)                 = default;
    HiSeqSnvErrorModel& operator=(HiSeqSnvErrorModel&&)      = default;
    
    virtual ~HiSeqSnvErrorModel() = default;

private:
    static constexpr std::array<std::array<PenaltyType, 51>, 3> maxQualities_ =
    {{
     {
     125, 125, 60, 55, 50, 30, 20, 15, 12, 12, 10, 10, 10, 10, 8, 7, 6, 6, 6, 6,
     6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1
     },
     {
     125, 125, 60, 60, 52, 52, 38, 38, 22, 22, 17, 17, 15, 15, 13, 13, 10, 10, 10, 10,
     8, 8, 7, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1
     },
     {
     125, 125, 125, 55, 55, 55, 40, 40, 40, 25, 25, 25, 19, 19, 19, 11, 11, 11, 9, 9,
     9, 7, 7, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1
     }
     }};
    
    virtual std::unique_ptr<SnvErrorModel> do_clone() const override;
    virtual void do_evaluate(const Haplotype& haplotype,
                             MutationVector& forward_snv_mask, PenaltyVector& forward_snv_priors,
                             MutationVector& reverse_snv_mask, PenaltyVector& reverse_snv_priors) const override ;
};
    
} // namespace octopus

#endif
