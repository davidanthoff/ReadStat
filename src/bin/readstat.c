#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#include "../readstat.h"
#include "module.h"
#include "modules/mod_readstat.h"
#include "modules/mod_csv.h"

#if HAVE_XLSXWRITER
#include "modules/mod_xlsx.h"
#endif

#define RS_VERSION_STRING  "1.0-prerelease"

#define RS_FORMAT_UNKNOWN       0x00
#define RS_FORMAT_DTA           0x01
#define RS_FORMAT_SAV           0x02
#define RS_FORMAT_POR           0x04
#define RS_FORMAT_SAS_DATA      0x08
#define RS_FORMAT_SAS_CATALOG   0x10

#define RS_FORMAT_CAN_WRITE     (RS_FORMAT_DTA | RS_FORMAT_SAV)

typedef struct rs_ctx_s {
    rs_module_t *module;
    void        *module_ctx;
    long         row_count;
    long         var_count;
} rs_ctx_t;

int format(char *filename) {
    size_t len = strlen(filename);
    if (len < sizeof(".dta")-1)
        return RS_FORMAT_UNKNOWN;

    if (strncmp(filename + len - 4, ".dta", 4) == 0)
        return RS_FORMAT_DTA;

    if (strncmp(filename + len - 4, ".sav", 4) == 0)
        return RS_FORMAT_SAV;

    if (strncmp(filename + len - 4, ".por", 4) == 0)
        return RS_FORMAT_POR;

    if (len < sizeof(".sas7bdat")-1)
        return RS_FORMAT_UNKNOWN;

    if (strncmp(filename + len - 9, ".sas7bdat", 9) == 0)
        return RS_FORMAT_SAS_DATA;

    if (strncmp(filename + len - 9, ".sas7bcat", 9) == 0)
        return RS_FORMAT_SAS_CATALOG;

    return RS_FORMAT_UNKNOWN;
}

int is_catalog(char *filename) {
    return (format(filename) == RS_FORMAT_SAS_CATALOG);
}

int can_read(char *filename) {
    return (format(filename) != RS_FORMAT_UNKNOWN);
}

rs_module_t *rs_module_for_filename(rs_module_t *modules, long module_count, const char *filename) {
    int i;
    for (i=0; i<module_count; i++) {
        rs_module_t mod = modules[i];
        if (mod.accept(filename))
            return &modules[i];
    }
    return NULL;
}

int can_write(rs_module_t *modules, long modules_count, char *filename) {
    return (rs_module_for_filename(modules, modules_count, filename) != NULL);
}

static void handle_error(const char *msg, void *ctx) {
    dprintf(STDERR_FILENO, "%s", msg);
}

static int handle_fweight(int var_index, void *ctx) {
    rs_ctx_t *rs_ctx = (rs_ctx_t *)ctx;
    if (rs_ctx->module->handle_fweight) {
        return rs_ctx->module->handle_fweight(var_index, rs_ctx->module_ctx);
    }
    return 0;
}

static int handle_info(int obs_count, int var_count, void *ctx) {
    rs_ctx_t *rs_ctx = (rs_ctx_t *)ctx;
    if (rs_ctx->module->handle_info) {
        return rs_ctx->module->handle_info(obs_count, var_count, rs_ctx->module_ctx);
    }
    return 0;
}

static int handle_value_label(const char *val_labels, readstat_value_t value,
                              const char *label, void *ctx) {
    rs_ctx_t *rs_ctx = (rs_ctx_t *)ctx;
    if (rs_ctx->module->handle_value_label) {
        return rs_ctx->module->handle_value_label(val_labels, value, label, rs_ctx->module_ctx);
    }
    return 0;
}

static int handle_variable(int index, readstat_variable_t *variable,
                           const char *val_labels, void *ctx) {
    rs_ctx_t *rs_ctx = (rs_ctx_t *)ctx;
    if (rs_ctx->module->handle_variable) {
        return rs_ctx->module->handle_variable(index, variable, val_labels, rs_ctx->module_ctx);
    }
    return 0;
}

static int handle_value(int obs_index, int var_index, readstat_value_t value, void *ctx) {
    rs_ctx_t *rs_ctx = (rs_ctx_t *)ctx;
    if (var_index == 0) {
        rs_ctx->row_count++;
    }
    if (obs_index == 0) {
        rs_ctx->var_count++;
    }
    if (rs_ctx->module->handle_value) {
        return rs_ctx->module->handle_value(obs_index, var_index, value, rs_ctx->module_ctx);
    }
    return 0;
}

readstat_error_t parse_file(readstat_parser_t *parser, const char *input_filename, int input_format, void *ctx) {
    readstat_error_t error = READSTAT_OK;

    if (input_format == RS_FORMAT_DTA) {
        error = readstat_parse_dta(parser, input_filename, ctx);
    } else if (input_format == RS_FORMAT_SAV) {
        error = readstat_parse_sav(parser, input_filename, ctx);
    } else if (input_format == RS_FORMAT_POR) {
        error = readstat_parse_por(parser, input_filename, ctx);
    } else if (input_format == RS_FORMAT_SAS_DATA) {
        error = readstat_parse_sas7bdat(parser, input_filename, ctx);
    } else if (input_format == RS_FORMAT_SAS_CATALOG) {
        error = readstat_parse_sas7bcat(parser, input_filename, ctx);
    }

    return error;
}

void print_version() {
    dprintf(STDERR_FILENO, "ReadStat version " RS_VERSION_STRING "\n");
}

void print_usage(const char *cmd) {
    print_version();

    dprintf(STDERR_FILENO, "\n  Standard usage:\n");
    dprintf(STDERR_FILENO, "\n     %s input.(dta|por|sav|sas7bdat) output.(dta|sav|csv"
#if HAVE_XLSXWRITER
            "|xlsx"
#endif
            ")\n", cmd);
    dprintf(STDERR_FILENO, "\n  Usage if your value labels are stored in a separate SAS catalog file:\n");
    dprintf(STDERR_FILENO, "\n     %s input.sas7bdat catalog.sas7bcat output.(dta|sav|csv"
#if HAVE_XLSXWRITER
            "|xlsx"
#endif
            ")\n\n", cmd);
}

int main(int argc, char** argv) {
    struct timeval start_time, end_time;
    readstat_error_t error = READSTAT_OK;
    char *input_filename = NULL;
    char *catalog_filename = NULL;
    char *output_filename = NULL;

    rs_module_t *modules = NULL;
    long module_count = 2;
    long module_index = 0;

#if HAVE_XLSXWRITER
    module_count++;
#endif

    modules = calloc(module_count, sizeof(rs_module_t));

    modules[module_index++] = rs_mod_readstat;
    modules[module_index++] = rs_mod_csv;

#if HAVE_XLSXWRITER
    modules[module_index++] = rs_mod_xlsx;
#endif

    if (argc == 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        print_version();
        return 0;
    } else if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    } if (argc == 3) {
        if (!can_read(argv[1]) || !can_write(modules, module_count, argv[2])) {
            print_usage(argv[0]);
            return 1;
        }
        input_filename = argv[1];
        output_filename = argv[2];
    } else if (argc == 4) {
        if (!can_read(argv[1]) || !is_catalog(argv[2]) || !can_write(modules, module_count, argv[3])) {
            print_usage(argv[0]);
            return 1;
        }
        input_filename = argv[1];
        catalog_filename = argv[2];
        output_filename = argv[3];
    } else {
        print_usage(argv[0]);
        return 1;
    }

    int input_format = format(input_filename);
    rs_module_t *module = rs_module_for_filename(modules, module_count, output_filename);

    gettimeofday(&start_time, NULL);

    readstat_parser_t *pass1_parser = readstat_parser_init();
    readstat_parser_t *pass2_parser = readstat_parser_init();

    rs_ctx_t *rs_ctx = calloc(1, sizeof(rs_ctx_t));

    void *module_ctx = module->init(output_filename);

    if (module_ctx == NULL)
        goto cleanup;

    rs_ctx->module = module;
    rs_ctx->module_ctx = module_ctx;

    // Pass 1 - Collect fweight and value labels
    readstat_set_error_handler(pass1_parser, &handle_error);
    readstat_set_info_handler(pass1_parser, &handle_info);
    readstat_set_value_label_handler(pass1_parser, &handle_value_label);
    readstat_set_fweight_handler(pass1_parser, &handle_fweight);

    if (catalog_filename) {
        error = parse_file(pass1_parser, catalog_filename, RS_FORMAT_SAS_CATALOG, rs_ctx);
    } else {
        error = parse_file(pass1_parser, input_filename, input_format, rs_ctx);
    }
    if (error != READSTAT_OK)
        goto cleanup;

    // Pass 2 - Parse full file
    readstat_set_error_handler(pass2_parser, &handle_error);
    readstat_set_info_handler(pass2_parser, &handle_info);
    readstat_set_variable_handler(pass2_parser, &handle_variable);
    readstat_set_value_handler(pass2_parser, &handle_value);

    error = parse_file(pass2_parser, input_filename, input_format, rs_ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    gettimeofday(&end_time, NULL);

    dprintf(STDERR_FILENO, "Converted %ld variables and %ld rows in %.2lf seconds\n",
            rs_ctx->var_count, rs_ctx->row_count, 
            (end_time.tv_sec + 1e-6 * end_time.tv_usec) -
            (start_time.tv_sec + 1e-6 * start_time.tv_usec));

cleanup:
    readstat_parser_free(pass1_parser);
    readstat_parser_free(pass2_parser);

    if (module->finish) {
        module->finish(rs_ctx->module_ctx);
    }

    free(rs_ctx);

    if (error != READSTAT_OK) {
        dprintf(STDERR_FILENO, "%s\n", readstat_error_message(error));
        unlink(output_filename);
        return 1;
    }

    return 0;
}
