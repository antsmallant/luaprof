#ifndef LUAPROF_PPROF_EXPORTER_H
#define LUAPROF_PPROF_EXPORTER_H

#include "luaprof/runtime.h"

typedef enum lp_export_format {
	LP_EXPORT_PPROF = 0,
	LP_EXPORT_FOLDED = 1
} lp_export_format;

typedef struct lp_export_symbols {
	void *userdata;
	const char *(*cfunction_name)(void *userdata,
		lp_lua_cfunction function, size_t *length);
} lp_export_symbols;

bool lp_export_result(const lp_result *result, const char *path,
	lp_export_format format, const char *sample_type, char *error,
	size_t error_capacity);
bool lp_export_result_with_symbols(const lp_result *result,
	const char *path, lp_export_format format, const char *sample_type,
	const lp_export_symbols *symbols, char *error, size_t error_capacity);

#endif
