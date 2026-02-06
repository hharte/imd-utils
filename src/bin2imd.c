/*
 * Binary to IMD Conversion Utilitiy (Cross-Platform.)
 *
 * www.github.com/hharte/imd-utils
 *
 * Copyright (c) 2025, Howard M. Harte
 *
 * Reference:
 * Original ImageDisk Utilities by Dave Dunfield (Dave's Old Computers)
 * http://dunfield.classiccmp.org/img/
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

#include "libimd.h" /* Use our IMD library */
#include "libimd_utils.h" /* For common utilities */

 /* Define version strings - replace with actual build system values if available */
#ifndef CMAKE_VERSION_STR
#define CMAKE_VERSION_STR "0.1.0" /* Placeholder version */
#endif
#ifndef GIT_VERSION_STR
#define GIT_VERSION_STR "dev" /* Placeholder git revision */
#endif

/* --- Constants --- */
#define MAX_FILENAME 260
#define MAX_HEADER_LINE 256
#define MAX_COMMENT_SIZE 65536
#define MAX_FORMAT_LINE 256
#define MAX_TRACK_DATA_BUFFER (LIBIMD_MAX_SECTORS_PER_TRACK * LIBIMD_MAX_SECTOR_SIZE)
#define MAX_TRACKS 256 /* Define MAX_TRACKS */

/* --- Data Structures --- */

/* Structure to hold format definition for a side */
typedef struct {
    int      mode_set;
    uint8_t  mode;             /* IMD Mode (0-5) */
    int      ssize_set;
    uint8_t  sector_size_code; /* IMD Sector Size Code (0-6) */
    uint32_t sector_size;      /* Actual size in bytes */
    int      smap_set;
    uint8_t  num_sectors;      /* Number of sectors */
    uint8_t  smap[LIBIMD_MAX_SECTORS_PER_TRACK]; /* Sector numbering map */
    int      cmap_set;         /* Store count here */
    uint8_t  cmap[LIBIMD_MAX_SECTORS_PER_TRACK]; /* Cylinder numbering map */
    int      hmap_set;         /* Store count here */
    uint8_t  hmap[LIBIMD_MAX_SECTORS_PER_TRACK]; /* Head numbering map */
} SideFormat;

/* Global options structure */
typedef struct {
    const char* input_filename;
    const char* output_filename;
    const char* format_filename;
    const char* comment_text;
    const char* comment_file;

    int verbose;            /* -V flag */
    int compression_mode;   /* IMD_COMPRESSION_* defines */
    int two_sides;          /* -1 / -2 flag */
    int cylinders_set;
    uint32_t num_cylinders; /* -N= value */
    int fill_specified;
    uint8_t fill_byte;      /* -F= value */
    int auto_yes;           /* -Y flag */

    SideFormat defaults[2]; /* Default format for side 0 and 1 */

} Options;

/* --- Global Variables --- */
static char* g_current_arg_ptr = NULL; /* For parsing helper functions */
static const char* g_current_context = NULL; /* For error messages (used locally) */

/* --- Helper Functions --- */

/* removed local imd_report_error_exit */
/* removed local warning */

/* Skips whitespace in the global argument pointer */
char skip_whitespace(void) {
    if (!g_current_arg_ptr) return '\0';
    while (*g_current_arg_ptr && isspace((unsigned char)*g_current_arg_ptr)) {
        g_current_arg_ptr++;
    }
    return *g_current_arg_ptr;
}

/* Parses a number from the global argument pointer */
/* Returns 1 on success, 0 on failure */
int parse_num_arg(unsigned long* val, int base, unsigned long low, unsigned long high) {
    char* endptr;
    if (!g_current_arg_ptr || !*g_current_arg_ptr) { return 0; } /* No value */

    /* Handle base prefixes */
    if (*g_current_arg_ptr == '$') { base = 16; g_current_arg_ptr++; }
    else if (*g_current_arg_ptr == '@') { base = 8; g_current_arg_ptr++; }
    else if (*g_current_arg_ptr == '%') { base = 2; g_current_arg_ptr++; }
    /* '.' prefix for decimal is handled implicitly */
    else if (*g_current_arg_ptr == '.') { base = 10; g_current_arg_ptr++; }


    *val = strtoul(g_current_arg_ptr, &endptr, base);
    if (endptr == g_current_arg_ptr) return 0; /* No digits parsed */

    if (*val < low || *val > high) {
        imd_report_error_exit("Value %lu out of range (%lu-%lu)", *val, low, high);
    }

    g_current_arg_ptr = endptr; /* Advance pointer */
    return 1;
}

/* Parses a map definition (e.g., 1,2,3-5,10.4) */
/* Returns number of elements parsed */
int parse_map_arg(uint8_t map[], int max_size, unsigned long low, unsigned long high) {
    int count = 0;
    unsigned long val1, val2, repeat_count;
    char separator;

    while (*g_current_arg_ptr && count < max_size) {
        if (!parse_num_arg(&val1, 10, low, high)) imd_report_error_exit("Expected number in map definition");
        if (val1 > 255) imd_report_error_exit("Map value %lu exceeds 255", val1);

        map[count++] = (uint8_t)val1;

        separator = *g_current_arg_ptr;
        if (separator == ',' || separator == '-' || separator == '.') {
            g_current_arg_ptr++; /* Consume separator */
        }
        else if (separator == '\0' || isspace((unsigned char)separator)) {
            break; /* End of map */
        }
        else {
            imd_report_error_exit("Unexpected character '%c' in map definition", separator);
        }

        if (separator == '-') { /* Range */
            if (!parse_num_arg(&val2, 10, low, high)) imd_report_error_exit("Expected end of range in map");
            if (val2 > 255) imd_report_error_exit("Map value %lu exceeds 255", val2);
            if (val2 > val1) {
                for (unsigned long v = val1 + 1; v <= val2 && count < max_size; ++v) map[count++] = (uint8_t)v;
            }
            else if (val1 > val2) {
                /* Need >= comparison for descending range */
                for (unsigned long v = val1 - 1; v >= val2 && count < max_size; --v) map[count++] = (uint8_t)v;
            }
            separator = *g_current_arg_ptr;
            if (separator == ',' || separator == '.') { g_current_arg_ptr++; }
            else if (separator != '\0' && !isspace((unsigned char)separator)) { imd_report_error_exit("Unexpected character after range"); }

        }
        else if (separator == '.') { /* Repeat */
            if (!parse_num_arg(&repeat_count, 10, 1, (unsigned long)max_size)) imd_report_error_exit("Expected repeat count in map"); /* Cast max_size */
            if (count > 0) { /* Need a value to repeat */
                uint8_t repeat_val = map[count - 1];
                if (repeat_count > 0) { /* Ensure repeat_count is positive */
                    for (unsigned long r = 1; r < repeat_count && count < max_size; ++r) { /* Repeat count-1 times */
                        map[count++] = repeat_val;
                    }
                }
            }
            separator = *g_current_arg_ptr;
            if (separator == ',') { g_current_arg_ptr++; }
            else if (separator != '\0' && !isspace((unsigned char)separator)) { imd_report_error_exit("Unexpected character after repeat"); }
        }

        if (separator == '\0' || isspace((unsigned char)separator)) break;
        if (separator != ',') imd_report_error_exit("Unexpected separator '%c' in map", separator);
        /* Comma separator handled implicitly by loop condition */

    }
    if (count >= max_size && *g_current_arg_ptr && !isspace((unsigned char)*g_current_arg_ptr)) {
        imd_report_error_exit("Map definition exceeds maximum size (%d)", max_size);
    }

    return count;
}


/* Parses a single format option (e.g., N=80, SS0=512, SM=1,2,3) */
/* Returns 1 if option parsed, 0 otherwise */
int parse_format_option(Options* opts, SideFormat format_defs[], const char* context) {
    /* FIX C4100: Mark opts as potentially unused */
    (void)opts;

    char opt_name[4] = { 0 }; /* Max 3 chars like SS0, SM1 etc. */
    int side_spec = -1; /* -1=both, 0=side0, 1=side1 */
    unsigned long val;
    size_t name_len = 0;
    g_current_context = context; /* Set context for error messages */

    /* Read option name (2 or 3 chars) */
    /* skip dash */
    if(*g_current_arg_ptr == '-')
        g_current_arg_ptr++;
    while (name_len < 3 && *g_current_arg_ptr && isalpha((unsigned char)*g_current_arg_ptr)) {
        opt_name[name_len++] = (char)toupper((unsigned char)*g_current_arg_ptr++); /* FIX C4244: Cast int to char */
    }
    if (name_len < 2) return 0; /* Not a valid option name */

    /* Check for side specifier (0 or 1) */
    if (name_len == 3 && isdigit((unsigned char)opt_name[2])) {
        side_spec = opt_name[2] - '0';
        if (side_spec > 1) imd_report_error_exit("Invalid side specifier '%c'", opt_name[2]);
        opt_name[2] = '\0'; /* Truncate name to 2 chars */
    }
    else if (name_len == 3) {
        return 0; /* Invalid 3-char option */
    }

    /* Expect '=' */
    if (*g_current_arg_ptr != '=') return 0;
    g_current_arg_ptr++; /* Skip '=' */

    SideFormat* target_defs[2] = { &format_defs[0], &format_defs[1] };
    int num_targets = (side_spec == -1) ? 2 : 1;
    int start_target = (side_spec == -1) ? 0 : side_spec;

    if (strcmp(opt_name, "DM") == 0) { /* Data Mode */
        if (!parse_num_arg(&val, 10, 0, 5)) imd_report_error_exit("Invalid value for DM");
        for (int i = 0; i < num_targets; ++i) { /* Use standard C for loop */
            target_defs[start_target + i]->mode = (uint8_t)val;
            target_defs[start_target + i]->mode_set = 1;
        }
    }
    else if (strcmp(opt_name, "SS") == 0) { /* Sector Size */
        if (!parse_num_arg(&val, 10, 128, 8192)) imd_report_error_exit("Invalid value for SS");
        uint8_t size_code = 0xFF;
        size_t lookup_count;
        const uint32_t* lookup = imd_get_sector_size_lookup(&lookup_count);
        for (uint8_t code = 0; code < lookup_count; ++code) {
            if (lookup[code] == val) {
                size_code = code;
                break;
            }
        }
        if (size_code == 0xFF) imd_report_error_exit("Unsupported sector size %lu for SS", val);
        for (int i = 0; i < num_targets; ++i) { /* Use standard C for loop */
            target_defs[start_target + i]->sector_size_code = size_code; /* Cast to uint8_t */
            target_defs[start_target + i]->sector_size = (uint32_t)val;
            target_defs[start_target + i]->ssize_set = 1;
        }
    }
    else if (strcmp(opt_name, "SM") == 0) { /* Sector Map */
        int n = parse_map_arg(target_defs[start_target]->smap, LIBIMD_MAX_SECTORS_PER_TRACK, 0, 255);
        if (n == 0) imd_report_error_exit("Empty sector map (SM) definition");
        target_defs[start_target]->num_sectors = (uint8_t)n; /* Cast to uint8_t */
        target_defs[start_target]->smap_set = 1;
        if (num_targets == 2) { /* Copy to other side if not side-specific */
            memcpy(target_defs[1]->smap, target_defs[0]->smap, (size_t)n);
            target_defs[1]->num_sectors = (uint8_t)n; /* Cast to uint8_t */
            target_defs[1]->smap_set = 1;
        }
    }
    else if (strcmp(opt_name, "CM") == 0) { /* Cylinder Map */
        int n = parse_map_arg(target_defs[start_target]->cmap, LIBIMD_MAX_SECTORS_PER_TRACK, 0, 255);
        if (n == 0) imd_report_error_exit("Empty cylinder map (CM) definition");
        target_defs[start_target]->cmap_set = n; /* Store count here temporarily */
        if (num_targets == 2) {
            memcpy(target_defs[1]->cmap, target_defs[0]->cmap, (size_t)n);
            target_defs[1]->cmap_set = n;
        }
    }
    else if (strcmp(opt_name, "HM") == 0) { /* Head Map */
        int n = parse_map_arg(target_defs[start_target]->hmap, LIBIMD_MAX_SECTORS_PER_TRACK, 0, 1);
        if (n == 0) imd_report_error_exit("Empty head map (HM) definition");
        target_defs[start_target]->hmap_set = n; /* Store count here temporarily */
        if (num_targets == 2) {
            memcpy(target_defs[1]->hmap, target_defs[0]->hmap, (size_t)n);
            target_defs[1]->hmap_set = n;
        }
    }
    else {
        /* Revert pointer if option name wasn't recognized */
        g_current_arg_ptr -= (name_len + 1); /* Backtrack past name and '=' */
        return 0; /* Not a recognized format option */
    }

    g_current_context = NULL; /* Clear context */
    return 1; /* Option parsed */
}

/* Prints usage information */
void print_usage(const char* prog_name) {
    const char* base_prog_name = imd_get_basename(prog_name); /* Use library function */
    if (!base_prog_name) base_prog_name = "bin2imd"; /* Fallback */

    printf("BIN2IMD (Cross-Platform) %s [%s] - Raw Binary to ImageDisk Converter\n", CMAKE_VERSION_STR, GIT_VERSION_STR);
    fprintf(stderr, "Copyright (C) 2025 - Howard M. Harte - https://github.com/hharte/imd-utils\n\n");
    fprintf(stderr, "The original MS-DOS version is available from http://dunfield.classiccmp.org/img/\n\n");
    fprintf(stderr, "Usage: %s binary-input-file IMD-output-file [option-file] [options]\n\n", base_prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  option-file    : Optional .B2I text file with track-specific format overrides.\n");
    fprintf(stderr, "  -1             : 1-sided output (default depends on format options).\n");
    fprintf(stderr, "  -2             : 2-sided output (default depends on format options).\n");
    fprintf(stderr, "  -C             : Write Compressed sectors if possible (default).\n");
    fprintf(stderr, "  -U             : Write Uncompressed sectors only.\n");
    fprintf(stderr, "  -V             : Verbose output.\n");
    fprintf(stderr, "  -Y             : Auto-Yes to overwrite prompt.\n");
    fprintf(stderr, "  -C=text        : Inline image Comment text (use ~ for space).\n");
    fprintf(stderr, "  -C@<file>      : Read image Comment from text file.\n");
    fprintf(stderr, "  -N=<cyls>      : Set Number of output cylinders (REQUIRED).\n");
    fprintf(stderr, "  -F=xx          : Missing sector Fill value (hex, default %02X).\n", LIBIMD_FILL_BYTE_DEFAULT);
    fprintf(stderr, "\nFormat Options (can be in option-file or command line):\n");
    fprintf(stderr, "  DM[0|1]=0-5    : Track Data Mode (0=500k FM, ..., 5=250k MFM).\n");
    fprintf(stderr, "  SS[0|1]=sz     : Track Sector Size (128, 256, ..., 8192).\n");
    fprintf(stderr, "  SM[0|1]=n,...  : Track Sector numbering Map (e.g., 1,2,3-5,10.4).\n");
    fprintf(stderr, "  CM[0|1]=n,...  : Track/sector Cylinder numbering Map (optional).\n");
    fprintf(stderr, "  HM[0|1]=n,...  : Track/sector Head numbering Map (optional, 0 or 1).\n");
    fprintf(stderr, "  (Options without 0/1 apply to both sides unless overridden).\n");
    fprintf(stderr, "  (Options in option-file override command line for specific tracks).\n");
    fprintf(stderr, "\nOption File (.B2I) Format:\n");
    fprintf(stderr, "  <track_num> [options...]\n");
    fprintf(stderr, "  Example: 0 DM=5 SS=512 SM=1,2,3\n");
    fprintf(stderr, "           40 DM=3 SS=1024 SM=0,1\n");
    fprintf(stderr, "  (Lines starting with ';' or blank are ignored).\n");
    fprintf(stderr, "  (Track numbers are 0-based physical track = cylinder * sides + head).\n");
    fprintf(stderr, "\n--help           : Display this help message and exit.\n");
}

/* Parses command line arguments */
void parse_args(int argc, char* argv[], Options* opts) {
    memset(opts, 0, sizeof(Options));
    opts->fill_byte = LIBIMD_FILL_BYTE_DEFAULT;
    opts->compression_mode = IMD_COMPRESSION_FORCE_COMPRESS; /* Default for new files */
    opts->two_sides = -1; /* Default: auto-detect based on options */

    int arg_index = 1;
    int file_count = 0;

    while (arg_index < argc) {
        char* arg = argv[arg_index];
        g_current_arg_ptr = arg; /* Set global pointer for helpers */

        if (strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }

        if (arg[0] == '-') {
            /* Cast to unsigned char for toupper */
            char opt_char = (char)toupper((unsigned char)arg[1]); /* FIX C4244: Cast int to char */
            char* value = NULL;

            if (strchr(arg, '=')) { /* Option with value */
                value = strchr(arg, '=') + 1;
                if (*value == '\0') {
                    imd_report(IMD_REPORT_LEVEL_WARNING, "Missing value after '=' for option %s", arg);
                    value = NULL;
                }
            }

            if (opt_char == '1' && !value && arg[2] == '\0') { opts->two_sides = 0; }
            else if (opt_char == '2' && !value && arg[2] == '\0') { opts->two_sides = 1; }
            else if (opt_char == 'C' && !value && arg[2] == '\0') { opts->compression_mode = IMD_COMPRESSION_FORCE_COMPRESS; }
            else if (opt_char == 'U' && !value && arg[2] == '\0') { opts->compression_mode = IMD_COMPRESSION_FORCE_DECOMPRESS; }
            else if (opt_char == 'V' && !value && arg[2] == '\0') { opts->verbose = 1; }
            else if (opt_char == 'Y' && !value && arg[2] == '\0') { opts->auto_yes = 1; }
            else if (opt_char == 'N' && value) {
                g_current_arg_ptr = value;
                unsigned long n;
                if (!parse_num_arg(&n, 10, 1, 255)) imd_report_error_exit("Invalid value for -N");
                opts->num_cylinders = (uint32_t)n;
                opts->cylinders_set = 1;
            }
            else if (opt_char == 'F' && value) {
                g_current_arg_ptr = value;
                unsigned long f;
                if (!parse_num_arg(&f, 16, 0, 255)) imd_report_error_exit("Invalid value for -F");
                opts->fill_byte = (uint8_t)f;
                opts->fill_specified = 1;
            }
            else if (opt_char == 'C' && value) { /* Comment */
                if (*value == '@') {
                    opts->comment_file = value + 1;
                    opts->comment_text = NULL;
                }
                else {
                    opts->comment_text = value;
                    opts->comment_file = NULL;
                }
            }
            else if (parse_format_option(opts, opts->defaults, "Command Line")) {
                /* Format option handled */
            }
            else {
                imd_report(IMD_REPORT_LEVEL_WARNING, "Unknown option '%s'", arg);
            }
        }
        else {
            /* File argument */
            if (file_count == 0) opts->input_filename = arg;
            else if (file_count == 1) opts->output_filename = arg;
            else if (file_count == 2) opts->format_filename = arg;
            else imd_report(IMD_REPORT_LEVEL_WARNING, "Ignoring extra file argument '%s'", arg);
            file_count++;
        }
        arg_index++;
        g_current_arg_ptr = NULL; /* Clear global pointer */
    }

    /* Auto-detect two_sides if not explicitly set */
    if (opts->two_sides == -1) {
        if (opts->defaults[1].mode_set || opts->defaults[1].ssize_set || opts->defaults[1].smap_set) {
            opts->two_sides = 1;
        }
        else {
            opts->two_sides = 0; /* Default to single sided if no side 1 options */
        }
    }

}

/* Validates format settings for a side */
void validate_side_format(const SideFormat* side, int side_num, const Options* opts) {
    /* FIX C4100: Mark opts as potentially unused */
    (void)opts;

    /* Local context for error reporting */
    char context[20];
    snprintf(context, sizeof(context), "Side %d Defaults", side_num);
    g_current_context = context;

    if (!side->mode_set) imd_report_error_exit("Data Mode (DM) must be defined");
    if (!side->ssize_set) imd_report_error_exit("Sector Size (SS) must be defined");
    if (!side->smap_set) imd_report_error_exit("Sector Map (SM) must be defined");
    if (side->num_sectors == 0) imd_report_error_exit("Sector Map (SM) cannot be empty");

    /* Check map consistency */
    if (side->cmap_set > 0 && side->cmap_set != side->num_sectors) {
        imd_report_error_exit("Cylinder Map (CM) size (%d) must match Sector Map size (%d)", side->cmap_set, side->num_sectors);
    }
    if (side->hmap_set > 0 && side->hmap_set != side->num_sectors) {
        imd_report_error_exit("Head Map (HM) size (%d) must match Sector Map size (%d)", side->hmap_set, side->num_sectors);
    }

    /* Check for duplicate sector numbers in smap */
    uint8_t seen[256] = { 0 };
    for (int i = 0; i < side->num_sectors; ++i) {
        if (seen[side->smap[i]]) {
            imd_report_error_exit("Duplicate sector number %u found in Sector Map (SM)", side->smap[i]);
        }
        seen[side->smap[i]] = 1;
    }
    g_current_context = NULL; /* Clear local context */
}

/* Reads the format file and applies overrides */
int read_format_file(Options* opts, SideFormat track_formats[][2], uint32_t max_cylinders) {
    FILE* ffmt = NULL;
    char line[MAX_FORMAT_LINE];
    unsigned long track_num_ul;
    uint32_t track_num;
    int line_num = 0;

    if (!opts->format_filename) return 0; /* No format file specified */

    ffmt = fopen(opts->format_filename, "r");
    if (!ffmt) {
        fprintf(stderr, "Error: Cannot open format file '%s': %s\n", opts->format_filename, strerror(errno));
        return -1; /* Error opening */
    }
    if (opts->verbose) printf("Reading format definition file: %s\n", opts->format_filename);
    g_current_context = opts->format_filename; /* Set context for parsing errors */
    while (fgets(line, sizeof(line), ffmt)) {
        line_num++;
        g_current_arg_ptr = line;
        skip_whitespace();

        if (*g_current_arg_ptr == ';' || *g_current_arg_ptr == '\0') {
            continue; /* Skip comment or blank lines */
        }

        /* Parse track number */
        /* Max track number is (max_cylinders * num_sides) - 1 */
        unsigned long max_track_num = (unsigned long)max_cylinders * (opts->two_sides + 1) - 1;
        if (!parse_num_arg(&track_num_ul, 10, 0, max_track_num)) {
            imd_report(IMD_REPORT_LEVEL_WARNING, "Format file line %d: Invalid track number", line_num);
            continue;
        }
        track_num = (uint32_t)track_num_ul;

        /* Initialize track format from defaults */
        uint32_t cyl = track_num / (opts->two_sides + 1);
        uint8_t head = (uint8_t)(track_num % (opts->two_sides + 1)); /* Cast result */
        /* No need to check cyl against max_cylinders here, already checked by parse_num_arg range */

        /* Copy defaults first */
        memcpy(&track_formats[cyl][head], &opts->defaults[head], sizeof(SideFormat));

        /* Parse options for this track */
        while (skip_whitespace()) {
            char context[30];
            snprintf(context, sizeof(context), "Format File Line %d", line_num);
            if (!parse_format_option(opts, track_formats[cyl], context)) {
                imd_report(IMD_REPORT_LEVEL_WARNING, "Format file line %d: Invalid option near '%s'", line_num, g_current_arg_ptr);
                break; /* Stop parsing this line */
            }
        }
        /* After parsing options for the line, validate the potentially modified format */
        /* Need to pass the specific track format being modified */
        validate_side_format(&track_formats[cyl][head], head, opts);
    }
    g_current_context = NULL; /* Clear context */
    fclose(ffmt);
    return 1; /* Format file processed */
}


/* --- Entry Point --- */

int main(int argc, char* argv[]) {
    Options opts;
    FILE* fin = NULL, * fout = NULL, * fcomment_src = NULL;
    char* comment_buffer = NULL;
    size_t comment_size = 0; /* Renamed */
    size_t comment_capacity = 0;
    int result = EXIT_FAILURE;
    /* Initialize to NULL */
    /* Initialize to NULL */
    /* Initialize to NULL */
    ImdTrackInfo current_track_info = { 0 };
    ImdWriteOpts write_opts = { 0 };
    uint8_t* track_data_buffer = NULL;
    SideFormat(*track_formats)[2] = NULL; /* [MAX_CYLINDERS][2] */
    char header_str[80];

    printf("BIN2IMD (Cross-Platform) %s [%s] - Raw Binary to ImageDisk Converter\n", CMAKE_VERSION_STR, GIT_VERSION_STR);

    /* Initialize Reporting */
    imd_set_verbosity(0, 0); /* Default: Not quiet, not verbose */

    /* --- Argument Parsing --- */
    parse_args(argc, argv, &opts);

    if (!opts.input_filename || !opts.output_filename) {
        print_usage(argv[0]);
        return 1;
    }
    if (!opts.cylinders_set) {
        imd_report_error_exit("-N=<cyls> option is required.");
    }
    /* Set verbosity level for reporting library AFTER parsing */
    imd_set_verbosity(0, opts.verbose); /* bin2imd has no -Q, verbose only */

    /* --- Validate Formats --- */
    validate_side_format(&opts.defaults[0], 0, &opts);
    if (opts.two_sides) {
        validate_side_format(&opts.defaults[1], 1, &opts);
    }

    /* --- Allocate Track Format Override Array --- */
    track_formats = calloc(opts.num_cylinders, sizeof(SideFormat[2]));
    if (!track_formats) {
        perror("Failed to allocate memory for track formats");
        goto cleanup;
    }
    /* Initialize with defaults */
    for (uint32_t c = 0; c < opts.num_cylinders; ++c) {
        memcpy(&track_formats[c][0], &opts.defaults[0], sizeof(SideFormat));
        if (opts.two_sides) {
            memcpy(&track_formats[c][1], &opts.defaults[1], sizeof(SideFormat));
        }
    }

    /* --- Read Format File Overrides --- */
    if (read_format_file(&opts, track_formats, opts.num_cylinders) < 0) {
        goto cleanup; /* Error occurred reading format file */
    }


    /* --- Open Files --- */
    fin = fopen(opts.input_filename, "rb");
    if (!fin) {
        fprintf(stderr, "Error: Cannot open input file '%s': %s\n", opts.input_filename, strerror(errno));
        goto cleanup;
    }

    if (!opts.auto_yes) {
        FILE* test_out = fopen(opts.output_filename, "rb");
        if (test_out) {
            fclose(test_out);
            printf("Output file '%s' already exists. Overwrite (Y/N)? ", opts.output_filename);
            fflush(stdout);
            int choice = getchar();
            int c_in; while ((c_in = getchar()) != '\n' && c_in != EOF);
            if (toupper((unsigned char)choice) != 'Y') {
                printf("Operation cancelled.\n");
                result = EXIT_SUCCESS;
                goto cleanup;
            }
        }
    }
    fout = fopen(opts.output_filename, "wb");
    if (!fout) {
        fprintf(stderr, "Error: Cannot open output file '%s': %s\n", opts.output_filename, strerror(errno));
        goto cleanup;
    }

    /* --- Allocate Track Data Buffer --- */
    track_data_buffer = (uint8_t*)malloc(MAX_TRACK_DATA_BUFFER);
    if (!track_data_buffer) {
        perror("Failed to allocate memory for track data buffer");
        goto cleanup;
    }


    /* --- Prepare and Write Header/Comment --- */
    comment_capacity = 1024;
    comment_buffer = (char*)malloc(comment_capacity);
    if (!comment_buffer) { perror("malloc comment"); goto cleanup; }
    comment_size = 0;
    memset(comment_buffer, 0, comment_capacity); /* Clear buffer */

    if (opts.comment_text) {
        strncpy(comment_buffer, opts.comment_text, comment_capacity - 1);
        comment_buffer[comment_capacity - 1] = '\0';
        comment_size = strlen(comment_buffer);
        /* Replace ~ with space as per original BIN2IMD */
        for (size_t i = 0; i < comment_size; ++i) {
            if (comment_buffer[i] == '~') comment_buffer[i] = ' ';
        }
        /* Add trailing CRLF if needed */
        if (comment_size > 0 && (comment_size < 2 || strncmp(&comment_buffer[comment_size - 2], "\r\n", 2) != 0)) {
            if (comment_size < comment_capacity - 3) {
                comment_buffer[comment_size++] = '\r';
                comment_buffer[comment_size++] = '\n';
                comment_buffer[comment_size] = '\0';
            }
        }
    }
    else if (opts.comment_file) {
        fcomment_src = fopen(opts.comment_file, "r");
        if (!fcomment_src) {
            fprintf(stderr, "Error opening comment file '%s': %s\n", opts.comment_file, strerror(errno));
        }
        else {
            comment_size = fread(comment_buffer, 1, comment_capacity - 1, fcomment_src);
            if (ferror(fcomment_src)) perror("Error reading comment file");
            comment_buffer[comment_size] = '\0';
            fclose(fcomment_src);
            fcomment_src = NULL;
        }
    }

    /* Add comment trailer */
    char trailer[100];
    snprintf(trailer, sizeof(trailer), "\r\nIMD file generated by BIN2IMD %s\r\n", CMAKE_VERSION_STR);
    size_t trailer_len = strlen(trailer);
    if (comment_size + trailer_len < comment_capacity) {
        strcat(comment_buffer, trailer);
        comment_size += trailer_len;
    }


    /* Write IMD Header */
    snprintf(header_str, sizeof(header_str), "BIN2IMD %s [%s]", CMAKE_VERSION_STR, GIT_VERSION_STR);
    if (imd_write_file_header(fout, header_str) != 0) {
        imd_report_error_exit("Failed to write IMD header.");
    }

    /* Write Comment */
    if (imd_write_comment_block(fout, comment_buffer, comment_size) != 0) {
        imd_report_error_exit("Failed to write comment block.");
    }
    free(comment_buffer); comment_buffer = NULL;


    /* --- Process and Write Tracks --- */
    if (opts.verbose) printf("Generating IMD file...\n");

    write_opts.compression_mode = opts.compression_mode; /* Use parsed mode */
    write_opts.force_non_bad = 0;
    write_opts.force_non_deleted = 0;
    write_opts.interleave_factor = LIBIMD_IL_AS_READ;
    for (int i = 0; i < LIBIMD_NUM_MODES; ++i) write_opts.tmode[i] = (uint8_t)i;

    uint64_t total_bytes_read = 0;
    uint64_t total_bytes_written = 0;

    for (uint8_t c = 0; c < opts.num_cylinders; ++c) {
        for (uint8_t h = 0; h <= (uint8_t)opts.two_sides; ++h) {
            SideFormat* fmt = &track_formats[c][h];
            size_t track_byte_size = (size_t)fmt->num_sectors * fmt->sector_size;

            if (opts.verbose > 1) {
                printf("Processing C:%u H:%u (Mode:%u Sectors:%u Size:%u)\n", c, h, fmt->mode, fmt->num_sectors, fmt->sector_size);
            }

            if (track_byte_size > MAX_TRACK_DATA_BUFFER) {
                imd_report_error_exit("Calculated track size (%zu bytes) exceeds buffer limit (%d bytes) for C:%u H:%u.",
                    track_byte_size, MAX_TRACK_DATA_BUFFER, c, h);
            }

            /* Read data for this track */
            size_t bytes_read = fread(track_data_buffer, 1, track_byte_size, fin);
            total_bytes_read += bytes_read;

            /* Pad if input file was short */
            if (bytes_read < track_byte_size) {
                if (feof(fin)) {
                    imd_report(IMD_REPORT_LEVEL_WARNING, "Input file ended early at C:%u H:%u. Padding %zu bytes with 0x%02X.",
                        c, h, track_byte_size - bytes_read, opts.fill_byte);
                    memset(track_data_buffer + bytes_read, opts.fill_byte, track_byte_size - bytes_read);
                }
                else {
                    perror("Error reading input binary file");
                    goto cleanup;
                }
            }

            /* Populate ImdTrackInfo */
            memset(&current_track_info, 0, sizeof(ImdTrackInfo));
            current_track_info.mode = fmt->mode;
            current_track_info.cyl = c;
            current_track_info.head = h;
            current_track_info.num_sectors = fmt->num_sectors;
            current_track_info.sector_size_code = fmt->sector_size_code;
            current_track_info.sector_size = fmt->sector_size;
            current_track_info.data = track_data_buffer;
            current_track_info.data_size = track_byte_size;
            current_track_info.loaded = 1;

            memcpy(current_track_info.smap, fmt->smap, fmt->num_sectors);
            if (fmt->cmap_set) {
                memcpy(current_track_info.cmap, fmt->cmap, fmt->num_sectors);
                current_track_info.hflag |= IMD_HFLAG_CMAP_PRES;
            }
            else { memset(current_track_info.cmap, c, fmt->num_sectors); }
            if (fmt->hmap_set) {
                memcpy(current_track_info.hmap, fmt->hmap, fmt->num_sectors);
                current_track_info.hflag |= IMD_HFLAG_HMAP_PRES;
            }
            else { memset(current_track_info.hmap, h, fmt->num_sectors); }

            /* Set all sector flags to 'Normal Data' initially for BIN2IMD */
            memset(current_track_info.sflag, IMD_SDR_NORMAL, fmt->num_sectors);

            /* Write the track using libimd */
            if (imd_write_track_imd(fout, &current_track_info, &write_opts) != 0) {
                imd_report_error_exit("Failed to write IMD track data for C:%u H:%u.", c, h);
            }
            total_bytes_written += track_byte_size;

        } /* End head loop */
    } /* End cylinder loop */

    if (opts.verbose) {
        printf("Successfully generated IMD file.\n");
        printf("Total bytes read from input: %llu\n", (unsigned long long)total_bytes_read);
        printf("Total sector bytes written: %llu\n", (unsigned long long)total_bytes_written);
        /* Check if there's remaining data in input */
        if (fgetc(fin) != EOF) {
            imd_report(IMD_REPORT_LEVEL_WARNING, "Input binary file contains more data than specified by format.");
        }
    }

    result = EXIT_SUCCESS; /* Success! */

cleanup:
    if (fin) fclose(fin);
    if (fout) fclose(fout);
    if (fcomment_src) fclose(fcomment_src);
    if (comment_buffer) free(comment_buffer);
    if (track_data_buffer) free(track_data_buffer);
    if (track_formats) free(track_formats);

    return (result == EXIT_SUCCESS ? 0 : 1);
}
