#include "dashing.h"
#include "sketch_and_cmp.h"
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
    else if(std::strcmp(argv[1], "cmp") == 0) return dist_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "union") == 0) return union_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "setdist") == 0) return setdist_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "hll") == 0) return hll_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "view") == 0) return view_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "mkdist") == 0) return mkdist_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "flatten") == 0) return flatten_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "printmat") == 0) return print_binary_main(argc - 1, argv + 1);
    else if(std::strcmp(argv[1], "dt_print") == 0) return dt_print_main(argc - 1, argv + 1);
	else {
        for(const char *const *p(argv + 1); *p; ++p) {
            std::string v(*p);
            std::transform(v.begin(), v.end(), v.begin(), [](auto c) {return std::tolower(c);});
            if(v == "-h" || v == "--help") main_usage(argv);
            if(v == "-v" || v == "--version") version_info(argv);
        }
        std::fprintf(stderr, "Usage: %s <subcommand> [options...]. Use %s <subcommand> for more options.\n"
                             "Subcommands:\nsketch\ndist\nhll\nunion\nprintmat\nview\nmkdist\nflatten\n\ncmp is also now a synonym for dist, which will be deprecated in the future.\n", *argv, *argv);
        RUNTIME_ERROR(std::string("Invalid subcommand ") + argv[1] + " provided.");
    }
}
