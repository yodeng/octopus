#!/usr/bin/env python3

import argparse
from os import makedirs, remove
from os.path import join, basename, exists
from subprocess import call
import csv
from pysam import VariantFile
import random
import numpy as np

default_measures = "AC AD AF ARF BQ CC CRF DP FRF GC GQ GQD NC MC MF MP MRC MQ MQ0 MQD PP PPD QD QUAL REFCALL REB RSB RTB SB SD SF SHC SMQ SOMATIC STR_LENGTH STR_PERIOD".split()

def run_octopus(octopus, ref_path, bam_path, regions_bed, measures, threads, out_path):
    call([octopus, '-R', ref_path, '-I', bam_path, '-t', regions_bed, '-o', out_path, '--threads', str(threads),
          '--legacy', '--csr-train'] + measures)

def get_reference_id(ref_path):
    return basename(ref_path).replace(".fasta", "")

def get_bam_id(bam_path):
    return basename(bam_path).replace(".bam", "")

def call_variants(octopus, ref_path, bam_path, regions_bed, measures, threads, out_dir):
    ref_id = get_reference_id(ref_path)
    bam_id = get_bam_id(bam_path)
    out_vcf = join(out_dir, "octopus." + bam_id + "." + ref_id + ".vcf.gz")
    run_octopus(octopus, ref_path, bam_path, regions_bed, measures, threads, out_vcf)
    legacy_vcf = out_vcf.replace(".vcf.gz", ".legacy.vcf.gz")
    return legacy_vcf

def run_rtg(rtg, rtg_ref_path, truth_vcf_path, confident_bed_path, octopus_vcf_path, out_dir):
    call([rtg, 'vcfeval', '-b', truth_vcf_path, '-t', rtg_ref_path, '--evaluation-regions', confident_bed_path,
          '--ref-overlap', '-c', octopus_vcf_path, '-o', out_dir])

def eval_octopus(octopus, ref_path, bam_path, regions_bed, measures, threads,
                 rtg, rtg_ref_path, truth_vcf_path, confident_bed_path, out_dir):
    octopus_vcf = call_variants(octopus, ref_path, bam_path, regions_bed, measures, threads, out_dir)
    rtf_eval_dir = join(out_dir, basename(octopus_vcf).replace(".legacy.vcf.gz", ".eval"))
    run_rtg(rtg, rtg_ref_path, truth_vcf_path, confident_bed_path, octopus_vcf, rtf_eval_dir)
    return rtf_eval_dir

def subset(vcf_in_path, vcf_out_path, bed_regions):
    call(['bcftools', 'view', '-R', bed_regions, '-O', 'z', '-o', vcf_out_path, vcf_in_path])

def get_annotation(field, rec):
    if field == 'QUAL':
        return rec.qual
    elif field == 'GQ':
        return rec.samples[0]['GQ']
    else:
        res = rec.info[field]
        if type(res) == tuple:
            res = res[0]
        return res

def is_missing(x):
    return x == '.' or np.isnan(float(x))

def annotation_to_string(x, missing_value):
    return str(missing_value) if is_missing(x) else str(x)

def make_ranger_data(octopus_vcf_path, out_path, classifcation, measures, missing_value=-1):
    vcf = VariantFile(octopus_vcf_path)
    with open(out_path, 'w') as ranger_data:
        datwriter = csv.writer(ranger_dat, delimiter=' ')
        for rec in vcf:
            row = [annotation_to_string(get_annotation(measure, rec), missing_value) for measure in measures]
            row.append(str(int(classifcation)))
            datwriter.writerow(row)

def concat(filenames, outpath):
    with open(outpath, 'w') as outfile:
        for fname in filenames:
            with open(fname) as infile:
                for line in infile:
                    outfile.write(line)

def shuffle(fname):
    lines = open(fname).readlines()
    random.shuffle(lines)
    open(fname, 'w').writelines(lines)

def add_header(fname, header):
    lines = open(fname).readlines()
    with open(fname, 'w') as f:
        f.write(header + '\n')
        f.writelines(lines)

def run_ranger_training(ranger, data_path, n_trees, min_node_size, threads, out):
    call([ranger, '--file', data_path, '--depvarname', 'TP', '--probability',
          '--ntree', str(n_trees), '--targetpartitionsize', str(min_node_size),
          '--nthreads', str(threads), '--outprefix', out, '--write', '--verbose'])

def main(options):
    if not exists(options.out):
        makedirs(options.out)
    rtg_eval_dirs = []
    for bam_path in options.reads:
        rtg_eval_dirs.append(eval_octopus(options.octopus, options.reference, bam_path, options.regions, options.measures,
                                          options.threads, options.rtg, options.sdf, options.truth, options.confident,
                                          options.out))
    data_paths = []
    tmp_paths = []
    for rtg_eval in rtg_eval_dirs:
        tp_vcf_path = join(rtg_eval, "tp.vcf.gz")
        tp_train_vcf_path = tp_vcf_path.replace("tp.vcf", "tp.train.vcf")
        subset(tp_vcf_path, tp_train_vcf_path, options.regions)
        tp_data_path = tp_train_vcf_path.replace(".vcf.gz", ".dat")
        make_ranger_data(tp_train_vcf_path, tp_data_path, True, options.measures, options.missing_value)
        data_paths.append(tp_data_path)
        fp_vcf_path = join(rtg_eval, "fp.vcf.gz")
        fp_train_vcf_path = fp_vcf_path.replace("fp.vcf", "fp.train.vcf")
        subset(fp_vcf_path, fp_train_vcf_path, options.regions)
        fp_data_path = fp_train_vcf_path.replace(".vcf.gz", ".dat")
        make_ranger_data(fp_train_vcf_path, fp_data_path, False, options.measures, options.missing_value)
        data_paths.append(fp_data_path)
        tmp_paths += [tp_train_vcf_path, fp_train_vcf_path]
    master_data_path = join(options.out, options.prefix + ".dat")
    concat(data_paths, master_data_path)
    for path in data_paths:
        remove(path)
    for path in tmp_paths:
        remove(path)
    shuffle(master_data_path)
    ranger_header = ' '.join(options.measures + ['TP'])
    add_header(master_data_path, ranger_header)
    ranger_out_prefix = join(options.out, options.prefix)
    run_ranger_training(options.ranger, master_data_path, options.trees, options.min_node_size, options.threads, ranger_out_prefix)
    remove(ranger_out_prefix + ".confusion")

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-R', '--reference',
                        type=str,
                        required=True,
                        help='Reference to use for calling')
    parser.add_argument('-I', '--reads',
                        nargs='+',
                        type=str,
                        required=True,
                        help='Input BAM files')
    parser.add_argument('-T', '--regions',
                        type=str,
                        required=True,
                        help='BED files containing regions to call')
    parser.add_argument('--measures',
                        type=str,
                        nargs='+',
                        default=default_measures,
                        help='Measures to use for training')
    parser.add_argument('--truth',
                        type=str,
                        required=True,
                        help='Truth VCF file')
    parser.add_argument('--confident',
                        type=str,
                        required=True,
                        help='BED files containing high confidence truth regions')
    parser.add_argument('--octopus',
                        type=str,
                        required=True,
                        help='Octopus binary')
    parser.add_argument('--rtg', 
                        type=str,
                        required=True,
                        help='RTG Tools binary')
    parser.add_argument('--sdf',
                        type=str,
                        required=True,
                        help='RTG Tools SDF reference index')
    parser.add_argument('--ranger', 
                        type=str,
                        required=True,
                        help='Ranger binary')
    parser.add_argument('--trees',
                        type=int,
                        default=300,
                        help='Number of trees to use in the random forest')
    parser.add_argument('--min_node_size',
                        type=int,
                        default=20,
                        help='Node size to stop growing trees, implicitly limiting tree depth')
    parser.add_argument('-o', '--out',
                        type=str,
                        help='Output directory')
    parser.add_argument('--prefix',
                        type=str,
                        default='ranger_octopus',
                        help='Output files prefix')
    parser.add_argument('-t', '--threads',
                        type=int,
                        default=1,
                        help='Number of threads for octopus')
    parser.add_argument('--missing_value',
                        type=float,
                        default=-1,
                        help='Value for missing measures')
    parsed, unparsed = parser.parse_known_args()
    main(parsed)
