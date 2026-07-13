#ifndef LUAPROF_PPROF_EXPORTER_H
#define LUAPROF_PPROF_EXPORTER_H

#include "luaprof/runtime.h"

typedef enum lp_export_format {
	LP_EXPORT_PPROF = 0,
	LP_EXPORT_FOLDED = 1
} lp_export_format;

bool lp_export_result(const lp_result_meta *result, const char *path,
	lp_export_format format, const char *sample_type, char *error,
	size_t error_capacity);

#endif
