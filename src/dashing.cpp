#include "tinythreadpp/source/fast_mutex.h"
#include <fstream>
#include <omp.h>
#include "bonsai/bonsai/include/util.h"
#include "bonsai/bonsai/include/database.h"
#include "bonsai/bonsai/include/bitmap.h"
#include "bonsai/bonsai/include/setcmp.h"
#include "hll/hll.h"
#include "hll/ccm.h"
#include "distmat/distmat.h"
#include <sstream>

using namespace sketch;
using circ::roundup;
using hll::hll_t;

#ifndef BUFFER_FLUSH_SIZE
#define BUFFER_FLUSH_SIZE (1u << 18)
#endif

namespace bns {
enum EmissionType {
    MASH_DIST = 0,
    JI        = 1,
    SIZES     = 2
};

void main_usage(char **argv) {
    std::fprintf(stderr, "Usage: %s <subcommand> [options...]. Use %s <subcommand> for more options. [Subcommands: sketch, dist, setdist, hll, printbinary.]\n",
                 *argv, *argv);
    std::exit(EXIT_FAILURE);
}

void dist_usage(const char *arg) {
    std::fprintf(stderr, "Usage: %s <opts> [genomes if not provided from a file with -F]\n"
                         "Flags:\n"
                         "-h/-?\tUsage\n"
                         "-k\tSet kmer size [31]\n"
                         "-p\tSet number of threads [1]\n"
                         "-s\tadd a spacer of the format <int>x<int>,<int>x<int>,"
                         "..., where the first integer corresponds to the space "
                         "between bases repeated the second integer number of times\n"
                         "-w\tSet window size [max(size of spaced kmer, [parameter])]\n"
                         "-S\tSet sketch size [16, for 2**16 bytes each]\n"
                         "-c\tCache sketches/use cached sketches\n"
                         "-H\tTreat provided paths as pre-made sketches.\n"
                         "-C\tDo not canonicalize. [Default: canonicalize]\n"
                         "-P\tSet prefix for sketch file locations [empty]\n"
                         "-x\tSet suffix in sketch file names [empty]\n"
                         "-o\tOutput for genome size estimates [stdout]\n"
                         "-I\tUse Ertl's Improved Estimator\n"
                         "-E\tUse Ertl's Original Estimator\n"
                         "-m\tUse Ertl's MLE Estimator with inclusion/exclusion [default\tUses Ertl's Joint MLE Estimator]\n"
                         "-O\tOutput for genome distance matrix [stdout]\n"
                         "-L\tClamp estimates below expected variance to 0. [Default: do not clamp]\n"
                         "-e\tEmit in scientific notation\n"
                         "-f\tReport results as float. (Only important for binary format.) This halves the memory footprint at the cost of precision loss.\n"
                         "-g\tUse entropy minimization (rather than lexical)\n"
                         "-F\tGet paths to genomes from file rather than positional arguments\n"
                         "-M\tEmit Mash distance (default: jaccard index)\n"
                         "-Z\tEmit genome sizes (default: jaccard -index)\n"
                , arg);
    std::exit(EXIT_FAILURE);
}


// Usage, utilities
void sketch_usage(const char *arg) {
    std::fprintf(stderr, "Usage: %s <opts> [genomes if not provided from a file with -F]\n"
                         "Flags:\n"
                         "-h/-?:\tUsage\n"
                         "-k\tSet kmer size [31]\n"
                         "-p\tSet number of threads [1]\n"
                         "-s\tadd a spacer of the format <int>x<int>,<int>x<int>,"
                         "..., where the first integer corresponds to the space "
                         "between bases repeated the second integer number of times\n"
                         "-w\tSet window size [max(size of spaced kmer, [parameter])]\n"
                         "-S\tSet sketch size [16, for 2**16 bytes each]\n"
                         "-F\tGet paths to genomes from file rather than positional arguments\n"
                         "-b:\tBatch size [16 genomes]\n"
                         "-c:\tCache sketches/use cached sketches\n"
                         "-C:\tDo not canonicalize. [Default: canonicalize]\n"
                         "-L:\tClamp estimates below expected variance to 0. [Default: do not clamp]\n"
                         "-P\tSet prefix for sketch file locations [empty]\n"
                         "-x\tSet suffix in sketch file names [empty]\n"
                         "-E\tUse Flajolet with inclusion/exclusion quantitation method for hll. [Default: Ertl Joint MLE]\n"
                         "-I\tUse Ertl improved estimator with inclusion/exclusion quantitation method for hll. This has low error but introduces bias. [Default: Ertl Joint MLE]\n"
                         "-J\tUse Ertl MLE with inclusion/exclusion quantitation method for hll [Default: Ertl Joint MLE, which is *different* and probably better.].\n"
                         "-z\tWrite gzip compressed. (Or zstd-compressed, if compiled with zlibWrapper.\n"
                , arg);
    std::exit(EXIT_FAILURE);
}

bool fname_is_fq(const std::string &path) {
    static const std::string fq1 = ".fastq", fq2 = ".fq";
    return path.find(fq1) != std::string::npos || path.find(fq2) != std::string::npos;
}

std::string hll_fname(const char *path, size_t sketch_p, int wsz, int k, int csz, const std::string &spacing, const std::string &suffix="", const std::string &prefix="") {
    std::string ret(prefix);
    {
        const char *p;
        if(ret.size() && (p = strrchr(get_cstr(path), '/')))
            ret += std::string(p);
        else
            ret += get_cstr(path);
    }
    ret += ".w";
    ret + std::to_string(std::max(csz, wsz));
    ret += ".";
    ret += std::to_string(k);
    ret += ".spacing";
    ret += spacing;
    ret += '.';
    if(suffix.size()) {
        ret += "suf";
        ret += suffix;
        ret += '.';
    }
    ret += std::to_string(sketch_p);
    ret += ".hll";
    return ret;
}

enum sketching_method {
    EXACT = 0,
    CBF   = 1,
    BY_FNAME = 2
};


size_t fsz2countcm(uint64_t fsz, double factor=1.) {
    return roundup(size_t(std::log2(fsz * factor))) + 2; // plus 2 to account for the fact that the file is likely compressed
}

size_t fsz2count(uint64_t fsz) {
    static constexpr size_t mul = 4; // Take these estimates and multiply by 4 just to be safe
    // This should be adapted according to the error rate of the pcbf
    if(fsz < 300ull << 20) return 1 * mul;
    if(fsz < 500ull << 20) return 3 * mul;
    if(fsz < 1ull << 30)  return 10 * mul;
    if(fsz < 3ull << 30)  return 20 * mul;
    return static_cast<size_t>(std::pow(2., std::log((fsz >> 30))) * 30.) * mul;
    // This likely does not account for compression.
}

// Main functions
int sketch_main(int argc, char *argv[]) {
    int wsz(0), k(31), sketch_size(16), skip_cached(false), co, nthreads(1), mincount(-1), nhashes(1), cmsketchsize(-1);
    bool canon(true), write_to_dev_null(false), write_gz(false), clamp(false);
    bool entropy_minimization = false;
    hll::EstimationMethod estim = hll::EstimationMethod::ERTL_MLE;
    hll::JointEstimationMethod jestim = hll::JointEstimationMethod::ERTL_JOINT_MLE;
    std::string spacing, paths_file, suffix, prefix;
    sketching_method sm = EXACT;
    uint64_t seedseedseed = 1337u;
    while((co = getopt(argc, argv, "n:P:F:c:p:x:R:s:S:k:w:H:q:BfjLzEDIcCeh?")) >= 0) {
        switch(co) {
            case 'B': sm = CBF; break;
            case 'C': canon = false; break;
            case 'D': write_to_dev_null = true; break;
            case 'E': jestim = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ORIGINAL); break;
            case 'F': paths_file = optarg; break;
            case 'H': nhashes = std::atoi(optarg); break;
            case 'I': jestim = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ERTL_IMPROVED); break;
            case 'J': jestim = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ERTL_MLE); break;
            case 'L': clamp = true; break;
            case 'P': prefix = optarg; break;
            case 'R': seedseedseed = std::strtoull(optarg, nullptr, 10); break;
            case 'S': sketch_size = std::atoi(optarg); break;
            case 'c': skip_cached = true; break;
            case 'e': entropy_minimization = true; break;
            case 'f': sm = BY_FNAME; break;
            case 'k': k = std::atoi(optarg); break;
            case 'n': mincount = std::atoi(optarg); break;
            case 'p': nthreads = std::atoi(optarg); break;
            case 'q': cmsketchsize = std::atoi(optarg); break;
            case 's': spacing = optarg; break;
            case 'w': wsz = std::atoi(optarg); break;
            case 'x': suffix = optarg; break;
            case 'z': write_gz = true; break;
            case 'h': case '?': sketch_usage(*argv); break;
        }
    }
    omp_set_num_threads(nthreads);
    spvec_t sv(parse_spacing(spacing.data(), k));
    Spacer sp(k, wsz, sv);
    std::vector<std::vector<std::string>> ivecs;
    std::vector<bool> use_filter;
    std::vector<cm::ccm_t> cms;
    std::vector<std::string> inpaths(paths_file.size() ? get_paths(paths_file.data())
                                                       : std::vector<std::string>(argv + optind, argv + argc));
    if(sm != EXACT) {
        if(cmsketchsize < 0) {
            cmsketchsize = fsz2countcm(
                std::accumulate(inpaths.begin(), inpaths.end(), 0u,
                                [](unsigned x, const auto &y) ->unsigned {return std::max(x, (unsigned)bns::filesize(y.data()));})
            );
        }
        if(sm == CBF)
            use_filter = std::vector<bool>(inpaths.size(), true);
        else // BY_FNAME
            for(const auto &path: inpaths) use_filter.emplace_back(fname_is_fq(path));
        auto nbits = std::log2(mincount) + 1;
        while(cms.size() < unsigned(nthreads))
            cms.emplace_back(nbits, cmsketchsize, nhashes, cms.size() * 1337u + seedseedseed);
    }
    KSeqBufferHolder kseqs(nthreads);
    if(wsz < sp.c_) wsz = sp.c_;
    if(inpaths.empty()) {
        std::fprintf(stderr, "No paths. See usage.\n");
        sketch_usage(*argv);
    }
    std::vector<hll_t> hlls;
    while(hlls.size() < (u32)nthreads) hlls.emplace_back(sketch_size, estim, jestim, 1, clamp);
    nthreads = std::max(nthreads, 1);
    omp_set_num_threads(nthreads);
    std::vector<std::string> fnames(nthreads);
#define MAIN_SKETCH_LOOP(MinType)\
    for(size_t i = 0; i < ivecs.size(); ++i) {\
        const int tid = omp_get_thread_num();\
        std::string &fname = fnames[tid];\
        std::vector<std::string> &scratch_stringvec = ivecs[i];\
        fname = hll_fname(scratch_stringvec[0].data(), sketch_size, wsz, k, sp.c_, spacing, suffix, prefix);\
        if(write_gz) fname += ".gz";\
        if(skip_cached && isfile(fname)) continue;\
        Encoder<MinType> enc(nullptr, 0, sp, nullptr, canon);\
        hll_t &h = hlls[tid];\
        if(use_filter.size() && use_filter[i]) {\
            auto &cm = cms[tid];\
            enc.for_each([&](u64 kmer){if(cm.addh(kmer) >= mincount) h.addh(kmer);}, inpaths[i].data(), &kseqs[tid]);\
        } else {\
            enc.for_each([&](u64 kmer){h.addh(kmer);}, inpaths[i].data(), &kseqs[tid]);\
        }\
        h.write(write_to_dev_null ? "/dev/null": static_cast<const char *>(fname.data()), write_gz);\
        h.clear();\
    }
    if(entropy_minimization) {
        #pragma omp parallel for schedule(dynamic, 16)
        MAIN_SKETCH_LOOP(bns::score::Entropy)
    } else {
        #pragma omp parallel for schedule(dynamic, 16)
        MAIN_SKETCH_LOOP(bns::score::Lex)
    }
#undef MAIN_SKETCH_LOOP
    LOG_INFO("Successfully finished sketching from %zu files\n", ivecs.size());
    return EXIT_SUCCESS;
}

template<typename FType, typename=typename std::enable_if<std::is_floating_point<FType>::value>::type>
size_t submit_emit_dists(const int pairfi, const FType *ptr, u64 hs, size_t index, ks::string &str, const std::vector<std::string> &inpaths, bool write_binary, bool use_scientific, const size_t buffer_flush_size=1ull<<18) {
    if(write_binary) {
       ::write(pairfi, ptr, sizeof(FType) * hs);
    } else {
        const char *const fmt(use_scientific ? "%e\t": "%f\t");
#if !NDEBUG
        std::fprintf(stderr, "format is '%s'. use_scientific = %s\n", fmt, use_scientific ? "true": "false");
#endif
        str += inpaths[index];
        str.putc_('\t');
        {
            u64 k;
            for(k = 0; k < index + 1;  ++k, str.putsn_("-\t", 2));
            for(k = 0; k < hs - index - 1; str.sprintf(fmt, ptr[k++]));
        }
        str.back() = '\n';
        if(str.size() >= 1 << 18) str.flush(pairfi);
    }
    return index;
}

template<typename FType1, typename FType2,
         typename=typename std::enable_if<
            std::is_floating_point<FType1>::value && std::is_floating_point<FType2>::value
          >::type
         >
typename std::common_type<FType1, FType2>::type dist_index(FType1 ji, FType2 ksinv) {
    // Adapter from Mash https://github.com/Marbl/Mash
    return ji ? -std::log(2. * ji / (1. + ji)) * ksinv: 1.;
}

template<typename FType, typename=typename std::enable_if<std::is_floating_point<FType>::value>::type>
void dist_loop(const int pairfi, std::vector<hll_t> &hlls, const std::vector<std::string> &inpaths, const bool use_scientific, const unsigned k, const EmissionType emit_fmt, bool write_binary, const size_t buffer_flush_size=1ull<<18) {
#if !NDEBUG
    std::fprintf(stderr, "value for use_scientific is %s\n", use_scientific ? "t": "f");
#endif
    std::array<std::vector<FType>, 2> dps;
    dps[0].resize(hlls.size() - 1);
    dps[1].resize(hlls.size() - 2);
    ks::string str;
    const FType ksinv = 1./ k;
    std::future<size_t> submitter;
    for(size_t i = 0; i < hlls.size(); ++i) {
        hll_t &h1(hlls[i]); // TODO: consider working backwards and pop_back'ing.
        std::vector<FType> &dists = dps[i & 1];
#if AVOID_COMPUTED_GOTO
        switch(emit_fmt) {
            case MASH_DIST: {
                #pragma omp parallel for schedule(dynamic)
                for(size_t j = i + 1; j < hlls.size(); ++j)
                    dists[j  i  1] = dist_index(jaccard_index(hlls[j], h1), ksinv);
                break;
            }
            case JI: {
                #pragma omp parallel for schedule(dynamic)
                for(size_t j = i + 1; j < hlls.size(); ++j)
                    dists[j  i  1] = jaccard_index(hlls[j], h1);
                break;
            }
            case SIZES: {
                #pragma omp parallel for schedule(dynamic)
                for(size_t j = i + 1; j < hlls.size(); ++j)
                    dists[j  i  1] = union_size(hlls[j], h1);
                break;
            }
            default:
                __builtin_unreachable();
		}
#else
#pragma GCC diagnostic ignored "-Wpedantic"
        static constexpr void *labels[] {&&MASH_DIST, &&JI, &&SIZES};
        goto *labels[emit_fmt];
        MASH_DIST: {
            #pragma omp parallel for schedule(dynamic)
            for(size_t j = i + 1; j < hlls.size(); ++j)
                dists[j - i - 1] = dist_index(jaccard_index(hlls[j], h1), ksinv);
            goto fr;
        }
        JI: {
            #pragma omp parallel for schedule(dynamic)
            for(size_t j = i + 1; j < hlls.size(); ++j)
                dists[j - i - 1] = jaccard_index(hlls[j], h1);
            goto fr;
        }
        SIZES: {
            #pragma omp parallel for schedule(dynamic)
            for(size_t j = i + 1; j < hlls.size(); ++j)
                dists[j - i - 1] = union_size(hlls[j], h1);
            goto fr;
        }
        fr:
        h1.free();
#pragma GCC diagnostic pop
#endif
        LOG_DEBUG("Finished chunk %zu of %zu\n", i + 1, hlls.size());
        if(i)
#if !NDEBUG
            LOG_DEBUG("Finished writing row %zu\n", submitter.get());
#else
            submitter.get();
#endif
        submitter = std::async(std::launch::async, submit_emit_dists<FType>, pairfi, dists.data(), hlls.size(), i, std::ref(str), std::ref(inpaths), write_binary, use_scientific, buffer_flush_size);
    }
    submitter.get();
    if(!write_binary) str.flush(pairfi);
}

namespace {
enum CompReading: unsigned {
    UNCOMPRESSED,
    GZ,
    AUTODETECT
};
}
#define FILL_SKETCH_MIN(MinType) \
    do {\
        Encoder<MinType> enc(nullptr, 0, sp, nullptr, canon);\
        if(cms.empty()) {\
            hll_t &h = hlls[i];\
            enc.for_each([&](u64 kmer){h.addh(kmer);}, inpaths[i].data(), &kseqs[tid]);\
        } else {\
            sketch::cm::ccm_t &cm = cms[tid];\
            enc.for_each([&,mincount](u64 kmer){if(cm.addh(kmer) >= mincount) hlls[i].addh(kmer);}, inpaths[i].data(), &kseqs[tid]);\
            cm.clear();\
        } \
    } while(0)

int dist_main(int argc, char *argv[]) {
    int wsz(0), k(31), sketch_size(16), use_scientific(false), co, cache_sketch(false),
        nthreads(1), mincount(30), nhashes(4);
    bool canon(true), presketched_only(false), write_binary(false),
         emit_float(false),
         clamp(false), sketch_query_by_seq(true), entropy_minimization(false);
    double factor = 1.;
    EmissionType emit_fmt(JI);
    hll::EstimationMethod estim = hll::EstimationMethod::ERTL_MLE;
    hll::JointEstimationMethod jestim = hll::JointEstimationMethod::ERTL_JOINT_MLE;
    std::string spacing, paths_file, suffix, prefix, pairofp_labels;
    CompReading reading_type = CompReading::UNCOMPRESSED;
    FILE *ofp(stdout), *pairofp(stdout);
    sketching_method sm = EXACT;
    omp_set_num_threads(1);
    std::vector<std::string> querypaths;
    while((co = getopt(argc, argv, "Q:P:x:F:c:p:o:s:w:O:S:k:=:T:gDazLfICbMEeHhZBNymq?")) >= 0) {
        switch(co) {
            case 'C': canon = false; break;
            case 'D': sketch_query_by_seq = false; break;
            case 'E': jestim = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ORIGINAL); break;
            case 'F': paths_file = optarg; break;
            case 'H': presketched_only = true; break;
            case 'I': jestim = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ERTL_IMPROVED); break;
            case 'L': clamp = true; break;
            case 'M': emit_fmt = MASH_DIST; break;
            case 'N': sm = BY_FNAME; break;
            case 'O': if((pairofp = fopen(optarg, "wb")) == nullptr)
                          LOG_EXIT("Could not open file at %s for writing.\n", optarg);
                      pairofp_labels = std::string(optarg) + ".labels";
                      break;
            case 'P': prefix = optarg;                 break;
            case 'Q': querypaths.emplace_back(optarg); break;
            case 'S': sketch_size = std::atoi(optarg); break;
            case 'W': cache_sketch = true;             break;
            case 'Z': emit_fmt = SIZES;                break;
            case 'a': reading_type = AUTODETECT;       break;
            case 'b': write_binary = true;             break;
            case 'c': mincount = std::atoi(optarg);    break;
            case 'e': use_scientific = true;           break;
            case 'f': emit_float = true;               break;
            case 'g': entropy_minimization = true;     break;
            case 'k': k = std::atoi(optarg);           break;
            case 'm': jestim = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ERTL_MLE); break;
            case 'o': if((ofp = fopen(optarg, "w")) == nullptr) LOG_EXIT("Could not open file at %s for writing.\n", optarg); break;
            case 'p': nthreads = std::atoi(optarg);    break;
            case 'q': nhashes = std::atoi(optarg);     break;
            case 's': spacing = optarg; break;
            case 'w': wsz = std::atoi(optarg); break;
            case 'x': suffix = optarg; break;
            case 'y': sm = CBF; break;
            case 'z': reading_type = GZ; break;
            case 'h': case '?': dist_usage(*argv);
        }
    }
    spvec_t sv(parse_spacing(spacing.data(), k));
    Spacer sp(k, wsz, sv);
    std::vector<std::string> inpaths(paths_file.size() ? get_paths(paths_file.data())
                                                       : std::vector<std::string>(argv + optind, argv + argc));
    if(inpaths.size() == 0) {
        std::fprintf(stderr, "No paths. See usage.\n");
        dist_usage(*argv);
    }
    omp_set_num_threads(nthreads);
    std::vector<sketch::cm::ccm_t> cms;
    std::vector<hll_t> hlls;
    KSeqBufferHolder kseqs(nthreads);
    if(sm == CBF || sm == BY_FNAME) {
        const auto cmsketchsize = fsz2countcm(
            std::accumulate(inpaths.begin(), inpaths.end(), 0u,
                            [](unsigned x, const auto &y) ->unsigned {return std::max(x, (unsigned)bns::filesize(y.data()));}),
            factor
        );
        unsigned nbits = std::log2(mincount) + 1;
        while(cms.size() < static_cast<unsigned>(nthreads))
            cms.emplace_back(nbits, cmsketchsize, nhashes, cms.size() * 1337u);
    }
    hlls.reserve(inpaths.size());
    while(hlls.size() < inpaths.size()) hlls.emplace_back(hll_t(sketch_size, estim, jestim, 1, clamp));
    if(wsz < sp.c_) wsz = sp.c_;
    #pragma omp parallel for
    for(size_t i = 0; i < hlls.size(); ++i) {
        const std::string &path(inpaths[i]);
        static const std::string suf = ".gz";
        if(presketched_only) hlls[i].read(path);
        else {
            const std::string fpath(hll_fname(path.data(), sketch_size, wsz, k, sp.c_, spacing, suffix, prefix));
            const bool isf = isfile(fpath);
            if(cache_sketch && isf) {
                LOG_DEBUG("Sketch found at %s with size %zu, %u\n", fpath.data(), size_t(1ull << sketch_size), sketch_size);
                hlls[i].read(fpath);
            } else {
                const int tid = omp_get_thread_num();
                if(entropy_minimization) {
                    FILL_SKETCH_MIN(score::Entropy);
                } else {
                    FILL_SKETCH_MIN(score::Lex);
                }
#undef FILL_SKETCH_MIN
                if(cache_sketch && !isf) hlls[i].write(fpath, (reading_type == GZ ? 1: reading_type == AUTODETECT ? std::equal(suf.rbegin(), suf.rend(), fpath.rbegin()): false));
            }
        }
    }
    kseqs.free();
    ks::string str("#Path\tSize (est.)\n");
    assert(str == "#Path\tSize (est.)\n");
    str.resize(1 << 18);
    {
        const int fn(fileno(ofp));
        for(size_t i(0); i < hlls.size(); ++i) {
            str.sprintf("%s\t%lf\n", inpaths[i].data(), hlls[i].report());
            if(str.size() >= 1 << 18) str.flush(fn);
        }
        str.flush(fn);
    }
    if(ofp != stdout) std::fclose(ofp);
    str.clear();
    // TODO: make 'screen' emit binary if -'b' is provided.
    // TODO: update 'screen'-like function to support entropy minimization.
    if(querypaths.size()) { // Screen query against input paths rather than perform all-pairs
        std::fprintf(pairofp, "#queryfile||seqname");
        for(const auto &ip: inpaths)
            std::fprintf(pairofp, "\t%s", ip.data());
        std::fputc('\n', pairofp);
        std::fflush(pairofp);
        hll_t hll(sketch_size, estim, jestim, 1, clamp);
        kseq_t ks = kseq_init_stack();
        std::vector<double> dists(inpaths.size());
        ks::string output;
        const int fn = fileno(pairofp);
        output.resize(1 << 16);
        Encoder<score::Lex> enc(sp);
        const char *fmt = use_scientific ? "\t%e": "\t%lf";
        for(const auto &path: querypaths) {
            gzFile fp;
            if((fp = gzopen(path.data(), "rb")) == nullptr) throw std::runtime_error(std::string("Could not open file at " + path));
            kseq_assign(&ks, fp);
            if(sketch_query_by_seq) {
                while(kseq_read(&ks) >= 0) {
                    output.sprintf("%s||%s", path.data(), ks.name.s ? ks.name.s: const_cast<char *>("seq_without_name"));
                    enc.for_each([&](u64 kmer) {hll.addh(kmer);}, ks.seq.s, ks.seq.l);
                    #pragma omp parallel for
                    for(size_t i = 0; i < hlls.size(); ++i)
                        dists[i] = hll.jaccard_index(hlls[i]);
                    for(size_t i(0); i < hlls.size(); output.sprintf(fmt, dists[i++]));
                    output.putc_('\n');
                    if(output.size() >= (1 << 16)) output.flush(fn);
                    hll.clear();
                }
            } else {
                output.sprintf("%s", path.data());
                while(kseq_read(&ks) >= 0)
                    enc.for_each([&](u64 kmer) {hll.addh(kmer);}, ks.seq.s, ks.seq.l);
                #pragma omp parallel for
                for(size_t i = 0; i < hlls.size(); ++i)
                    dists[i] = hll.jaccard_index(hlls[i]);
                for(size_t i(0); i < hlls.size(); output.sprintf(fmt, dists[i++]));
                output.putc_('\n');
                if(output.size() >= (1 << 16)) output.flush(fn);
                hll.clear();
            }
        }
        output.flush(fn);
        kseq_destroy_stack(ks);
    } else {
        if(write_binary) {
            const char *name = emit_float ? dm::MAGIC_NUMBER<float>().name(): dm::MAGIC_NUMBER<double>().name();
            std::fwrite(name, std::strlen(name + 1), 1, pairofp);
            const size_t hs(hlls.size());
            std::fwrite(&hs, sizeof(hs), 1, pairofp);
        } else {
            str.sprintf("##Names \t");
            for(const auto &path: inpaths) str.sprintf("%s\t", path.data());
            str.back() = '\n';
            str.write(fileno(pairofp)); str.free();
        }
        auto fn_ptr = emit_float ? dist_loop<float> :dist_loop<double>;
        static constexpr uint32_t buffer_flush_size = BUFFER_FLUSH_SIZE;
        fn_ptr(fileno(pairofp), hlls, inpaths, use_scientific, k, emit_fmt, write_binary, buffer_flush_size);
    }
    if(write_binary) {
        if(pairofp_labels.empty()) pairofp_labels = "unspecified";
        std::FILE *fp = std::fopen(pairofp_labels.data(), "wb");
        if(fp == nullptr) RUNTIME_ERROR(std::string("Could not open file at ") + pairofp_labels);
        for(const auto &path: inpaths) std::fwrite(path.data(), path.size(), 1, fp), std::fputc('\n', fp);
        std::fclose(fp);
    }
    if(pairofp != stdout) std::fclose(pairofp);
    return EXIT_SUCCESS;
}

int print_binary_main(int argc, char *argv[]) {
    int c;
    bool use_float = false, use_scientific = false;
    std::string outpath;
    for(char **p(argv); *p; ++p) if(std::strcmp(*p, "-h") && std::strcmp(*p, "--help") == 0) goto usage;
    if(argc == 1) {
        usage:
        std::fprintf(stderr, "%s <path to binary file> [- to read from stdin]\n", argv ? static_cast<const char *>(*argv): "flashdans");
    }
    while((c = getopt(argc, argv, ":o:sfh?")) >= 0) {
        switch(c) {
            case 'o': outpath = optarg; break;
            case 'f': use_float = true; break;
            case 's': use_scientific = true; break;
        }
    }
    std::FILE *fp;
    if(outpath.empty()) outpath = "/dev/stdout";
    if(use_float) {
        dm::DistanceMatrix<float> mat(argv[optind]);
        if((fp = std::fopen(outpath.data(), "wb")) == nullptr) RUNTIME_ERROR(ks::sprintf("Could not open file at %s", outpath.data()).data());
        mat.printf(fp, use_scientific);
    } else {
        dm::DistanceMatrix<double> mat(argv[optind]);
        if((fp = std::fopen(outpath.data(), "wb")) == nullptr) RUNTIME_ERROR(ks::sprintf("Could not open file at %s", outpath.data()).data());
        mat.printf(fp, use_scientific);
    }
    std::fclose(fp);
    return EXIT_SUCCESS;
}

int setdist_main(int argc, char *argv[]) {
    int wsz(0), k(31), use_scientific(false), co;
    bool canon(true), emit_jaccard(true);
    unsigned bufsize(1 << 18);
    int nt(1);
    std::string spacing, paths_file;
    FILE *ofp(stdout), *pairofp(stdout);
    omp_set_num_threads(1);
    while((co = getopt(argc, argv, "F:c:p:o:O:S:B:k:CMeh?")) >= 0) {
        switch(co) {
            case 'B': std::stringstream(optarg) << bufsize; break;
            case 'k': k = std::atoi(optarg); break;
            case 'p': omp_set_num_threads((nt = std::atoi(optarg))); break;
            case 's': spacing = optarg; break;
            case 'C': canon = false; break;
            case 'w': wsz = std::atoi(optarg); break;
            case 'F': paths_file = optarg; break;
            case 'o': ofp = fopen(optarg, "w"); break;
            case 'O': pairofp = fopen(optarg, "w"); break;
            case 'e': use_scientific = true; break;
            case 'J': emit_jaccard = false; break;
            case 'h': case '?': dist_usage(*argv);
        }
    }
    std::vector<char> rdbuf(bufsize);
    spvec_t sv(parse_spacing(spacing.data(), k));
    Spacer sp(k, wsz, sv);
    std::vector<std::string> inpaths(paths_file.size() ? get_paths(paths_file.data())
                                                       : std::vector<std::string>(argv + optind, argv + argc));
    KSeqBufferHolder h(nt);
    std::vector<khash_t(all)> hashes;
    while(hashes.size() < inpaths.size()) hashes.emplace_back(khash_t(all){0, 0, 0, 0, 0, 0, 0});
    for(auto hash: hashes) kh_resize(all, &hash, 1 << 20); // Try to reduce the number of allocations.
    const size_t nhashes(hashes.size());
    if(wsz < sp.c_) wsz = sp.c_;
    if(inpaths.size() == 0) {
        std::fprintf(stderr, "No paths. See usage.\n");
        dist_usage(*argv);
    }
    #pragma omp parallel for
    for(size_t i = 0; i < nhashes; ++i) {
        fill_set_genome<score::Lex>(inpaths[i].data(), sp, &hashes[i], i, nullptr, canon, h.data() + omp_get_thread_num());
    }
    LOG_DEBUG("Filled genomes. Now analyzing data.\n");
    ks::string str;
    str.sprintf("#Path\tSize (est.)\n");
    {
        const int fn(fileno(ofp));
        for(size_t i(0); i < nhashes; ++i) {
            str.sprintf("%s\t%zu\n", inpaths[i].data(), kh_size(&hashes[i]));
            if(str.size() > 1 << 17) str.flush(fn);
        }
        str.flush(fn);
    }
    // TODO: Emit overlaps and symmetric differences.
    if(ofp != stdout) std::fclose(ofp);
    std::vector<double> dists(nhashes - 1);
    str.clear();
    str.sprintf("##Names \t");
    for(auto &path: inpaths) str.sprintf("%s\t", path.data());
    str.back() = '\n';
    str.write(fileno(pairofp)); str.free();
    setvbuf(pairofp, rdbuf.data(), _IOLBF, rdbuf.size());
    const char *const fmt(use_scientific ? "\t%e": "\t%f");
    const double ksinv = 1./static_cast<double>(k);
    for(size_t i = 0; i < nhashes; ++i) {
        const khash_t(all) *h1(&hashes[i]);
        size_t j;
#define DO_LOOP(val) for(j = i + 1; j < nhashes; ++j) dists[j - i - 1] = (val)
        if(emit_jaccard) {
            #pragma omp parallel for
            DO_LOOP(jaccard_index(&hashes[j], h1));
        } else {
            #pragma omp parallel for
            DO_LOOP(dist_index(jaccard_index(&hashes[j], h1), ksinv));
        }
#undef DO_LOOP

        for(j = 0; j < i + 1; fputc('\t', pairofp), fputc('-', pairofp), ++j);
        for(j = 0; j < nhashes - i - 1; fprintf(pairofp, fmt, dists[j++]));
        fputc('\n', pairofp);
        std::free(h1->keys); std::free(h1->vals); std::free(h1->flags);
    }
    return EXIT_SUCCESS;
}

int hll_main(int argc, char *argv[]) {
    int c, wsz(0), k(31), num_threads(-1), sketch_size(24);
    bool canon(true);
    std::string spacing, paths_file;
    if(argc < 2) {
        usage: LOG_EXIT("Usage: %s <opts> <paths>\nFlags:\n"
                        "-k:\tkmer length (Default: 31. Max: 31)\n"
                        "-w:\twindow size (Default: -1)  Must be -1 (ignored) or >= kmer length.\n"
                        "-s:\tspacing (default: none). format: <value>x<times>,<value>x<times>,...\n"
                        "   \tOmitting x<times> indicates 1 occurrence of spacing <value>\n"
                        "-S:\tsketch size (default: 24). (Allocates 2 << [param] bytes of memory per HyperLogLog.\n"
                        "-p:\tnumber of threads.\n"
                        "-F:\tPath to file which contains one path per line\n"
                        , argv[0]);
    }
    while((c = getopt(argc, argv, "Cw:s:S:p:k:tfh?")) >= 0) {
        switch(c) {
            case 'C': canon = false; break;
            case 'h': case '?': goto usage;
            case 'k': k = std::atoi(optarg); break;
            case 'p': num_threads = std::atoi(optarg); break;
            case 's': spacing = optarg; break;
            case 'S': sketch_size = std::atoi(optarg); break;
            case 'w': wsz = std::atoi(optarg); break;
            case 'F': paths_file = optarg; break;
        }
    }
    if(wsz < k) wsz = k;
    std::vector<std::string> inpaths(paths_file.size() ? get_paths(paths_file.data())
                                                       : std::vector<std::string>(argv + optind, argv + argc));
    spvec_t sv(parse_spacing(spacing.data(), k));
    LOG_INFO("Processing %zu paths with %i threads\n", inpaths.size(), num_threads);
    const double est(estimate_cardinality<bns::score::Lex>(inpaths, k, wsz, sv, canon, nullptr, num_threads, sketch_size));
    std::fprintf(stdout, "Estimated number of unique exact matches: %lf\n", est);
    return EXIT_SUCCESS;
}

} // namespace bns

using namespace bns;

int main(int argc, char *argv[]) {
    std::ios_base::sync_with_stdio(false);
    if(argc == 1) main_usage(argv);
    if(std::strcmp(argv[1], "sketch") == 0) return sketch_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "dist") == 0) return dist_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "setdist") == 0) return setdist_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "hll") == 0) return hll_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "printbinary") == 0) return print_binary_main(argc - 1, argv + 1);
    else {
        for(const char *const *p(argv + 1); *p; ++p)
            if(std::string(*p) == "-h" || std::string(*p) == "--help") main_usage(argv);
        RUNTIME_ERROR(std::string("Invalid subcommand ") + argv[1] + " provided.");
    }
}
