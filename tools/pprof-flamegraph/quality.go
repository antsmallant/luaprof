package main

import (
	"strconv"
	"strings"

	"github.com/google/pprof/profile"
)

// These profile-level counters are independent of the selected sample metric.
var cpuQualityFields = []string{
	"sample_hz", "samples", "overrun_events", "overrun_ticks",
	"dropped_events", "unstable_events", "profiler_overhead_events",
	"stale_events", "timer_failures", "stack_truncations",
	"aggregate_overflows", "symbol_overflows", "scheduler_workers",
}

type cpuQuality struct {
	values      map[string]uint64
	unavailable string
	problems    []string
}

func readCPUQuality(parsed *profile.Profile) *cpuQuality {
	isCPU := false
	for _, sampleType := range parsed.SampleType {
		isCPU = isCPU || (sampleType.Type == "cpu" && sampleType.Unit == "nanoseconds")
	}
	if !isCPU {
		return nil
	}
	quality := &cpuQuality{values: make(map[string]uint64)}
	known := make(map[string]bool)
	for _, name := range cpuQualityFields {
		known[name] = true
	}
	seen := make(map[string]bool)
	invalid := make(map[string]bool)
	version, versionCount := "", 0
	for _, comment := range parsed.Comments {
		key, value, hasValue := strings.Cut(comment, "=")
		if key == "luaprof.metadata.version" {
			version, versionCount = value, versionCount+1
			continue
		}
		if !strings.HasPrefix(key, "luaprof.cpu.") {
			continue
		}
		name := strings.TrimPrefix(key, "luaprof.cpu.")
		if !known[name] {
			continue // Allow future versions to add unrelated keys.
		}
		if seen[name] {
			invalid[name] = true // Merged profiles may contain conflicting counters.
		}
		seen[name] = true
		decimal := hasValue && value != ""
		for _, digit := range value {
			decimal = decimal && digit >= '0' && digit <= '9'
		}
		number, err := strconv.ParseUint(value, 10, 64)
		if !decimal || err != nil || (name == "sample_hz" && (number == 0 || number > 10000)) {
			invalid[name] = true
		}
		quality.values[name] = number
	}
	switch {
	case versionCount == 0:
		quality.unavailable = "metadata not provided"
		if len(seen) != 0 {
			quality.unavailable = "metadata version missing"
		}
	case versionCount != 1:
		quality.unavailable = "duplicate metadata versions"
	case version != "1":
		quality.unavailable = "unsupported metadata version"
	}
	if quality.unavailable != "" {
		quality.values = nil
		return quality
	}
	for _, name := range cpuQualityFields {
		if !seen[name] || invalid[name] {
			delete(quality.values, name)
			quality.problems = append(quality.problems, name)
		}
	}
	return quality
}

func (quality *cpuQuality) lines() []string {
	if quality == nil {
		return nil
	}
	if quality.unavailable != "" {
		return []string{"CPU quality: unavailable (" + quality.unavailable + ")"}
	}
	value := func(name string) string {
		if number, ok := quality.values[name]; ok {
			return name + "=" + strconv.FormatUint(number, 10)
		}
		return name + "=unknown"
	}
	lines := []string{
		"CPU quality: " + value("samples") + "; " + value("sample_hz") + " Hz",
		value("overrun_events") + "; " + value("overrun_ticks"),
	}
	var losses []string
	for _, name := range cpuQualityFields[4 : len(cpuQualityFields)-1] {
		if quality.values[name] != 0 {
			losses = append(losses, value(name))
		}
	}
	if len(losses) != 0 {
		lines = append(lines, strings.Join(losses, "; "))
	}
	if quality.values["overrun_ticks"] != 0 || quality.values["overrun_events"] != 0 || len(losses) != 0 {
		lines = append(lines, "Sampling affected; missing contexts are not added to frame weights.")
	}
	if quality.values["scheduler_workers"] != 0 {
		lines = append(lines, value("scheduler_workers"))
	}
	if len(quality.problems) != 0 {
		lines = append(lines, "Quality metadata missing/invalid: "+strings.Join(quality.problems, ", "))
	}
	return lines
}

// The metadata uses ASCII keys and numbers. Wrap even a single long field at
// narrow widths so uint64 values and loss counters never disappear off-canvas.
func wrapQualityLines(lines []string, width int) []string {
	columns := (width - 20) / int(approximateGlyph)
	if columns < 1 {
		columns = 1
	}
	var wrapped []string
	for _, line := range lines {
		current := ""
		for _, word := range strings.Fields(line) {
			if current != "" && len(current)+1+len(word) > columns {
				wrapped = append(wrapped, current)
				current = ""
			}
			for len(word) > columns {
				wrapped = append(wrapped, word[:columns])
				word = word[columns:]
			}
			if current != "" {
				current += " "
			}
			current += word
		}
		if current != "" {
			wrapped = append(wrapped, current)
		}
	}
	return wrapped
}
