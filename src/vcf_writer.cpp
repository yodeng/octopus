//
//  vcf_writer.cpp
//  Octopus
//
//  Created by Daniel Cooke on 29/07/2015.
//  Copyright (c) 2015 Oxford University. All rights reserved.
//

#include "vcf_writer.h"

#include <stdexcept>

#include "vcf_header.h"
#include "vcf_record.h"

VcfWriter::VcfWriter(const fs::path& file_path)
:
file_path_ {file_path},
writer_ {file_path_}
{}

VcfWriter::VcfWriter(const fs::path& file_path, const VcfHeader& header)
:
file_path_ {file_path},
writer_ {file_path_}
{
    write(header);
    is_header_written_ = true;
}

void VcfWriter::write(const VcfHeader& header)
{
    writer_.write(header);
    is_header_written_ = true;
}

void VcfWriter::write(const VcfRecord& record)
{
    if (is_header_written_) {
        write(record);
    } else {
        throw std::runtime_error {"Cannot write VCF record as header has not beein written"};
    }
}