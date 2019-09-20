#include "dashing.h"
namespace bns {

// I think I dont want a gzFile here
template<typename T>
void dt_print_core(std::vector<std::string> &paths, std::FILE *ofp) {
    T ret(paths.back().data());
    
/*
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
	*/
	ret.printf(ofp);
}
int dt_print_main(int argc, char *argv[]) {
    if(std::find_if(argv, argc + argv,
                    [](const char *s) {return std::strcmp(s, "--help") == 0 || std::strcmp(s, "-h") == 0;})
       != argc + argv)
        dt_print_usage(*argv);
    bool compress = false;
    int compression_level = 6;
    const char *opath = "/dev/stdout";
    std::vector<std::string> paths;
    Sketch sketch_type = HLL;

	// Dont think I need these but keep em around for now
    for(int c;(c = getopt(argc, argv, "b:o:F:zZ:h?")) >= 0;) {
        switch(c) {
            case 'h': dt_print_usage(*argv);
            case 'Z': compression_level = std::atoi(optarg); [[fallthrough]];
            case 'z': compress = true; break;
            case 'o': opath = optarg; break;
            case 'F': paths = get_paths(optarg); break;
            //case 'r': sketch_type = RANGE_MINHASH; break;
            //case 'H': sketch_type = FULL_KHASH_SET; break;
            //case 'b': sketch_type = BLOOM_FILTER; break;
        }
    }
    if(argc == optind && paths.empty()) dt_print_usage(*argv);
    std::for_each(argv + optind, argv + argc, [&](const char *s){paths.emplace_back(s);});
    char mode[6];
    if(compress && compression_level)
        std::sprintf(mode, "wb%d", compression_level % 23);
    else
        std::sprintf(mode, "wT");
    
	//std::FILE *ofp = fopen(outpath.data(), "wb");
	std::FILE *ofp = fopen(opath, "wb");
	//gzFile ofp = gzopen(opath, mode);
    if(!ofp) throw std::runtime_error(std::string("Could not open file at ") + opath);
    switch(sketch_type) {
        case HLL: dt_print_core<hll::hll_t>(paths, ofp); break;
        //case BLOOM_FILTER: dt_print_core<bf::bf_t>(paths, ofp); break;
        //case FULL_KHASH_SET: dt_print_core<khset64_t>(paths, ofp); break;
        //case RANGE_MINHASH: dt_print_core<mh::FinalRMinHash<uint64_t>>(paths, ofp); break;
        default: throw NotImplementedError(ks::sprintf("Union not implemented for %s\n", sketch_names[sketch_type]).data());
    }
	std::fclose(ofp);
    //gzclose(ofp);
    return 0;
}

}
