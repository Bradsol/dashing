#include <omp.h>
#include "bonsai/bonsai/include/util.h"
#include "bonsai/bonsai/include/database.h"
#include "bonsai/bonsai/include/bitmap.h"
#include "bonsai/bonsai/include/setcmp.h"
#include "hll/bbmh.h"
#include "hll/mh.h"
#include "hll/mult.h"
#include "khset/khset.h"
#include "distmat/distmat.h"
#include <sstream>
#include "getopt.h"
#include <sys/stat.h>
#include "substrs.h"

#if __cplusplus >= 201703L && __cpp_lib_execution
#include <execution>
#endif
#ifdef FNAME_SEP
#pragma message("Not: FNAME_SEP already defined. [not default \"' '\"]")
#else
#define FNAME_SEP ' '
#endif

using namespace sketch;
using circ::roundup;
using hll::hll_t;
using sketch::common::NotImplementedError;

#ifndef BUFFER_FLUSH_SIZE
#define BUFFER_FLUSH_SIZE (1u << 18)
#endif

using option_struct = struct option;
namespace bns {
using sketch::common::WangHash;
static const char *executable = nullptr;

struct GlobalArgs {
    size_t weighted_jaccard_cmsize = 22;
    size_t weighted_jaccard_nhashes = 8;
    uint32_t bbnbits = 16;
};
static GlobalArgs gargs;

enum EmissionType {
    MASH_DIST = 0,
    JI        = 1,
    SIZES     = 2,
    FULL_MASH_DIST = 3,
    FULL_CONTAINMENT_DIST = 4,
    CONTAINMENT_INDEX = 5,
    CONTAINMENT_DIST = 6,
    SYMMETRIC_CONTAINMENT_INDEX = 7,
    SYMMETRIC_CONTAINMENT_DIST = 8,
};

static const char *emt2str(EmissionType result_type) {
    switch(result_type) {
        case MASH_DIST: return "MASH_DIST";
        case JI: return "JI";
        case SIZES: return "SIZES";
        case FULL_MASH_DIST: return "FULL_MASH_DIST";
        case FULL_CONTAINMENT_DIST: return "FULL_CONTAINMENT_DIST";
        case CONTAINMENT_INDEX: return "CONTAINMENT_INDEX";
        case CONTAINMENT_DIST: return "CONTAINMENT_DIST";
        case SYMMETRIC_CONTAINMENT_INDEX: return "SYMMETRIC_CONTAINMENT_INDEX";
        case SYMMETRIC_CONTAINMENT_DIST: return "SYMMETRIC_CONTAINMENT_DIST";
        default: break;
    }
    return "ILLEGAL_EMISSION_FMT";
}

static constexpr bool is_symmetric(EmissionType result_type) {
    switch(result_type) {
        case MASH_DIST: case JI: case SIZES:
        case FULL_MASH_DIST: case SYMMETRIC_CONTAINMENT_INDEX: case SYMMETRIC_CONTAINMENT_DIST:
            return true;

        case CONTAINMENT_INDEX: case CONTAINMENT_DIST: case FULL_CONTAINMENT_DIST:
        default: break;
    }
    return false;
}

enum EncodingType {
    BONSAI,
    NTHASH,
    RK,
    CYCLIC
};

struct khset64_t: public kh::khset64_t {
    // TODO: change to sorted hash sets for faster comparisons, implement parallel merge sort.
    using final_type = khset64_t;
    void addh(uint64_t v) {this->insert(v);}
    double cardinality_estimate() const {
        return this->size();
    }
    khset64_t(): kh::khset64_t() {}
    khset64_t(size_t reservesz): kh::khset64_t(reservesz) {}
    khset64_t(const std::string &s): khset64_t(s.data()) {}
    khset64_t(const char *s) {
        this->read(s);
    }
    void read(const std::string &s) {read(s.data());}
    void read(const char *s) {
        gzFile fp = gzopen(s, "rb");
        kh::khset64_t::read(fp);
        gzclose(fp);
    }
    void free() {
        auto ptr = reinterpret_cast<kh::khash_t(set64) *>(this);
        std::free(ptr->keys);
        std::free(ptr->vals);
        std::free(ptr->flags);
        std::memset(ptr, 0, sizeof(*this));
    }
    void write(const std::string &s) const {write(s.data());}
    void write(gzFile fp) const {kh::khset64_t::write(fp);}
    void write(const char *s) const {
        gzFile fp = gzopen(s, "wb");
        kh::khset64_t::write(fp);
        gzclose(fp);
    }
    double jaccard_index(const khset64_t &other) const {
        auto p1 = this, p2 = &other;
        if(size() > other.size())
            std::swap(p1, p2);
        size_t olap = 0;
        p1->for_each([&](auto v) {olap += p2->contains(v);});
        return static_cast<double>(olap) / (p1->size() + p2->size() - olap);
    }
    double containment_index(const khset64_t &other) const {
        auto p1 = this, p2 = &other;
        if(size() > other.size())
            std::swap(p1, p2);
        uint64_t olap = 0;
        p1->for_each([&](auto v) {olap += p2->contains(v);});
        return static_cast<double>(olap) / (this->size());
    }
    khset64_t &operator+=(const khset64_t &o) {
        o.for_each([this](auto x) {this->insert(x);});
        return *this;
    }
    uint64_t union_size(const khset64_t &other) const {
        auto p1 = this, p2 = &other;
        if(size() > other.size())
            std::swap(p1, p2);
        uint64_t olap = 0;
        p1->for_each([&](auto v) {olap += p2->contains(v);});
        return p1->size() + p2->size() - olap;
    }
};
enum EmissionFormat: unsigned {
    UT_TSV = 0,
    BINARY   = 1,
    UPPER_TRIANGULAR = 2,
    PHYLIP_UPPER_TRIANGULAR = 2,
    FULL_TSV = 3,
    JSON = 4
};

enum Sketch: int {
    HLL,
    BLOOM_FILTER,
    RANGE_MINHASH,
    FULL_KHASH_SET,
    COUNTING_RANGE_MINHASH,
    BB_MINHASH,
    BB_SUPERMINHASH,
    COUNTING_BB_MINHASH, // TODO make this work.
};
static const char *sketch_names [] {
    "HLL/HyperLogLog",
    "BF/BloomFilter",
    "RMH/Range Min-Hash/KMV",
    "FHS/Full Hash Set",
    "CRHM/Counting Range Minhash",
    "BB/B-bit Minhash",
    "BBS/B-bit SuperMinHash",
    "CBB/Counting B-bit Minhash",
};
using CBBMinHashType = mh::CountingBBitMinHasher<uint64_t, uint16_t>; // Is counting to 65536 enough for a transcriptome?
template<typename T> struct SketchEnum;
using SuperMinHashType = mh::SuperMinHash<>;
template<> struct SketchEnum<hll::hll_t> {static constexpr Sketch value = HLL;};
template<> struct SketchEnum<bf::bf_t> {static constexpr Sketch value = BLOOM_FILTER;};
template<> struct SketchEnum<mh::RangeMinHash<uint64_t>> {static constexpr Sketch value = RANGE_MINHASH;};
template<> struct SketchEnum<mh::CountingRangeMinHash<uint64_t>> {static constexpr Sketch value = COUNTING_RANGE_MINHASH;};
template<> struct SketchEnum<mh::BBitMinHasher<uint64_t>> {static constexpr Sketch value = BB_MINHASH;};
template<> struct SketchEnum<CBBMinHashType> {static constexpr Sketch value = COUNTING_BB_MINHASH;};
template<> struct SketchEnum<khset64_t> {static constexpr Sketch value = FULL_KHASH_SET;};
template<> struct SketchEnum<SuperMinHashType> {static constexpr Sketch value = BB_SUPERMINHASH;};
template<> struct SketchEnum<wj::WeightedSketcher<hll::hll_t>> {static constexpr Sketch value = HLL;};
template<> struct SketchEnum<wj::WeightedSketcher<bf::bf_t>> {static constexpr Sketch value = BLOOM_FILTER;};
template<> struct SketchEnum<wj::WeightedSketcher<mh::RangeMinHash<uint64_t>>> {static constexpr Sketch value = RANGE_MINHASH;};
template<> struct SketchEnum<wj::WeightedSketcher<mh::CountingRangeMinHash<uint64_t>>> {static constexpr Sketch value = COUNTING_RANGE_MINHASH;};
template<> struct SketchEnum<wj::WeightedSketcher<mh::BBitMinHasher<uint64_t>>> {static constexpr Sketch value = BB_MINHASH;};
template<> struct SketchEnum<wj::WeightedSketcher<CBBMinHashType>> {static constexpr Sketch value = COUNTING_BB_MINHASH;};
template<> struct SketchEnum<wj::WeightedSketcher<khset64_t>> {static constexpr Sketch value = FULL_KHASH_SET;};
template<> struct SketchEnum<wj::WeightedSketcher<SuperMinHashType>> {static constexpr Sketch value = BB_SUPERMINHASH;};


template<typename T>
double cardinality_estimate(T &x) {
    return x.cardinality_estimate();
}

template<> double cardinality_estimate(hll::hll_t &x) {return x.report();}
template<> double cardinality_estimate(mh::FinalBBitMinHash &x) {return x.est_cardinality_;}
template<> double cardinality_estimate(mh::FinalDivBBitMinHash &x) {return x.est_cardinality_;}

static size_t bytesl2_to_arg(int nblog2, Sketch sketch) {
    switch(sketch) {
        case HLL: return nblog2;
        case BLOOM_FILTER: return nblog2 + 3; // 8 bits per byte
        case RANGE_MINHASH: return size_t(1) << (nblog2 - 3); // 8 bytes per minimizer
        case COUNTING_RANGE_MINHASH: return (size_t(1) << (nblog2)) / (sizeof(uint64_t) + sizeof(uint32_t));
        case BB_MINHASH:
            return nblog2 - std::floor(std::log2(gargs.bbnbits / 8));
        case BB_SUPERMINHASH:
            return size_t(1) << (nblog2 - int(std::log2(gargs.bbnbits / 8)));
        case FULL_KHASH_SET: return 16; // Reserve hash set size a bit. Mostly meaningless, resizing as necessary.
        default: {
            char buf[128];
            std::sprintf(buf, "Sketch %s not yet supported.\n", (size_t(sketch) >= (sizeof(sketch_names) / sizeof(char *)) ? "Not such sketch": sketch_names[sketch]));
            RUNTIME_ERROR(buf);
            return -1337;
        }
    }

}


template<typename SketchType>
struct FinalSketch {
    using final_type = SketchType;
};
#define FINAL_OVERLOAD(type) \
template<> struct FinalSketch<type> { \
    using final_type = typename type::final_type;}
FINAL_OVERLOAD(mh::CountingRangeMinHash<uint64_t>);
FINAL_OVERLOAD(mh::RangeMinHash<uint64_t>);
FINAL_OVERLOAD(mh::BBitMinHasher<uint64_t>);
FINAL_OVERLOAD(SuperMinHashType);
FINAL_OVERLOAD(CBBMinHashType);
FINAL_OVERLOAD(wj::WeightedSketcher<mh::CountingRangeMinHash<uint64_t>>);
FINAL_OVERLOAD(wj::WeightedSketcher<mh::RangeMinHash<uint64_t>>);
FINAL_OVERLOAD(wj::WeightedSketcher<mh::BBitMinHasher<uint64_t>>);
FINAL_OVERLOAD(wj::WeightedSketcher<SuperMinHashType>);
FINAL_OVERLOAD(wj::WeightedSketcher<CBBMinHashType>);
template<typename T>struct SketchFileSuffix {static constexpr const char *suffix = ".sketch";};
#define SSS(type, suf) template<> struct SketchFileSuffix<type> {static constexpr const char *suffix = suf;}
SSS(mh::CountingRangeMinHash<uint64_t>, ".crmh");
SSS(mh::RangeMinHash<uint64_t>, ".rmh");
SSS(khset64_t, ".khs");
SSS(bf::bf_t, ".bf");
SSS(mh::BBitMinHasher<uint64_t>, ".bmh");
SSS(SuperMinHashType, ".bbs");
SSS(CBBMinHashType, ".cbmh");
SSS(mh::HyperMinHash<uint64_t>, ".hmh");
SSS(hll::hll_t, ".hll");

using CRMFinal = mh::FinalCRMinHash<uint64_t, std::greater<uint64_t>, uint32_t>;
template<typename T> INLINE double similarity(const T &a, const T &b) {
    return a.jaccard_index(b);
    //return jaccard_index(a, b);
}

template<> INLINE double similarity<CRMFinal>(const CRMFinal &a, const CRMFinal &b) {
    return a.histogram_intersection(b);
}

using RMFinal = mh::FinalRMinHash<uint64_t, std::greater<uint64_t>>;
namespace us {
template<typename T> INLINE double union_size(const T &a, const T &b) {
    throw NotImplementedError(std::string("union_size not available for type ") + __PRETTY_FUNCTION__);
}

#define US_DEC(type) \
template<> INLINE double union_size<type> (const type &a, const type &b) { \
    return a.union_size(b); \
}

US_DEC(RMFinal)
US_DEC(CRMFinal)
US_DEC(khset64_t)
US_DEC(hll::hllbase_t<>)
template<> INLINE double union_size<mh::FinalBBitMinHash> (const mh::FinalBBitMinHash &a, const mh::FinalBBitMinHash &b) {
    return (a.est_cardinality_ + b.est_cardinality_ ) / (1. + a.jaccard_index(b));
}
} // namespace us
template<typename T>
double containment_index(const T &a, const T &b) {
    return a.containment_index(b);
}
#define CONTAIN_OVERLOAD_FAIL(x)\
template<>\
double containment_index<x>(const x &b, const x &a) {\
    RUNTIME_ERROR(std::string("Containment index not implemented for ") + __PRETTY_FUNCTION__);\
}
CONTAIN_OVERLOAD_FAIL(RMFinal)
CONTAIN_OVERLOAD_FAIL(bf::bf_t)
CONTAIN_OVERLOAD_FAIL(wj::WeightedSketcher<RMFinal>)
CONTAIN_OVERLOAD_FAIL(wj::WeightedSketcher<bf::bf_t>)


void main_usage(char **argv) {
    std::fprintf(stderr, "Usage: %s <subcommand> [options...]. Use %s <subcommand> for more options. [Subcommands: sketch, dist, setdist, hll, printmat.]\n",
                 *argv, *argv);
    std::exit(EXIT_FAILURE);
}

size_t posix_fsize(const char *path) {
    struct stat st;
    stat(path, &st);
    return st.st_size;
}

size_t posix_fsizes(const std::string &path, const char sep=FNAME_SEP) {
    size_t ret = 0;
    for_each_substr([&ret](const char *s) {struct stat st; ::stat(s, &st); ret += st.st_size;}, path, sep);
    return ret;
}

namespace detail {
struct path_size {
    friend void swap(path_size&, path_size&);
    std::string path;
    size_t size;
    path_size(std::string &&p, size_t sz): path(std::move(p)), size(sz) {}
    path_size(const std::string &p, size_t sz): path(p), size(sz) {}
    path_size(path_size &&o): path(std::move(o.path)), size(o.size) {}
    path_size(): size(0) {}
    path_size &operator=(path_size &&o) {
        std::swap(o.path, path);
        std::swap(o.size, size);
        return *this;
    }
};

inline void swap(path_size &a, path_size &b) {
    std::swap(a.path, b.path);
    std::swap(a.size, b.size);
}

void sort_paths_by_fsize(std::vector<std::string> &paths) {
    if(paths.size() < 2) return;
    uint32_t *fsizes = static_cast<uint32_t *>(std::malloc(paths.size() * sizeof(uint32_t)));
    if(!fsizes) throw std::bad_alloc();
    #pragma omp parallel for
    for(size_t i = 0; i < paths.size(); ++i)
        fsizes[i] = posix_fsizes(paths[i].data());
    std::vector<path_size> ps(paths.size());
    #pragma omp parallel for
    for(size_t i = 0; i < paths.size(); ++i)
        ps[i] = path_size(paths[i], fsizes[i]);
    std::free(fsizes);
    std::sort(ps.begin(), ps.end(), [](const auto &x, const auto &y) {return x.size > y.size;});
    paths.clear();
    for(const auto &p: ps) paths.emplace_back(std::move(p.path));
}
} // namespace detail

void dist_usage(const char *arg) {
    std::fprintf(stderr, "Usage: %s <opts> [genome1 genome2 seq.fq [...] if not provided from a file with -F]\n"
                         "Flags:\n"
                         "-h/-?, --help\tUsage\n\n\n"
                         "===Encoding Options===\n\n"
                         "-k, --kmer-length\tSet kmer size [31], max 32\n"
                         "-s, --spacing\tadd a spacer of the format <int>x<int>,<int>x<int>,"
                         "..., where the first integer corresponds to the space "
                         "-w, --window-size\tSet window size [max(size of spaced kmer, [parameter])]\n"
                         "-S, --sketch-size\tSet sketch size [10, for 2**10 bytes each]\n"
                         "--use-nthash\tUse nthash for encoding. (not reversible, but fast, rolling, and specialized for DNA).\n"
                         "            \tAs a warning, this does not currently ignore Ns in reads, but it does allow us to use kmers with k > 32\n"
                         "--use-cyclic-hash\tUses a cyclic hash for encoding. Not reversible, but fast. Ns are correctly ignored.\n"
                         "-C, --no-canon\tDo not canonicalize. [Default: canonicalize]\n\n\n"
                         "===Output Files===\n\n"
                         "-o, --out-sizes\tOutput for genome size estimates [stdout]\n"
                         "-O, --out-dists\tOutput for genome distance matrix [stdout]\n\n\n"
                         "===Filtering Options===\n\n"
                         "-y, --countmin\tFilter all input data by count-min sketch.\n"
                         "--sketch-by-fname\tAutodetect fastq or fasta data by filename (.fq or .fastq within filename).\n"
                         " When filtering with count-min sketches by either -y or -N, set minimum count:"
                         "-c, --min-count\tSet minimum count for kmers to pass count-min filtering.\n"
                         "-q, --nhashes\tSet count-min number of hashes. Default: [4]\n"
                         "-t, --cm-sketch-size\tSet count-min sketch size (log2). Default: 20\n"
                         "-R, --seed\tSet seed for seeds for count-min sketches\n\n\n"
                         "===Runtime Options\n\n"
                         "-F, --paths\tGet paths to genomes from file rather than positional arguments\n"
                         "-W, --cache-sketches\tCache sketches/use cached sketches\n"
                         "-p, --nthreads\tSet number of threads [1]\n"
                         "--presketched\tTreat provided paths as pre-made sketches.\n"
                         "-P, --prefix\tSet prefix for sketch file locations [empty]\n"
                         "-x, --suffix\tSet suffix in sketch file names [empty]\n"
                         "--avoid-sorting\tAvoid sorting files by genome sizes. This avoids a computational step, but can result in degraded load-balancing.\n\n\n"
                         "===Emission Formats===\n\n"
                         "-b, --emit-binary\tEmit distances in binary (default: human-readable, upper-triangular)\n"
                         "-U, --phylip\tEmit distances in PHYLIP upper triangular format(default: human-readable, upper-triangular)\n"
                         "between bases repeated the second integer number of times\n"
                         "-T, --full-tsv\tpostprocess binary format to human-readable TSV (not upper triangular)\n\n\n"
                         "===Emission Details===\n\n"
                         "-e, --emit-scientific\tEmit in scientific notation\n\n\n"
                         "===Data Structures===\n\n"
                         "Default: HyperLogLog. Alternatives:\n"
                         "--use-bb-minhash/-8\tCreate b-bit minhash sketches\n"
                         "--use-bloom-filter\tCreate bloom filter sketches\n"
                         "--use-range-minhash\tCreate range minhash sketches\n"
                         "--use-super-minhash\tCreate b-bit superminhash sketches\n"
                         "--use-counting-range-minhash\tCreate range minhash sketches\n"
                         "--use-full-khash-sets\tUse full khash sets for comparisons, rather than sketches. This can take a lot of memory and time!\n\n\n"
                         "===Sketch-specific Options===\n\n"
                         "-I, --improved      \tUse Ertl's Improved Estimator for HLL\n"
                         "-E, --original      \tUse Ertl's Original Estimator for HLL\n"
                         "-J, --ertl-joint-mle\tUse Ertl's JMLE Estimator for HLL[default:Uses Ertl-MLE]\n\n\n"
                         "===b-bit Minhashing Options (apply for b-bit minhash and b-bit superminhash) ===\n\n"
                         "--bbits,-B\tSet `b` for b-bit minwise hashing to <int>. Default: 16\n\n\n"
                         "===Distance Emission Types===\n\n"
                         "Default: Jaccard Index\n"
                         "Alternatives:\n"
                         "-M, --mash-dist    \tEmit Mash distance [ji ? (-log(2. * ji / (1. + ji)) / k) : 1.]\n"
                         "--full-mash-dist   \tEmit full (not approximate) Mash distance. [1. - (2.*ji/(1. + ji))^(1/k)]\n"
                         "--sizes            \tEmit union sizes (default: jaccard index)\n"
                         "--containment-index\tEmit Containment Index (|A & B| / |A|)\n"
                         "--containment-dist \tEmit distance metric using containment index. [Let C = (|A & B| / |A|). C ? -log(C) / k : 1.] \n"
                         "--symmetric-containment-dist\tEmit symmetric containment index symcon(A, B) = max(C(A, B), C(B, A))\n"
                         "--symmetric-containment-index\ttEmit distance metric using maximum containment index. symdist(A, B) = min(cdist(A,B), cdist(B, A))\n"
                         "--full-containment-dist \tEmit distance metric using containment index, without log approximation. [Let C = (|A & B| / |A|). C ? 1. - C^(1/k) : 1.] \n"
                         "\n\n"
                         "===Count-min-based Streaming Weighted Jaccard===\n"
                         "--wj               \tEnable weighted jaccard adapter\n"
                         "--wj-cm-sketch-size\tSet count-min sketch size for count-min streaming weighted jaccard [16]\n"
                         "--wj-cm-nhashes    \tSet count-min sketch number of hashes for count-min streaming weighted jaccard [8]\n"
                , arg);
    std::exit(EXIT_FAILURE);
}


// Usage, utilities
void sketch_usage(const char *arg) {
    std::fprintf(stderr, "Usage: %s <opts> [genomes if not provided from a file with -F]\n"
                         "Flags:\n"
                         "-h/-?:\tEmit usage\n"
                         "\n\n"
                         "Sketch options --\n\n"
                         "--kmer-length/-k\tSet kmer size [31], max 32\n"
                         "--spacing/-s\tadd a spacer of the format <int>x<int>,<int>x<int>,"
                         "..., where the first integer corresponds to the space "
                         "between bases repeated the second integer number of times\n"
                         "--window-size/-w\tSet window size [max(size of spaced kmer, [parameter])]\n"
                         "--sketch-size/-S\tSet log2 sketch size in bytes [10, for 2**10 bytes each]\n"
                         "--no-canon/-C\tDo not canonicalize. [Default: canonicalize]\n"
                         "--bbits/-B\tSet `b` for b-bit minwise hashing to <int>. Default: 16\n\n\n"
                         "Run options --\n\n"
                         "--nthreads/-p\tSet number of threads [1]\n"
                         "--prefix/-P\tSet prefix for sketch file locations [empty]\n"
                         "--suffix/-x\tSet suffix in sketch file names [empty]\n"
                         "--paths/-F\tGet paths to genomes from file rather than positional arguments\n"
                         "--skip-cached/-c\tSkip alreday produced/cached sketches (save sketches to disk in directory of the file [default] or in folder specified by -P\n"
                         "--avoid-sorting\tAvoid sorting files by genome sizes. This avoids a computational step, but can result in degraded load-balancing.\n\n\n"
                         "\n\n"
                         "Estimation methods --\n\n"
                         "--original/-E\tUse Flajolet with inclusion/exclusion quantitation method for hll. [Default: Ertl MLE]\n"
                         "--improved/-I\tUse Ertl Improved estimator [Default: Ertl MLE]\n"
                         "--ertl-jmle/-J\tUse Ertl JMLE\n\n\n"
                         "Filtering Options --\n\n"
                         "Default: consume all kmers. Alternate options: \n"
                         "--sketch-by-fname\tAutodetect fastq or fasta data by filename (.fq or .fastq within filename).\n"
                         "--countmin/-b\tFilter all input data by count-min sketch.\n\n\n"
                         "Options for count-min filtering --\n\n"
                         "--nhashes/-H\tSet count-min number of hashes. Default: [4]\n"
                         "--cm-sketch-size/-q\tSet count-min sketch size (log2). Default: 20\n"
                         "--min-count/-n\tProvide minimum expected count for fastq data. If unspecified, all kmers are passed.\n"
                         "--seed/-R\tSet seed for seeds for count-min sketches\n\n\n"
                         "Sketch Type Options --\n\n"
                         "--use-bb-minhash/-8\tCreate b-bit minhash sketches\n"
                         "--use-bloom-filter\tCreate bloom filter sketches\n"
                         "--use-range-minhash\tCreate range minhash sketches\n"
                         "--use-super-minhash\tCreate b-bit super minhash sketches\n"
                         "--use-counting-range-minhash\tCreate range minhash sketches\n"
                         "--use-full-khash-sets\tUse full khash sets for comparisons, rather than sketches. This can take a lot of memory and time!\n"
                         "\n\n"
                         "===Count-min-based Streaming Weighted Jaccard===\n"
                         "--wj               \tEnable weighted jaccard adapter\n"
                         "--wj-cm-sketch-size\tSet count-min sketch size for count-min streaming weighted jaccard [16]\n"
                         "--wj-cm-nhashes    \tSet count-min sketch number of hashes for count-min streaming weighted jaccard [8]\n"
                , arg);
    std::exit(EXIT_FAILURE);
}

bool fname_is_fq(const std::string &path) {
    static const std::string fq1 = ".fastq", fq2 = ".fq";
    return path.find(fq1) != std::string::npos || path.find(fq2) != std::string::npos;
}


template<typename SketchType>
std::string make_fname(const char *path, size_t sketch_p, int wsz, int k, int csz, const std::string &spacing, const std::string &suffix="", const std::string &prefix="") {
    std::string ret(prefix);
    if(ret.size()) ret += '/';
    {
        const char *p, *p2;
#if 0
        if((p = std::strchr(path, FNAME_SEP)) != nullptr) {
            ++p;
        }
        else p = path;
#else
		p = (p = std::strchr(path, FNAME_SEP)) ? p + 1: path;
#endif
        std::fprintf(stderr, "p is '%s'\n", p);
        if(ret.size() && (p2 = strrchr(p, '/'))) ret += std::string(p2 + 1);
        else                                     ret += p;
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
    ret += SketchFileSuffix<SketchType>::suffix;
    return ret;
}

enum sketching_method: int {
    EXACT = 0,
    CBF   = 1,
    BY_FNAME = 2
};



/*
 *
  enc.for_each([&](u64 kmer){h.addh(kmer);}, inpaths[i].data(), &kseqs[tid]);\
 */

template<typename T>
INLINE void set_estim_and_jestim(T &x, hll::EstimationMethod estim, hll::JointEstimationMethod jestim) {}

template<typename Hashstruct>
INLINE void set_estim_and_jestim(hll::hllbase_t<Hashstruct> &h, hll::EstimationMethod estim, hll::JointEstimationMethod jestim) {
    h.set_estim(estim);
    h.set_jestim(jestim);
}
using hll::EstimationMethod;
using hll::JointEstimationMethod;

template<typename T> T construct(size_t ssarg);
template<typename T, bool is_weighted>
struct Constructor;
template<typename T> struct Constructor<T, false> {
    static auto create(size_t ssarg) {
        return T(ssarg);
    }
};
template<typename T> struct Constructor<T, true> {
    static auto create(size_t ssarg) {
        using base_type = typename T::base_type;
        using cm_type = typename T::cm_type;
        return T(cm_type(16, gargs.weighted_jaccard_cmsize, gargs.weighted_jaccard_nhashes), construct<base_type>(ssarg));
    }
};

template<typename T>
T construct(size_t ssarg) {
    Constructor<T, wj::is_weighted_sketch<T>::value> constructor;
    return constructor.create(ssarg);
}

template<> mh::BBitMinHasher<uint64_t> construct<mh::BBitMinHasher<uint64_t>>(size_t p) {return mh::BBitMinHasher<uint64_t>(p, gargs.bbnbits);}

template<typename SketchType>
void sketch_core(uint32_t ssarg, uint32_t nthreads, uint32_t wsz, uint32_t k, const Spacer &sp, const std::vector<std::string> &inpaths, const std::string &suffix, const std::string &prefix, std::vector<cm::ccm_t> &cms, EstimationMethod estim, JointEstimationMethod jestim, KSeqBufferHolder &kseqs, const std::vector<bool> &use_filter, const std::string &spacing, bool skip_cached, bool canon, uint32_t mincount, bool entropy_minimization, EncodingType enct=BONSAI) {
    std::vector<SketchType> sketches;
    uint32_t sketch_size = bytesl2_to_arg(ssarg, SketchEnum<SketchType>::value);
    while(sketches.size() < (u32)nthreads) sketches.push_back(construct<SketchType>(sketch_size)), set_estim_and_jestim(sketches.back(), estim, jestim);
    std::vector<std::string> fnames(nthreads);
    RollingHasher<uint64_t> rolling_hasher(k, canon);
#if !NDEBUG
    for(size_t i = 0; i < inpaths.size(); std::fprintf(stderr, "Path: %s at %i\n", inpaths[i].data(), i), ++i);
#endif

#define MAIN_SKETCH_LOOP(MinType)\
    for(size_t i = 0; i < inpaths.size(); ++i) {\
        const int tid = omp_get_thread_num();\
        std::string &fname = fnames[tid];\
        fname = make_fname<SketchType>(inpaths[i].data(), sketch_size, wsz, k, sp.c_, spacing, suffix, prefix);\
        LOG_DEBUG("fname: %s from %s\n", fname.data(), inpaths[i].data());\
        if(skip_cached && isfile(fname)) continue;\
        Encoder<MinType> enc(nullptr, 0, sp, nullptr, canon);\
        auto &h = sketches[tid];\
        if(use_filter.size() && use_filter[i]) {\
            auto &cm = cms[tid];\
            if(enct == NTHASH) {\
                for_each_substr([&](const char *s) {enc.for_each_hash([&](u64 kmer){if(cm.addh(kmer) >= mincount) h.add(kmer);}, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            } else if(enct == BONSAI) {\
                for_each_substr([&](const char *s) {enc.for_each([&](u64 kmer){if(cm.addh(kmer) >= mincount) h.addh(kmer);}, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            } else {\
                for_each_substr([&](const char *s) {rolling_hasher.for_each_hash([&](u64 kmer){if(cm.addh(kmer) >= mincount) h.addh(kmer);}, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            }\
            cm.clear();  \
        } else {\
            if(enct == NTHASH) {\
                for_each_substr([&](const char *s) {enc.for_each_hash([&](u64 kmer){h.add(kmer);}, inpaths[i].data(), &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            } else if(enct == BONSAI) {\
                for_each_substr([&](const char *s) {enc.for_each([&](u64 kmer){h.addh(kmer);}, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            } else {\
                for_each_substr([&](const char *s) {rolling_hasher.for_each_hash([&](u64 kmer){h.addh(kmer);}, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            }\
        }\
        h.write(fname.data());\
        h.clear();\
    }
    if(entropy_minimization) {
        #pragma omp parallel for schedule(dynamic)
        MAIN_SKETCH_LOOP(bns::score::Entropy)
    } else {
        #pragma omp parallel for schedule(dynamic)
        MAIN_SKETCH_LOOP(bns::score::Lex)
    }
#undef MAIN_SKETCH_LOOP
}

#define LO_ARG(LONG, SHORT) {LONG, required_argument, 0, SHORT},
#define LO_NO(LONG, SHORT) {LONG, no_argument, 0, SHORT},
#define LO_FLAG(LONG, SHORT, VAR, VAL) {LONG, no_argument, (int *)&VAR, VAL},

#define SKETCH_LONG_OPTS \
static option_struct sketch_long_options[] = {\
    LO_FLAG("countmin", 'b', sm, CBF)\
    LO_FLAG("sketch-by-fname", 'f', sm, BY_FNAME)\
    LO_FLAG("no-canon", 'C', canon, false)\
    LO_FLAG("skip-cached", 'c', skip_cached, true)\
    LO_FLAG("by-entropy", 'e', entropy_minimization, true) \
    LO_FLAG("use-bb-minhash", '8', sketch_type, BB_MINHASH)\
    LO_ARG("bbits", 'B')\
    LO_ARG("paths", 'F')\
    LO_ARG("prefix", 'P')\
    LO_ARG("nhashes", 'H')\
    LO_ARG("original", 'E')\
    LO_ARG("improved", 'I')\
    LO_ARG("ertl-joint-mle", 'J')\
    LO_ARG("seed", 'R')\
    LO_ARG("sketch-size", 'S')\
    LO_ARG("kmer-length", 'k')\
    LO_ARG("min-count", 'n')\
    LO_ARG("nthreads", 'p')\
    LO_ARG("cm-sketch-size", 'q')\
    LO_ARG("spacing", 's')\
    LO_ARG("window-size", 'w')\
    LO_ARG("suffix", 'x')\
    LO_ARG("wj-cm-sketch-size", 136)\
    LO_ARG("wj-cm-nhashes", 137)\
    LO_ARG("suffix", 'x')\
\
    LO_FLAG("use-range-minhash", 128, sketch_type, RANGE_MINHASH)\
    LO_FLAG("use-counting-range-minhash", 129, sketch_type, COUNTING_RANGE_MINHASH)\
    LO_FLAG("use-full-khash-sets", 130, sketch_type, FULL_KHASH_SET)\
    LO_FLAG("use-bloom-filter", 131, sketch_type, BLOOM_FILTER)\
    LO_FLAG("use-super-minhash", 132, sketch_type, BB_SUPERMINHASH)\
    LO_FLAG("use-nthash", 133, enct, NTHASH)\
    LO_FLAG("use-cyclic-hash", 134, enct, CYCLIC)\
    LO_FLAG("avoid-sorting", 135, avoid_fsorting, true)\
    LO_FLAG("wj", 138, weighted_jaccard, true)\
    {0,0,0,0}\
};

// Main functions
int sketch_main(int argc, char *argv[]) {
    int wsz(0), k(31), sketch_size(10), skip_cached(false), co, nthreads(1), mincount(1), nhashes(4), cmsketchsize(-1);
    int canon(true);
    int entropy_minimization = false, avoid_fsorting = false, weighted_jaccard = false;
    hll::EstimationMethod estim = hll::EstimationMethod::ERTL_MLE;
    hll::JointEstimationMethod jestim = static_cast<hll::JointEstimationMethod>(hll::EstimationMethod::ERTL_MLE);
    std::string spacing, paths_file, suffix, prefix;
    sketching_method sm = EXACT;
    Sketch sketch_type = HLL;
    EncodingType enct = BONSAI;
    uint64_t seedseedseed = 1337u;
    int option_index = 0;
    SKETCH_LONG_OPTS
    while((co = getopt_long(argc, argv, "n:P:F:p:x:R:s:S:k:w:H:q:B:8JbfjEIcCeh?", sketch_long_options, &option_index)) >= 0) {
        switch(co) {
            case 'B': gargs.bbnbits = std::atoi(optarg); break;
            case 'F': paths_file = optarg; break;
            case 'H': nhashes = std::atoi(optarg); break;
            case 'E': jestim = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ORIGINAL); break;
            case 'I': jestim = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ERTL_IMPROVED); break;
            case 'J': jestim = hll::JointEstimationMethod::ERTL_JOINT_MLE; break;
            case 'P': prefix = optarg; break;
            case 'R': seedseedseed = std::strtoull(optarg, nullptr, 10); break;
            case 'S': sketch_size = std::atoi(optarg); break;
            case 'k': k = std::atoi(optarg); break;
            case '8': sketch_type = BB_MINHASH; break;
            case 'b': sm = CBF; break;
            case 136:
                gargs.weighted_jaccard_cmsize  = std::atoi(optarg); weighted_jaccard = true; break;
            case 137:
                gargs.weighted_jaccard_nhashes = std::atoi(optarg); weighted_jaccard = true; break;
            case 'n':
                      mincount = std::atoi(optarg);
                      std::fprintf(stderr, "mincount: %d\n", mincount);
                      break;
            case 'p': nthreads = std::atoi(optarg); break;
            case 'q': cmsketchsize = std::atoi(optarg); break;
            case 's': spacing = optarg; break;
            case 'w': wsz = std::atoi(optarg); break;
            case 'x': suffix = optarg; break;
            case 'h': case '?': sketch_usage(*argv); break;
        }
    }
    if(k > 32 && enct == BONSAI)
        RUNTIME_ERROR("k must be <= 32 for non-rolling hashes.");
    if(k > 32 && spacing.size())
        RUNTIME_ERROR("kmers must be unspaced for k > 32");
    nthreads = std::max(nthreads, 1);
    omp_set_num_threads(nthreads);
    Spacer sp(k, wsz, parse_spacing(spacing.data(), k));
    std::vector<bool> use_filter;
    std::vector<cm::ccm_t> cms;
    std::vector<std::string> inpaths(paths_file.size() ? get_paths(paths_file.data())
                                                       : std::vector<std::string>(argv + optind, argv + argc));
    LOG_INFO("Sketching genomes with sketch: %d/%s\n", sketch_type, sketch_names[sketch_type]);
    if(inpaths.empty()) {
        std::fprintf(stderr, "No paths. See usage.\n");
        sketch_usage(*argv);
    }
    if(!avoid_fsorting)
        detail::sort_paths_by_fsize(inpaths);
    if(sm != EXACT) {
        if(cmsketchsize < 0) {
            cmsketchsize = 20;
            LOG_WARNING("Note: count-min sketch size not set. Defaulting to 20 for log2(sketch_size).\n");
        }
        if(sm == CBF)
            use_filter = std::vector<bool>(inpaths.size(), true);
        else // BY_FNAME
            for(const auto &path: inpaths) use_filter.emplace_back(fname_is_fq(path));
        auto nbits = std::log2(mincount) + 1;
        while(cms.size() < unsigned(nthreads))
            cms.emplace_back(nbits, cmsketchsize, nhashes, (cms.size() ^ seedseedseed) * 1337uL);
    }
    KSeqBufferHolder kseqs(nthreads);
    if(wsz < sp.c_) wsz = sp.c_;
#define SKETCH_CORE(type) \
    sketch_core<type>(sketch_size, nthreads, wsz, k, sp, inpaths,\
                            suffix, prefix, cms, estim, jestim,\
                            kseqs, use_filter, spacing, skip_cached, canon, mincount, entropy_minimization, enct)
    switch(sketch_type) {
        case HLL: SKETCH_CORE(hll::hll_t); break;
        case BLOOM_FILTER: SKETCH_CORE(bf::bf_t); break;
        case RANGE_MINHASH: SKETCH_CORE(mh::RangeMinHash<uint64_t>); break;
        case COUNTING_RANGE_MINHASH: SKETCH_CORE(mh::CountingRangeMinHash<uint64_t>); break;
        case BB_MINHASH: SKETCH_CORE(mh::BBitMinHasher<uint64_t>); break;
        case BB_SUPERMINHASH: SKETCH_CORE(SuperMinHashType); break;
        default: {
            char buf[128];
            std::sprintf(buf, "Sketch %s not yet supported.\n", (size_t(sketch_type) >= (sizeof(sketch_names) / sizeof(char *)) ? "Not such sketch": sketch_names[sketch_type]));
            RUNTIME_ERROR(buf);
        }
    }
#undef SKETCH_CORE
    LOG_INFO("Successfully finished sketching from %zu files\n", inpaths.size());
    return EXIT_SUCCESS;
}


template<typename FType, typename=typename std::enable_if<std::is_floating_point<FType>::value>::type>
size_t submit_emit_dists(int pairfi, const FType *ptr, u64 hs, size_t index, ks::string &str, const std::vector<std::string> &inpaths, EmissionFormat emit_fmt, bool use_scientific, const size_t buffer_flush_size=BUFFER_FLUSH_SIZE) {
    if(emit_fmt & BINARY) {
        const ssize_t nbytes = sizeof(FType) * (hs - index - 1);
        LOG_DEBUG("Writing %zd bytes for %zu items\n", nbytes, (hs - index - 1));
        ssize_t i = ::write(pairfi, ptr, nbytes);
        if(i != nbytes) {
            std::fprintf(stderr, "written %zd bytes instead of expected %zd\n", i, nbytes);
        }
    } else {
        auto &strref = inpaths[index];
        str += strref;
        if(emit_fmt == UT_TSV) {
            const char *fmt = use_scientific ? "\t%e": "\t%f";
            {
                u64 k;
                for(k = 0; k < index + 1;  ++k, kputsn_("\t-", 2, reinterpret_cast<kstring_t *>(&str)));
                for(k = 0; k < hs - index - 1; str.sprintf(fmt, ptr[k++]));
            }
        } else { // emit_fmt == UPPER_TRIANGULAR
            const char *fmt = use_scientific ? " %e": " %f";
            if(strref.size() < 9)
                str.append(9 - strref.size(), ' ');
            for(u64 k = 0; k < hs - index - 1; str.sprintf(fmt, ptr[k++]));
        }
        str.putc_('\n');
        str.flush(pairfi);
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

template<typename FType1, typename FType2,
         typename=typename std::enable_if<
            std::is_floating_point<FType1>::value && std::is_floating_point<FType2>::value
          >::type
         >
typename std::common_type<FType1, FType2>::type containment_dist(FType1 containment, FType2 ksinv) {
    // Adapter from Mash https://github.com/Marbl/Mash
    return containment ? -std::log(containment) * ksinv: 1.;
}

template<typename FType1, typename FType2,
         typename=typename std::enable_if<
            std::is_floating_point<FType1>::value && std::is_floating_point<FType2>::value
          >::type
         >
typename std::common_type<FType1, FType2>::type full_dist_index(FType1 ji, FType2 ksinv) {
    return 1. - std::pow(2.*ji/(1. + ji), ksinv);
}

template<typename FType1, typename FType2,
         typename=typename std::enable_if<
            std::is_floating_point<FType1>::value && std::is_floating_point<FType2>::value
          >::type
         >
typename std::common_type<FType1, FType2>::type full_containment_dist(FType1 containment, FType2 ksinv) {
    return 1. - std::pow(containment, ksinv);
}

template<typename SketchType, typename T, typename Func>
INLINE void perform_core_op(T &dists, size_t nhlls, SketchType *hlls, const Func &func, size_t i) {
    auto &h1 = hlls[i];
    #pragma omp parallel for schedule(dynamic)
    for(size_t j = i + 1; j < nhlls; ++j)
        dists[j - i - 1] = func(hlls[j], h1);
    h1.free();
}

#define CORE_ITER(zomg) do {\
        switch(result_type) {\
            case MASH_DIST: {\
                perform_core_op(dists, nsketches, hlls, [ksinv](const auto &x, const auto &y) {return dist_index(similarity<const SketchType>(x, y), ksinv);}, i);\
                break;\
            }\
            case JI: {\
            perform_core_op(dists, nsketches, hlls, similarity<const SketchType>, i);\
                break;\
            }\
            case SIZES: {\
            perform_core_op(dists, nsketches, hlls, us::union_size<SketchType>, i);\
                break;\
            }\
            case FULL_MASH_DIST:\
                perform_core_op(dists, nsketches, hlls, [ksinv](const auto &x, const auto &y) {return full_dist_index(similarity<const SketchType>(x, y), ksinv);}, i);\
                break;\
            case SYMMETRIC_CONTAINMENT_DIST:\
                perform_core_op(dists, nsketches, hlls, [ksinv](const auto &x, const auto &y) {return dist_index(std::max(containment_index(x, y), containment_index(y, x)), ksinv);}, i);\
                break;\
            case SYMMETRIC_CONTAINMENT_INDEX:\
                perform_core_op(dists, nsketches, hlls, [](const auto &x, const auto &y) {return std::max(containment_index(x, y), containment_index(y, x));}, i);\
                break;\
            default: __builtin_unreachable();\
        } } while(0)

template<typename SketchType>
void partdist_loop(std::FILE *ofp, SketchType *hlls, const std::vector<std::string> &inpaths, const bool use_scientific, const unsigned k, const EmissionType result_type, EmissionFormat emit_fmt, int nthreads, const size_t buffer_flush_size,
                   size_t nq)
{
    const float ksinv = 1./ k;
    if(nq >= inpaths.size()) {
        RUNTIME_ERROR(ks::sprintf("Wrong number of query/references. (ip size: %zu, nq: %zu\n", inpaths.size(), nq).data());
    }
    size_t nr = inpaths.size() - nq;
    float *arr = static_cast<float *>(std::malloc(nr * nq * sizeof(float)));
    if(!arr) throw std::bad_alloc();
#if TIMING
    auto start = std::chrono::high_resolution_clock::now();
#endif
    std::future<void> write_future, fmt_future;
    std::array<ks::string, 2> buffers;
    for(auto &b: buffers) b.resize(4 * nr);
    for(size_t qi = nr; qi < inpaths.size(); ++qi) {
        size_t qind =  qi - nr;
        switch(result_type) {
#define dist_sim(x, y) dist_index(similarity(x, y), ksinv)
#define fulldist_sim(x, y) full_dist_index(similarity(x, y), ksinv)
#define fullcont_sim(x, y) full_containment_dist(containment_index(x, y), ksinv)
#define cont_sim(x, y) containment_dist(containment_index(x, y), ksinv)
#define DO_LOOP(func)\
                for(size_t j = 0; j < nr; ++j) {\
                    arr[qind * nr + j] = func(hlls[j], hlls[qi]);\
                }
            case MASH_DIST:
                #pragma omp parallel for schedule(dynamic)
                DO_LOOP(dist_sim);
                break;
            case FULL_MASH_DIST:
                #pragma omp parallel for schedule(dynamic)
                DO_LOOP(fulldist_sim);
                break;
            case JI:
                #pragma omp parallel for schedule(dynamic)
                DO_LOOP(similarity);
                break;
            case SIZES:
                #pragma omp parallel for schedule(dynamic)
                DO_LOOP(us::union_size);
                break;
            case CONTAINMENT_INDEX:
                #pragma omp parallel for schedule(dynamic)
                DO_LOOP(containment_index);
                break;
            case CONTAINMENT_DIST:
                #pragma omp parallel for schedule(dynamic)
                DO_LOOP(cont_sim);
                break;
            case FULL_CONTAINMENT_DIST:
                #pragma omp parallel for schedule(dynamic)
                DO_LOOP(fullcont_sim);
                break;
            default: __builtin_unreachable();
#undef DO_LOOP
        }
        switch(emit_fmt) {
            case BINARY:
                if(write_future.valid()) write_future.get();
                write_future = std::async(std::launch::async, [ptr=arr + (qi - nr) * nq, nb=sizeof(float) * nr](const int fn) {
                    if(unlikely(::write(fn, ptr, nb) != ssize_t(nb))) RUNTIME_ERROR("Error writing to binary file");
                }, ::fileno(ofp));
                break;
            case UT_TSV: case UPPER_TRIANGULAR: default:
                // RUNTIME_ERROR(std::string("Illegal output format. numeric: ") + std::to_string(int(emit_fmt)));
            case FULL_TSV:
                if(fmt_future.valid()) fmt_future.get();
                fmt_future = std::async(std::launch::async, [nr,qi,ofp,ind=qi-nr,&inpaths,use_scientific,arr,&buffers,&write_future]() {
                    auto &buffer = buffers[qi & 1];
                    buffer += inpaths[qi];
                    const char *fmt = use_scientific ? "\t%e": "\t%f";
                    float *aptr = arr + ind * nr;
                    for(size_t i = 0; i < nr; ++i) {
                        buffer.sprintf(fmt, aptr[i]);
                    }
                    buffer.putc_('\n');
                    if(write_future.valid()) write_future.get();
                    write_future = std::async(std::launch::async, [ofp,&buffer]() {buffer.flush(::fileno(ofp));});
                });
                break;
        }
    }
    if(fmt_future.valid()) fmt_future.get();
    if(write_future.valid()) write_future.get();
#if TIMING
    auto end = std::chrono::high_resolution_clock::now();
#endif
    std::free(arr);
}

template<typename SketchType>
void dist_loop(std::FILE *ofp, SketchType *hlls, const std::vector<std::string> &inpaths, const bool use_scientific, const unsigned k, const EmissionType result_type, EmissionFormat emit_fmt, int nthreads, const size_t buffer_flush_size=BUFFER_FLUSH_SIZE, size_t nq=0) {
    if(nq) {
        partdist_loop<SketchType>(ofp, hlls, inpaths, use_scientific, k, result_type, emit_fmt, nthreads, buffer_flush_size, nq);
        return;
    }
    if(!is_symmetric(result_type)) {
        char buf[1024];
        std::sprintf(buf, "Can't perform symmetric distance comparisons with a symmetric method (%s/%d). To perform an asymmetric distance comparison between a given set and itself, provide the same list of filenames to both -Q and -F.\n", emt2str(result_type), int(result_type));
        RUNTIME_ERROR(buf);
    }
    const float ksinv = 1./ k;
    const int pairfi = fileno(ofp);
    omp_set_num_threads(nthreads);
    const size_t nsketches = inpaths.size();
    if((emit_fmt & BINARY) == 0) {
        std::future<size_t> submitter;
        std::array<std::vector<float>, 2> dps;
        dps[0].resize(nsketches - 1);
        dps[1].resize(nsketches - 2);
        ks::string str;
        for(size_t i = 0; i < nsketches; ++i) {
            std::vector<float> &dists = dps[i & 1];
            CORE_ITER(_a);
            LOG_DEBUG("Finished chunk %zu of %zu\n", i + 1, nsketches);
            if(i) submitter.get();
            submitter = std::async(std::launch::async, submit_emit_dists<float>,
                                   pairfi, dists.data(), nsketches, i,
                                   std::ref(str), std::ref(inpaths), emit_fmt, use_scientific, buffer_flush_size);
        }
        submitter.get();
    } else {
        dm::DistanceMatrix<float> dm(nsketches);
        for(size_t i = 0; i < nsketches; ++i) {
            auto span = dm.row_span(i);
            auto &dists = span.first;
            CORE_ITER(_b);
        }
        if(emit_fmt == FULL_TSV) dm.printf(ofp, use_scientific, &inpaths);
        else {
            assert(emit_fmt == BINARY);
            dm.write(ofp);
        }
    }
}

namespace {
enum CompReading: unsigned {
    UNCOMPRESSED,
    GZ,
    AUTODETECT
};
}

#define FILL_SKETCH_MIN(MinType)  \
    {\
        Encoder<MinType> enc(nullptr, 0, sp, nullptr, canon);\
        if(cms.empty()) {\
            auto &h = sketch;\
            if(enct == BONSAI) for_each_substr([&](const char *s) {enc.for_each([&](u64 kmer){h.addh(kmer);}, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            else if(enct == NTHASH) for_each_substr([&](const char *s) {enc.for_each_hash([&](u64 kmer){h.addh(kmer);}, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            else for_each_substr([&](const char *s) {rolling_hasher.for_each_hash([&](u64 kmer){h.addh(kmer);}, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
        } else {\
            sketch::cm::ccm_t &cm = cms.at(tid);\
            const auto lfunc = [&](u64 kmer){if(cm.addh(kmer) >= mincount) sketch.addh(kmer);};\
            if(enct == BONSAI)      for_each_substr([&](const char *s) {enc.for_each(lfunc, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            else if(enct == NTHASH) for_each_substr([&](const char *s) {enc.for_each_hash(lfunc, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            else                    for_each_substr([&](const char *s) {rolling_hasher.for_each_hash(lfunc, s, &kseqs[tid]);}, inpaths[i], FNAME_SEP);\
            cm.clear();\
        }\
        CONST_IF(!samesketch) new(final_sketches + i) final_type(std::move(sketch)); \
    }


template<typename SketchType>
void dist_sketch_and_cmp(const std::vector<std::string> &inpaths, std::vector<sketch::cm::ccm_t> &cms, KSeqBufferHolder &kseqs, std::FILE *ofp, std::FILE *pairofp,
                         Spacer sp,
                         unsigned ssarg, unsigned mincount, EstimationMethod estim, JointEstimationMethod jestim, bool cache_sketch, EmissionType result_type, EmissionFormat emit_fmt,
                         bool presketched_only, unsigned nthreads, bool use_scientific, std::string suffix, std::string prefix, bool canon, bool entropy_minimization, std::string spacing,
                         size_t nq=0, EncodingType enct=BONSAI)
{
    // nq -- number of queries
    //       for convenience, we will perform our comparisons (all-p) against (all-q) [remainder]
    //       and use the same guts for all portions of the process
    //       except for the final comparison and output.
    assert(nq <= inpaths.size());
    using final_type = typename FinalSketch<SketchType>::final_type;
    std::vector<SketchType> sketches;
    sketches.reserve(inpaths.size());
    uint32_t sketch_size = bytesl2_to_arg(ssarg, SketchEnum<SketchType>::value);
    while(sketches.size() < inpaths.size()) {
        sketches.emplace_back(construct<SketchType>(sketch_size));
        set_estim_and_jestim(sketches.back(), estim, jestim);
    }
    static constexpr bool samesketch = std::is_same<SketchType, final_type>::value;
    final_type *final_sketches =
        samesketch ? reinterpret_cast<final_type *>(sketches.data())
                   : static_cast<final_type *>(std::malloc(sizeof(*final_sketches) * inpaths.size()));
    CONST_IF(samesketch) {
        if(final_sketches == nullptr) throw std::bad_alloc();
    }

    std::atomic<uint32_t> ncomplete;
    ncomplete.store(0);
    const unsigned k = sp.k_;
    const unsigned wsz = sp.w_;
    RollingHasher<uint64_t> rolling_hasher(k, canon);
    #pragma omp parallel for schedule(dynamic)
    for(size_t i = 0; i < sketches.size(); ++i) {
        const std::string &path(inpaths[i]);
        auto &sketch = sketches[i];
        if(presketched_only)  {
            CONST_IF(samesketch) {
                sketch.read(path);
                set_estim_and_jestim(sketch, estim, jestim); // HLL is the only type that needs this, and it's the same
            } else new(final_sketches + i) final_type(path.data()); // Read from path
        } else {
            const std::string fpath(make_fname<SketchType>(path.data(), sketch_size, wsz, k, sp.c_, spacing, suffix, prefix));
            const bool isf = isfile(fpath);
            if(cache_sketch && isf) {
                LOG_DEBUG("Sketch found at %s with size %zu, %u\n", fpath.data(), size_t(1ull << sketch_size), sketch_size);
                CONST_IF(samesketch) {
                    sketch.read(fpath);
                    set_estim_and_jestim(sketch, estim, jestim);
                } else {
                    new(final_sketches + i) final_type(fpath);
                }
            } else {
                const int tid = omp_get_thread_num();
                if(entropy_minimization) {
                    FILL_SKETCH_MIN(score::Entropy);
                } else {
                    FILL_SKETCH_MIN(score::Lex);
                }
                CONST_IF(samesketch) {
                    if(cache_sketch && !isf) sketch.write(fpath);
                } else if(cache_sketch) final_sketches[i].write(fpath);
            }
        }
        ++ncomplete; // Atomic
    }
    kseqs.free();
    ks::string str("#Path\tSize (est.)\n");
    assert(str == "#Path\tSize (est.)\n");
    str.resize(BUFFER_FLUSH_SIZE);
    {
        const int fn(fileno(ofp));
        for(size_t i(0); i < sketches.size(); ++i) {
            double card;
            CONST_IF(samesketch) card = cardinality_estimate(sketches[i]);
            else                 card = cardinality_estimate(final_sketches[i]);
            str.sprintf("%s\t%lf\n", inpaths[i].data(), card);
            if(str.size() >= BUFFER_FLUSH_SIZE) str.flush(fn);
        }
        str.flush(fn);
    }
    if(ofp != stdout) std::fclose(ofp);
    str.clear();
    if(emit_fmt == UT_TSV) {
        str.sprintf("##Names\t");
        for(size_t i = 0; i < inpaths.size() - nq; ++i)
            str.sprintf("%s\t", inpaths[i].data());
        str.back() = '\n';
        str.write(fileno(pairofp)); str.free();
    } else if(emit_fmt == UPPER_TRIANGULAR) { // emit_fmt == UPPER_TRIANGULAR
        std::fprintf(pairofp, "%zu\n", inpaths.size());
        std::fflush(pairofp);
    }
    dist_loop<final_type>(pairofp, final_sketches, inpaths, use_scientific, k, result_type, emit_fmt, nthreads, BUFFER_FLUSH_SIZE, nq);
    CONST_IF(!samesketch) {
#if __cplusplus >= 201703L
        std::destroy_n(
#  if __cpp_lib_execution
            std::execution::par_unseq,
#  endif
            final_sketches, inpaths.size());
#else
        std::for_each(final_sketches, final_sketches + inpaths.size(), [](auto &sketch) {
            using destructor_type = typename std::decay<decltype(sketch)>::type;
            sketch.~destructor_type();
        });
#endif
        std::free(final_sketches);
    }
}

#define DIST_LONG_OPTS \
static option_struct dist_long_options[] = {\
    LO_FLAG("full-tsv", 'T', emit_fmt, FULL_TSV)\
    LO_FLAG("emit-binary", 'b', emit_fmt, BINARY)\
    LO_FLAG("phylip", 'U', emit_fmt, UPPER_TRIANGULAR)\
    LO_FLAG("no-canon", 'C', canon, false)\
    LO_FLAG("by-entropy", 'g', entropy_minimization, true) \
    LO_FLAG("use-bb-minhash", '8', sketch_type, BB_MINHASH)\
    LO_FLAG("full-mash-dist", 'l', result_type, FULL_MASH_DIST)\
    LO_FLAG("mash-dist", 'M', result_type, MASH_DIST)\
    LO_FLAG("countmin", 'y', sm, CBF)\
    LO_FLAG("sketch-by-fname", 'N', sm, BY_FNAME)\
    LO_FLAG("sizes", 'Z', result_type, SIZES)\
    LO_FLAG("use-scientific", 'e', use_scientific, true)\
    LO_FLAG("cache-sketches", 'W', cache_sketch, true)\
    LO_FLAG("presketched", 'H', presketched_only, true)\
    LO_FLAG("avoid-sorting", 'n', avoid_fsorting, true)\
    LO_ARG("out-sizes", 'o') \
    LO_ARG("query-paths", 'Q') \
    LO_ARG("out-dists", 'O') \
    LO_ARG("bbits", 'B')\
    LO_ARG("original", 'E')\
    LO_ARG("improved", 'I')\
    LO_ARG("ertl-joint-mle", 'J')\
    LO_ARG("ertl-mle", 'm')\
    LO_ARG("paths", 'F')\
    LO_ARG("prefix", 'P')\
    LO_ARG("nhashes", 'q')\
    LO_ARG("seed", 'R')\
    LO_ARG("sketch-size", 'S')\
    LO_ARG("bbits", 'B')\
    LO_ARG("original", 'E')\
    LO_ARG("improved", 'I')\
    LO_ARG("ertl-joint-mle", 'J')\
    LO_ARG("ertl-mle", 'm')\
    LO_ARG("paths", 'F')\
    LO_ARG("prefix", 'P')\
    LO_ARG("nhashes", 'q')\
    LO_ARG("seed", 'R')\
    LO_ARG("sketch-size", 'S')\
    LO_ARG("kmer-length", 'k')\
    LO_ARG("min-count", 'c')\
    LO_ARG("nthreads", 'p')\
    LO_ARG("cm-sketch-size", 't')\
    LO_ARG("spacing", 's')\
    LO_ARG("window-size", 'w')\
    LO_ARG("suffix", 'x')\
    LO_ARG("help", 'h')\
    LO_FLAG("use-range-minhash", 128, sketch_type, RANGE_MINHASH)\
    LO_FLAG("use-counting-range-minhash", 129, sketch_type, COUNTING_RANGE_MINHASH)\
    LO_FLAG("use-full-khash-sets", 130, sketch_type, FULL_KHASH_SET)\
    LO_FLAG("containment-index", 131, result_type, CONTAINMENT_INDEX) \
    LO_FLAG("containment-dist", 132, result_type, CONTAINMENT_DIST) \
    LO_FLAG("full-containment-dist", 133, result_type, FULL_CONTAINMENT_DIST) \
    LO_FLAG("use-bloom-filter", 134, sketch_type, BLOOM_FILTER)\
    LO_FLAG("use-super-minhash", 135, sketch_type, BB_SUPERMINHASH)\
    LO_FLAG("use-nthash", 136, enct, NTHASH)\
    LO_FLAG("symmetric-containment-index", 137, result_type, SYMMETRIC_CONTAINMENT_INDEX) \
    LO_FLAG("symmetric-containment-dist", 138, result_type, SYMMETRIC_CONTAINMENT_DIST) \
    LO_FLAG("use-cyclic-hash", 139, enct, CYCLIC)\
    LO_ARG("wj-cm-sketch-size", 140)\
    LO_ARG("wj-cm-nhashes", 141)\
    LO_FLAG("wj", 142, weighted_jaccard, true)\
    {0,0,0,0}\
};

int dist_main(int argc, char *argv[]) {
    int wsz(0), k(31), sketch_size(10), use_scientific(false), co, cache_sketch(false),
        nthreads(1), mincount(5), nhashes(4), cmsketchsize(-1);
    int canon(true), presketched_only(false), entropy_minimization(false),
         avoid_fsorting(false), weighted_jaccard(false);
    Sketch sketch_type = HLL;
         // bool sketch_query_by_seq(true);
    EmissionFormat emit_fmt = UT_TSV;
    EncodingType enct = BONSAI;
    EmissionType result_type(JI);
    hll::EstimationMethod estim = hll::EstimationMethod::ERTL_MLE;
    hll::JointEstimationMethod jestim = static_cast<hll::JointEstimationMethod>(hll::EstimationMethod::ERTL_MLE);
    std::string spacing, paths_file, suffix, prefix, pairofp_labels, pairofp_path;
    FILE *ofp(stdout), *pairofp(stdout);
    sketching_method sm = EXACT;
    std::vector<std::string> querypaths;
    uint64_t seedseedseed = 1337u;
    if(argc == 1) dist_usage(*argv);
    int option_index;
    DIST_LONG_OPTS
    while((co = getopt_long(argc, argv, "n:Q:P:x:F:c:p:o:s:w:O:S:k:=:t:R:8TgDazlICbMEeHJhZBNyUmqW?", dist_long_options, &option_index)) >= 0) {
        switch(co) {
            case '8': sketch_type = BB_MINHASH; break;
            case 'B': gargs.bbnbits = std::atoi(optarg);   break;
            case 'F': paths_file = optarg;              break;
            case 'P': prefix     = optarg;              break;
            case 'U': emit_fmt   =  UPPER_TRIANGULAR;   break;
            case 'l': result_type = FULL_MASH_DIST;     break;
            case 'T': emit_fmt = FULL_TSV;              break;
            case 'Q': querypaths = get_paths(optarg); break;
            case 'R': seedseedseed = std::strtoull(optarg, nullptr, 10); break;
            case 'E': jestim   = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ORIGINAL); break;
            case 'I': jestim   = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ERTL_IMPROVED); break;
            case 'J': jestim   = hll::JointEstimationMethod::ERTL_JOINT_MLE; break;
            case 'm': jestim   = (hll::JointEstimationMethod)(estim = hll::EstimationMethod::ERTL_MLE); LOG_WARNING("Note: ERTL_MLE is default. This flag is redundant.\n"); break;
            case 'S': sketch_size = std::atoi(optarg);  break;
            case 'e': use_scientific = true; break;
            case 'C': canon = false; break;
            case 'b': emit_fmt = BINARY;                break;
            case 'c': mincount = std::atoi(optarg);     break;
            case 'g': entropy_minimization = true; LOG_WARNING("Entropy-based minimization is probably theoretically ill-founded, but it might be of practical value.\n"); break;
            case 'k': k        = std::atoi(optarg);           break;
            case 'M': result_type = MASH_DIST; break;
            case 'o': if((ofp = fopen(optarg, "w")) == nullptr) LOG_EXIT("Could not open file at %s for writing.\n", optarg); break;
            case 'p': nthreads = std::atoi(optarg);     break;
            case 'q': nhashes  = std::atoi(optarg);     break;
            case 't': cmsketchsize = std::atoi(optarg); break;
            case 's': spacing  = optarg;                break;
            case 'w': wsz      = std::atoi(optarg);         break;
            case 'W': cache_sketch = true; break;
            case 'x': suffix   = optarg;                 break;
            case 'O': if((pairofp = fopen(optarg, "wb")) == nullptr)
                          LOG_EXIT("Could not open file at %s for writing.\n", optarg);
                      pairofp_labels = std::string(optarg) + ".labels";
                      pairofp_path = optarg;
                      break;
            case 140:
                gargs.weighted_jaccard_cmsize  = std::atoi(optarg); weighted_jaccard = true; break;
            case 141:
                gargs.weighted_jaccard_nhashes = std::atoi(optarg); weighted_jaccard = true; break;
            case 'h': case '?': dist_usage(*argv);
        }
    }
    if(k > 32 && enct == BONSAI)
        RUNTIME_ERROR("k must be <= 32 for non-rolling hashes.");
    if(k > 32 && spacing.size())
        RUNTIME_ERROR("kmers must be unspaced for k > 32");
    if(nthreads < 0) nthreads = 1;
    std::vector<std::string> inpaths(paths_file.size() ? get_paths(paths_file.data())
                                                       : std::vector<std::string>(argv + optind, argv + argc));
    if(inpaths.empty())
        std::fprintf(stderr, "No paths. See usage.\n"), dist_usage(*argv);
    omp_set_num_threads(nthreads);
    Spacer sp(k, wsz, parse_spacing(spacing.data(), k));
    size_t nq = querypaths.size();
    if(nq == 0 && !is_symmetric(result_type)) {
        querypaths = inpaths;
        nq = querypaths.size();
        LOG_WARNING("=====Note===== No query files provided, but an asymmetric distance was requested. Switching to a query/reference format with all references as queries.\n"
                    "In the future, this will throw an error.\nYou must provide query and reference paths (-Q/-F) to calculate asymmetric distances.\n");
    }
    if(!presketched_only && !avoid_fsorting) {
        detail::sort_paths_by_fsize(inpaths);
        detail::sort_paths_by_fsize(querypaths);
    }
    inpaths.reserve(inpaths.size() + querypaths.size());
    for(auto &p: querypaths)
        inpaths.push_back(std::move(p));
    {
        decltype(querypaths) tmp;
        std::swap(tmp, querypaths);
    }
    std::vector<sketch::cm::ccm_t> cms;
    KSeqBufferHolder kseqs(nthreads);
    switch(sm) {
        case CBF: case BY_FNAME: {
            if(cmsketchsize < 0) {
                cmsketchsize = 20;
                LOG_WARNING("CM Sketch size not set. Defaulting to 20, 1048576 entries per table\n");
            }
            unsigned nbits = std::log2(mincount) + 1;
            cms.reserve(nthreads);
            while(cms.size() < static_cast<unsigned>(nthreads))
                cms.emplace_back(nbits, cmsketchsize, nhashes, (cms.size() ^ seedseedseed) * 1337uL);
            break;
        }
        case EXACT: default: break;
    }
    if(enct == NTHASH)
        std::fprintf(stderr, "Use nthash's rolling hash for kmers. This comes at the expense of reversibility\n");
#define CALL_DIST(sketchtype) \
    dist_sketch_and_cmp<sketchtype>(inpaths, cms, kseqs, ofp, pairofp, sp, sketch_size,\
                                    mincount, estim, jestim, cache_sketch, result_type,\
                                    emit_fmt, presketched_only, nthreads,\
                                    use_scientific, suffix, prefix, canon,\
                                    entropy_minimization, spacing, nq, enct)

#define CALL_DIST_WEIGHTED(sketchtype) CALL_DIST(sketch::wj::WeightedSketcher<sketchtype>)

#define CALL_DIST_BOTH(sketchtype) do {if(weighted_jaccard) CALL_DIST_WEIGHTED(sketchtype); else CALL_DIST(sketchtype);} while(0)

    switch(sketch_type) {
        case BB_MINHASH:      CALL_DIST_BOTH(mh::BBitMinHasher<uint64_t>); break;
        case BB_SUPERMINHASH: CALL_DIST_BOTH(SuperMinHashType); break;
        case HLL:             CALL_DIST_BOTH(hll::hll_t); break;
        case RANGE_MINHASH:   CALL_DIST_BOTH(mh::RangeMinHash<uint64_t>); break;
        case BLOOM_FILTER:    CALL_DIST_BOTH(bf::bf_t); break;
        case FULL_KHASH_SET:  CALL_DIST_BOTH(khset64_t); break;
        case COUNTING_RANGE_MINHASH:
                              CALL_DIST_BOTH(mh::CountingRangeMinHash<uint64_t>); break;
        default: {
                char buf[128];
                std::sprintf(buf, "Sketch %s not yet supported.\n", (size_t(sketch_type) >= (sizeof(sketch_names) / sizeof(char *)) ? "Not such sketch": sketch_names[sketch_type]));
                RUNTIME_ERROR(buf);
        }
    }

    std::future<void> label_future;
    if(emit_fmt == BINARY) {
        if(pairofp_labels.empty()) pairofp_labels = "unspecified";
        label_future = std::async(std::launch::async, [&inpaths](const std::string &labels) {
            std::FILE *fp = std::fopen(labels.data(), "wb");
            if(fp == nullptr) RUNTIME_ERROR(std::string("Could not open file at ") + labels);
            for(const auto &path: inpaths) std::fwrite(path.data(), path.size(), 1, fp), std::fputc('\n', fp);
            std::fclose(fp);
        }, pairofp_labels);
    }
    if(pairofp != stdout) std::fclose(pairofp);
    if(label_future.valid()) label_future.get();
    return EXIT_SUCCESS;
}

int print_binary_main(int argc, char *argv[]) {
    int c;
    bool use_scientific = false;
    std::string outpath;
    for(char **p(argv); *p; ++p) if(std::strcmp(*p, "-h") && std::strcmp(*p, "--help") == 0) goto usage;
    if(argc == 1) {
        usage:
        std::fprintf(stderr, "%s printmat <path to binary file> [- to read from stdin]\n"
                             "-o\tSpecify output file (default: stdout)\n"
                             "-s\tEmit in scientific notation\n",
                     argv ? static_cast<const char *>(*argv): "dashing");
        std::exit(EXIT_FAILURE);
    }
    while((c = getopt(argc, argv, ":o:sh?")) >= 0) {
        switch(c) {
            case 'o': outpath = optarg; break;
            case 's': use_scientific = true; break;
            case 'h': case '?': goto usage;
        }
    }
    std::FILE *fp;
    if(outpath.empty()) outpath = "/dev/stdout";
    dm::DistanceMatrix<float> mat(argv[optind]);
    if((fp = std::fopen(outpath.data(), "wb")) == nullptr) RUNTIME_ERROR(ks::sprintf("Could not open file at %s", outpath.data()).data());
    mat.printf(fp, use_scientific);
    std::fclose(fp);
    return EXIT_SUCCESS;
}

int setdist_main(int argc, char *argv[]) {
    LOG_WARNING("setdist_main is deprecated and will be removed. Instead, call `dashing dist` with --use-full-khash-sets to use hash sets instead of sketches.\n");
    return 1;
}

int hll_main(int argc, char *argv[]) {
    int c, wsz(0), k(31), num_threads(-1), sketch_size(24);
    bool canon(true);
    std::string spacing, paths_file;
    if(argc < 2) {
        usage: LOG_EXIT("Usage: %s <opts> <paths>\nFlags:\n"
                        "-k:\tkmer length (Default: 31. Max: 32)\n"
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

void union_usage [[noreturn]] (char *ex) {
    std::fprintf(stderr, "Usage: %s genome1 <genome2>...\n"
                         "Flags:\n"
                         "-o: Write union sketch to file [/dev/stdout]\n"
                         "-z: Emit compressed sketch\n"
                         "-Z: Set gzip compression level\n"
                         "-r: RangeMinHash sketches\n"
                         "-H: Full Khash Sets\n"
                         "-b: Bloom Filters\n"
                ,
                 ex);
    std::exit(1);
}

template<typename T>
T &merge(T &dest, const T &src) {
    return dest += src;
}

template<typename T>
void union_core(std::vector<std::string> &paths, gzFile ofp) {
    T ret(paths.back().data());
    paths.pop_back();
    if(paths.size() == 1) {
        merge(ret, T(paths.back().data()));
        paths.pop_back();
        assert(paths.empty());
        return;
    }
    T tmp(paths.back().data());
    paths.pop_back();
    merge(ret, tmp);
    while(paths.size()) {
        tmp.read(paths.back().data());
        paths.pop_back();
        merge(ret, tmp);
    }
    ret.write(ofp);
}

int union_main(int argc, char *argv[]) {
    if(std::find_if(argv, argc + argv,
                    [](const char *s) {return std::strcmp(s, "--help") == 0 || std::strcmp(s, "-h") == 0;})
       != argc + argv)
        union_usage(*argv);
    bool compress = false;
    int compression_level = 6;
    const char *opath = "/dev/stdout";
    std::vector<std::string> paths;
    Sketch sketch_type = HLL;
    for(int c;(c = getopt(argc, argv, "b:o:F:zZ:h?")) >= 0;) {
        switch(c) {
            case 'h': union_usage(*argv);
            case 'Z': compression_level = std::atoi(optarg); [[fallthrough]];
            case 'z': compress = true; break;
            case 'o': opath = optarg; break;
            case 'F': paths = get_paths(optarg); break;
            case 'r': sketch_type = RANGE_MINHASH; break;
            case 'H': sketch_type = FULL_KHASH_SET; break;
            case 'b': sketch_type = BLOOM_FILTER; break;
        }
    }
    if(argc == optind && paths.empty()) union_usage(*argv);
    std::for_each(argv + optind, argv + argc, [&](const char *s){paths.emplace_back(s);});
    char mode[6];
    if(compress && compression_level)
        std::sprintf(mode, "wb%d", compression_level % 23);
    else
        std::sprintf(mode, "wT");
    gzFile ofp = gzopen(opath, mode);
    if(!ofp) throw std::runtime_error(std::string("Could not open file at ") + opath);
    switch(sketch_type) {
        case HLL: union_core<hll::hll_t>(paths, ofp); break;
        case BLOOM_FILTER: union_core<bf::bf_t>(paths, ofp); break;
        case FULL_KHASH_SET: union_core<khset64_t>(paths, ofp); break;
        case RANGE_MINHASH: union_core<mh::FinalRMinHash<uint64_t>>(paths, ofp); break;
        default: throw sketch::common::NotImplementedError(ks::sprintf("Union not implemented for %s\n", sketch_names[sketch_type]).data());
    }
    gzclose(ofp);
    return 0;
}

int view_main(int argc, char *argv[]) {
    if(argc < 2) RUNTIME_ERROR("Usage: dashing view f1.hll [f2.hll ...]. Only HLLs currently supported.");
    for(int i = 1; i < argc; hll::hll_t(argv[i++]).printf(stdout));
    return 0;
}

} // namespace bns

using namespace bns;

void version_info(char *argv[]) {
    std::fprintf(stderr, "Dashing version: %s\n", DASHING_VERSION);
    std::exit(1);
}

int main(int argc, char *argv[]) {
    bns::executable = argv[0];
    std::fprintf(stderr, "Dashing version: %s\n", DASHING_VERSION);
    if(argc == 1) main_usage(argv);
    if(std::strcmp(argv[1], "sketch") == 0) return sketch_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "dist") == 0) return dist_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "union") == 0) return union_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "setdist") == 0) return setdist_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "hll") == 0) return hll_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "view") == 0) return view_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "printmat") == 0) return print_binary_main(argc - 1, argv + 1);
    else {
        for(const char *const *p(argv + 1); *p; ++p) {
            std::string v(*p);
            std::transform(v.begin(), v.end(), v.begin(), [](auto c) {return std::tolower(c);});
            if(v == "-h" || v == "--help") main_usage(argv);
            if(v == "-v" || v == "--version") version_info(argv);
        }
        std::fprintf(stderr, "Usage: %s <subcommand> [options...]. Use %s <subcommand> for more options. [Subcommands: sketch, dist, setdist, hll, union, printmat, view.]\n",
                     *argv, *argv);
        RUNTIME_ERROR(std::string("Invalid subcommand ") + argv[1] + " provided.");
    }
}
