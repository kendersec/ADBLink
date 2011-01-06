#include <debug.h>
#include <cmdline.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <ctype.h>

extern char *optarg;
extern int optind, opterr, optopt;

static struct option long_options[] = {
    {"start-address", required_argument, 0, 's'},
    {"inc-address",   required_argument, 0, 'i'},
    {"locals-only",   no_argument,       0, 'l'},
    {"quiet",         no_argument,       0, 'Q'},
    {"noupdate",      no_argument,       0, 'n'},
    {"lookup",        required_argument, 0, 'L'},
    {"default",       required_argument, 0, 'D'},
    {"verbose",       no_argument,       0, 'V'},
    {"help",          no_argument,       0, 'h'},
	{"mapfile",       required_argument, 0, 'M'},
	{"output",        required_argument, 0, 'o'},
    {"prelinkmap",    required_argument, 0, 'p'},
    {0, 0, 0, 0},
};

/* This array must parallel long_options[] */
static const char *descriptions[] = {
    "start address to prelink libraries to",
    "address increment for each library",
    "prelink local relocations only",
    "suppress informational and non-fatal error messages",
    "do a dry run--calculate the prelink info but do not update any files",
    "provide a directory for library lookup",
    "provide a default library or executable for symbol lookup",
    "print verbose output",
    "print help screen",
	"print a list of prelink addresses to file (prefix filename with + to append instead of overwrite)",
    "specify an output directory (if multiple inputs) or file (is single input)",
    "specify a file with prelink addresses instead of a --start-address/--inc-address combination",
};

void print_help(const char *name) {
    fprintf(stdout,
            "invokation:\n"
            "\t%s file1 [file2 file3 ...] -Ldir1 [-Ldir2 ...] -saddr -iinc [-Vqn] [-M<logfile>]\n"
            "\t%s -l file [-Vqn] [-M<logfile>]\n"
            "\t%s -h\n\n", name, name, name);
    fprintf(stdout, "options:\n");
    struct option *opt = long_options;
    const char **desc = descriptions;
    while (opt->name) {
        fprintf(stdout, "\t-%c/--%s%s: %s\n",
                opt->val,
                opt->name,
                (opt->has_arg ? " (argument)" : ""),
                *desc);
        opt++;
        desc++;
    }
}

int get_options(int argc, char **argv,
                int *start_addr,
                int *inc_addr,
                int *locals_only,
                int *quiet,
                int *dry_run,
                char ***dirs,
                int *num_dirs,
                char ***defaults,
                int *num_defaults,
                int *verbose,
				char **mapfile,
                char **output,
                char **prelinkmap) {
    int c;

    ASSERT(dry_run); *dry_run = 0;
    ASSERT(quiet); *quiet = 0;
    ASSERT(verbose); *verbose = 0;
    ASSERT(dirs); *dirs = NULL;
    ASSERT(num_dirs); *num_dirs = 0;
    ASSERT(defaults); *defaults = NULL;
    ASSERT(num_defaults); *num_defaults = 0;
    ASSERT(start_addr); *start_addr = -1;
    ASSERT(inc_addr); *inc_addr =   -1;
    ASSERT(locals_only); *locals_only = 0;
	ASSERT(mapfile); *mapfile = NULL;
	ASSERT(output); *output = NULL;
    ASSERT(prelinkmap); *prelinkmap = NULL;
    int dirs_size = 0;
    int defaults_size = 0;

    while (1) {
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long (argc, argv,
                         "VhnQlL:D:s:i:M:o:p:",
                         long_options,
                         &option_index);
        /* Detect the end of the options. */
        if (c == -1) break;

        if (isgraph(c)) {
            INFO ("option -%c with value `%s'\n", c, (optarg ?: "(null)"));
        }

#define SET_STRING_OPTION(name) do {                                   \
    ASSERT(optarg);                                                    \
    (*name) = strdup(optarg);                                          \
} while(0)

#define SET_REPEATED_STRING_OPTION(arr, num, size) do {                \
	if (*num == size) {                                                \
		size += 10;                                                    \
		*arr = (char **)REALLOC(*arr, size * sizeof(char *));          \
	}                                                                  \
	SET_STRING_OPTION(((*arr) + *num));                                \
	(*num)++;                                                          \
} while(0)

#define SET_INT_OPTION(val) do {                                       \
    ASSERT(optarg);                                                    \
	if (strlen(optarg) >= 2 && optarg[0] == '0' && optarg[1] == 'x') { \
			FAILIF(1 != sscanf(optarg+2, "%x", val),                   \
				   "Expecting a hexadecimal argument!\n");             \
	} else {                                                           \
		FAILIF(1 != sscanf(optarg, "%d", val),                         \
			   "Expecting a decimal argument!\n");                     \
	}                                                                  \
} while(0)

        switch (c) {
        case 0:
            /* If this option set a flag, do nothing else now. */
            if (long_options[option_index].flag != 0)
                break;
            INFO ("option %s", long_options[option_index].name);
            if (optarg)
                INFO (" with arg %s", optarg);
            INFO ("\n");
            break;
        case 'Q': *quiet = 1; break;
		case 'n': *dry_run = 1; break;
		case 'M':
			SET_STRING_OPTION(mapfile);
			break;
		case 'o':
			SET_STRING_OPTION(output);
			break;
        case 'p':
            SET_STRING_OPTION(prelinkmap);
            break;
        case 's':
            SET_INT_OPTION(start_addr);
            break;
        case 'i':
            SET_INT_OPTION(inc_addr);
            break;
        case 'L':
            SET_REPEATED_STRING_OPTION(dirs, num_dirs, dirs_size);
            break;
        case 'D':
            SET_REPEATED_STRING_OPTION(defaults, num_defaults, defaults_size);
            break;
        case 'l': *locals_only = 1; break;
        case 'h': print_help(argv[0]); exit(1); break;
        case 'V': *verbose = 1; break;
        case '?':
            /* getopt_long already printed an error message. */
            break;

#undef SET_STRING_OPTION
#undef SET_REPEATED_STRING_OPTION
#undef SET_INT_OPTION

        default:
            FAILIF(1, "Unknown option");
        }
    }

    return optind;
}
