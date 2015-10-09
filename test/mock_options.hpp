//
//  mock_options.h
//  Octopus
//
//  Created by Daniel Cooke on 14/09/2015.
//  Copyright (c) 2015 Oxford University. All rights reserved.
//

#ifndef Octopus_mock_options_h
#define Octopus_mock_options_h

#include <boost/program_options.hpp>

#include "program_options.hpp"
#include "test_common.hpp"

namespace po = boost::program_options;

inline po::variables_map get_basic_mock_options()
{
    const char *argv[] = {"octopus",
        "--reference", human_reference_fasta.c_str(),
        
        //"--reads", NA12878_low_coverage.string().c_str(), HG00101.string().c_str(), HG00102.string().c_str(), HG00103.string().c_str(),
        "--reads", NA12878_high_coverage.string().c_str(), HG00102.string().c_str(),
        
        "--model", "cancer", // default "population"
        "--normal-sample", "NA12878", // for cancer model
        "--ploidy", "2",
        //"--make-blocked-refcalls",
        //"--make-positional-refcalls",
        
        //"--regions", "5:157,031,410-157,031,449",
        //"--regions", "11:67503118-67503253",
        
        //"--regions", "2:104,142,854-104,142,925", // population caller fails here
        //"--regions", "2:104,142,897-104,142,936", // but is ok here
        //"--regions", "2:104142870-104142984", // and here
        
        //"--regions", "2:142376817-142376922",
        //"--regions", "20",
        //"--regions", "8:94,786,581-94,786,835", // empty region
        //"--regions", "5:157,031,410-157,031,449", "8:94,786,581-94,786,835",
        //"--regions", "5:157,031,227-157,031,282",
        //"--regions", "5:157,031,211-157,031,269",
        //"--regions", "5:157,030,955-157,031,902",
        //"--regions", "5:157,031,214-157,031,280",
        //"--regions", "2:104,142,525-104,142,742",
        //"--regions", "2:104,141,138-104,141,448", // really nice example of phasing
        //"--regions", "4:141,265,067-141,265,142", // another nice example of phasing
        //"--regions", "7:58,000,994-58,001,147", // very interesting example
        //"--regions", "7:103,614,916-103,615,058",
        //"--regions", "13:28,265,032-28,265,228", // cool phasing region
        //"--regions", "13:96,095,387-96,095,455",
        //"--regions", "11:81,266,010-81,266,122", // all homo-alt
        //"--regions", "11:81,266,084-81,266,123",
        
        //"--regions", "16:9,299,984-9,300,090", // complex indels
        "--regions", "13:96,733,039-96,733,124", // complex indels
        //"--regions", "13:96732801-96733349",
        
        //"--regions", "7:122579662-122579817", // complex indels (unverified)
        
        //"--regions", "16:9,378,560-9,378,687", // very complex indel region
        
        //"--regions", "16:62,646,838-62,647,242", // complex phasable insertion and SNPs (in NA12878-HC)
        
        //"--regions", "MT:11,669-11,768", // very high coverage snp
        //"--regions", "MT",
        
        "--min-variant-posterior", "5",
        "--min-refcall-posterior", "1",
        "--min-somatic-posterior", "2",
        
        "--output", test_out_vcf.c_str(),
        
        "--min-supporting-reads", "2",
        "--min-mapping-quality", "20",
        "--min-snp-base-quality", "30",
        "--tail-trim-size", "3",
        "--trim-soft-clipped",
        "--remove-duplicate-reads",
        "--reference-cache-size", "20000",
        nullptr};
    
    int argc = sizeof(argv) / sizeof(char*) - 1;
    
    return Octopus::Options::parse_options(argc, argv);
}

#endif
