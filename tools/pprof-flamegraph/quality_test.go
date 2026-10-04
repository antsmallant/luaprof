package main

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"github.com/google/pprof/profile"
)

func qualityProfile() *profile.Profile {
	parsed := &profile.Profile{
		SampleType: []*profile.ValueType{
			{Type: "samples", Unit: "count"},
			{Type: "cpu", Unit: "nanoseconds"},
		},
		DefaultSampleType: "cpu",
		Comments:          []string{"luaprof CPU sampling profile", "luaprof.metadata.version=1"},
	}
	for _, name := range cpuQualityFields {
		value := "0"
		switch name {
		case "sample_hz":
			value = "100"
		case "samples":
			value = "5"
		}
		parsed.Comments = append(parsed.Comments, "luaprof.cpu."+name+"="+value)
	}
	return parsed
}

func replaceQualityField(parsed *profile.Profile, name, value string) {
	prefix := "luaprof.cpu." + name + "="
	for i, comment := range parsed.Comments {
		if strings.HasPrefix(comment, prefix) {
			parsed.Comments[i] = prefix + value
		}
	}
}

func TestReadCPUQuality(t *testing.T) {
	parsed := qualityProfile()
	replaceQualityField(parsed, "overrun_events", "1")
	replaceQualityField(parsed, "overrun_ticks", "18446744073709551615")
	replaceQualityField(parsed, "profiler_overhead_events", "9223372036854775808")
	quality := readCPUQuality(parsed)
	if quality.unavailable != "" || len(quality.problems) != 0 ||
		quality.values["overrun_ticks"] != ^uint64(0) ||
		quality.values["profiler_overhead_events"] != uint64(1)<<63 {
		t.Fatalf("quality = %#v", quality)
	}
	text := strings.Join(quality.lines(), "\n")
	for _, expected := range []string{
		"samples=5", "sample_hz=100 Hz", "overrun_events=1",
		"overrun_ticks=18446744073709551615",
		"profiler_overhead_events=9223372036854775808", "Sampling affected",
	} {
		if !strings.Contains(text, expected) {
			t.Fatalf("missing %q in %s", expected, text)
		}
	}
	zero := strings.Join(readCPUQuality(qualityProfile()).lines(), "\n")
	if !strings.Contains(zero, "overrun_events=0; overrun_ticks=0") ||
		strings.Contains(zero, "Sampling affected") || strings.Contains(zero, "accurate") {
		t.Fatalf("unexpected zero-quality summary: %s", zero)
	}
}

func TestCPUQualityUnavailableAndUnknownKeys(t *testing.T) {
	for _, test := range []struct {
		name, reason string
		edit         func(*profile.Profile)
	}{
		{"legacy", "metadata not provided", func(p *profile.Profile) { p.Comments = nil }},
		{"no version", "metadata version missing", func(p *profile.Profile) {
			p.Comments = append(p.Comments[:1], p.Comments[2:]...)
		}},
		{"unknown version", "unsupported metadata version", func(p *profile.Profile) {
			p.Comments[1] = "luaprof.metadata.version=<future>&"
		}},
		{"duplicate version", "duplicate metadata versions", func(p *profile.Profile) {
			p.Comments = append(p.Comments, "luaprof.metadata.version=1")
		}},
	} {
		t.Run(test.name, func(t *testing.T) {
			parsed := qualityProfile()
			test.edit(parsed)
			quality := readCPUQuality(parsed)
			if quality.unavailable != test.reason || quality.values != nil {
				t.Fatalf("quality = %#v", quality)
			}
			if text := strings.Join(quality.lines(), " "); strings.Contains(text, "overrun_ticks=0") {
				t.Fatalf("unavailable metadata shown as zero: %s", text)
			}
		})
	}
	parsed := qualityProfile()
	parsed.Comments = append(parsed.Comments, "third-party comment & <text>", "luaprof.cpu.future=invalid")
	if quality := readCPUQuality(parsed); quality.unavailable != "" || len(quality.problems) != 0 {
		t.Fatalf("unknown comments changed quality: %#v", quality)
	}
	parsed.SampleType = []*profile.ValueType{{Type: "alloc_space", Unit: "bytes"}}
	if quality := readCPUQuality(parsed); quality != nil {
		t.Fatalf("memory profile got CPU quality: %#v", quality)
	}
}

func TestCPUQualityInvalidFields(t *testing.T) {
	for _, value := range []string{"", "-1", "+1", " 1", "1.5", "NaN", "18446744073709551616"} {
		t.Run(value, func(t *testing.T) {
			parsed := qualityProfile()
			replaceQualityField(parsed, "overrun_ticks", value)
			quality := readCPUQuality(parsed)
			if _, ok := quality.values["overrun_ticks"]; ok ||
				!reflect.DeepEqual(quality.problems, []string{"overrun_ticks"}) {
				t.Fatalf("invalid field accepted: %#v", quality)
			}
			if !strings.Contains(strings.Join(quality.lines(), " "), "overrun_ticks=unknown") {
				t.Fatal("invalid field was not shown as unknown")
			}
		})
	}
	for _, value := range []string{"0", "10001"} {
		parsed := qualityProfile()
		replaceQualityField(parsed, "sample_hz", value)
		if quality := readCPUQuality(parsed); len(quality.problems) != 1 {
			t.Fatalf("invalid frequency accepted: %#v", quality)
		}
	}
	parsed := qualityProfile()
	parsed.Comments = append(parsed.Comments, "luaprof.cpu.overrun_ticks=7")
	quality := readCPUQuality(parsed)
	if _, ok := quality.values["overrun_ticks"]; ok {
		t.Fatal("duplicate field accepted")
	}
	parsed = qualityProfile()
	for i, comment := range parsed.Comments {
		if strings.HasPrefix(comment, "luaprof.cpu.overrun_ticks=") {
			parsed.Comments = append(parsed.Comments[:i], parsed.Comments[i+1:]...)
			break
		}
	}
	quality = readCPUQuality(parsed)
	if !reflect.DeepEqual(quality.problems, []string{"overrun_ticks"}) {
		t.Fatalf("missing field not identified: %#v", quality)
	}
}

func TestQualitySVGLayoutAndWeights(t *testing.T) {
	parsed := qualityProfile()
	for _, name := range cpuQualityFields[2:] {
		replaceQualityField(parsed, name, "18446744073709551615")
	}
	root := newTreeNode("")
	frame := newTreeNode("work")
	frame.value = 50
	root.value, root.children["work"] = 50, frame
	for _, interactive := range []bool{false, true} {
		for _, width := range []int{320, 640, 1200} {
			t.Run(fmt.Sprintf("interactive=%v/width=%d", interactive, width), func(t *testing.T) {
				var output bytes.Buffer
				quality := readCPUQuality(parsed)
				if err := renderSVG(&output, root, 50, metric{name: "cpu", unit: "nanoseconds"}, renderOptions{
					width: width, interactive: interactive, quality: quality,
				}); err != nil {
					t.Fatal(err)
				}
				assertValidXML(t, output.Bytes())
				decoder := xml.NewDecoder(bytes.NewReader(output.Bytes()))
				lastQualityY, frameY := 0, 0
				var texts []string
				for {
					token, err := decoder.Token()
					if err == io.EOF {
						break
					}
					if err != nil {
						t.Fatal(err)
					}
					element, ok := token.(xml.StartElement)
					if !ok {
						continue
					}
					attrs := make(map[string]string)
					for _, attr := range element.Attr {
						attrs[attr.Name.Local] = attr.Value
					}
					if element.Name.Local == "text" && attrs["class"] == "quality" {
						lastQualityY, _ = strconv.Atoi(attrs["y"])
						var text string
						if err := decoder.DecodeElement(&text, &element); err != nil {
							t.Fatal(err)
						}
						if len(text) > (width-20)/int(approximateGlyph) {
							t.Fatalf("quality text exceeds canvas: %q", text)
						}
						texts = append(texts, text)
					}
					if element.Name.Local == "rect" && attrs["rx"] == "1" {
						y, _ := strconv.ParseFloat(attrs["y"], 64)
						frameY = int(y)
						if attrs["width"] != fmt.Sprintf("%d.000", width) {
							t.Fatalf("frame width changed: %s", attrs["width"])
						}
					}
				}
				if lastQualityY == 0 || frameY <= lastQualityY {
					t.Fatalf("quality overlaps frame: qualityY=%d frameY=%d", lastQualityY, frameY)
				}
				// Remove wrapping spaces to recover even long uint64 field tokens.
				text := strings.ReplaceAll(strings.Join(texts, ""), " ", "")
				for _, name := range cpuQualityFields[2:] {
					if !strings.Contains(text, name+"=18446744073709551615") {
						t.Fatalf("SVG lost field %s: %s", name, text)
					}
				}
			})
		}
	}
}

func TestRunSavedQualityProfile(t *testing.T) {
	parsed := qualityProfile()
	replaceQualityField(parsed, "overrun_events", "1")
	replaceQualityField(parsed, "overrun_ticks", "3")
	function := &profile.Function{ID: 1, Name: "work"}
	location := &profile.Location{ID: 1, Line: []profile.Line{{Function: function}}}
	parsed.Function, parsed.Location = []*profile.Function{function}, []*profile.Location{location}
	parsed.Sample = []*profile.Sample{{Location: []*profile.Location{location}, Value: []int64{5, 50000000}}}
	path := filepath.Join(t.TempDir(), "cpu.pb.gz")
	file, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := parsed.Write(file); err != nil {
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	for _, mode := range [][]string{nil, {"--interactive"}, {"--sample=samples"}} {
		var output, diagnostics bytes.Buffer
		if err := run(append(mode, path), &output, &diagnostics); err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(output.String(), "overrun_events=1; overrun_ticks=3") {
			t.Fatalf("saved profile quality not displayed: %s", output.String())
		}
		assertValidXML(t, output.Bytes())
	}
}

// Integration sets this directory after running the C exporter in another
// process. Use Google's parser to verify the actual wire comments and weights.
func TestExportedCPUQuality(t *testing.T) {
	directory := os.Getenv("LP_EXPORTED_PROFILE_DIR")
	if directory == "" {
		t.Skip("C exporter fixtures not requested")
	}
	for _, name := range []string{"cpu", "zero", "empty", "large"} {
		t.Run(name, func(t *testing.T) {
			data, err := os.ReadFile(filepath.Join(directory, "luaprof-pprof-"+name+".pb.gz"))
			if err != nil {
				t.Fatal(err)
			}
			parsed, err := profile.ParseData(data)
			if err != nil {
				t.Fatal(err)
			}
			if err := parsed.CheckValid(); err != nil {
				t.Fatal(err)
			}
			quality := readCPUQuality(parsed)
			want := map[string]uint64{
				"sample_hz": 100, "samples": 5, "overrun_events": 1, "overrun_ticks": 3,
				"dropped_events": 0, "unstable_events": 0, "profiler_overhead_events": 0,
				"stale_events": 0, "timer_failures": 0, "stack_truncations": 0,
				"aggregate_overflows": 0, "symbol_overflows": 0, "scheduler_workers": 0,
			}
			if name == "empty" {
				want["samples"], want["dropped_events"] = 0, 1
			}
			if name == "zero" {
				want["overrun_events"], want["overrun_ticks"] = 0, 0
			}
			if name == "large" {
				want["overrun_ticks"], want["profiler_overhead_events"] = ^uint64(0), uint64(1)<<63
				for field, value := range map[string]uint64{
					"dropped_events": 7, "unstable_events": 8, "stale_events": 9,
					"timer_failures": 10, "stack_truncations": 11, "aggregate_overflows": 12,
					"symbol_overflows": 13, "scheduler_workers": 2,
				} {
					want[field] = value
				}
			}
			if quality.unavailable != "" || len(quality.problems) != 0 || !reflect.DeepEqual(quality.values, want) {
				t.Fatalf("exported quality = %#v, want %#v", quality, want)
			}
			var samples, cpu int64
			for _, sample := range parsed.Sample {
				samples += sample.Value[0]
				cpu += sample.Value[1]
			}
			if samples != int64(want["samples"]) || cpu != samples*10000000 {
				t.Fatalf("overrun changed sample weights: samples=%d cpu=%d", samples, cpu)
			}
			if name == "empty" {
				if _, _, err := buildTree(parsed, metric{name: "cpu", index: 1}); err == nil {
					t.Fatal("empty profile unexpectedly produced a flame graph")
				}
			}
		})
	}
}

func TestExampleCPUQuality(t *testing.T) {
	directory := os.Getenv("LP_EXAMPLE_PROFILE_DIR")
	if directory == "" {
		t.Skip("thread/Skynet example profiles not requested")
	}
	for _, name := range []string{"thread-vm", "skynet"} {
		t.Run(name, func(t *testing.T) {
			data, err := os.ReadFile(filepath.Join(directory, name+"-cpu.pb.gz"))
			if err != nil {
				t.Fatal(err)
			}
			parsed, err := profile.ParseData(data)
			if err != nil {
				t.Fatal(err)
			}
			quality := readCPUQuality(parsed)
			if quality.unavailable != "" || len(quality.problems) != 0 || quality.values["sample_hz"] != 100 {
				t.Fatalf("example metadata missing or invalid: %#v", quality)
			}
			if name == "skynet" && quality.values["scheduler_workers"] == 0 {
				t.Fatal("Skynet example did not use scheduler backend")
			}
			var samples, cpu int64
			for _, sample := range parsed.Sample {
				samples += sample.Value[0]
				cpu += sample.Value[1]
			}
			if uint64(samples) != quality.values["samples"] || cpu != samples*parsed.Period {
				t.Fatalf("metadata and exported weights disagree: samples=%d cpu=%d", samples, cpu)
			}
		})
	}
}
