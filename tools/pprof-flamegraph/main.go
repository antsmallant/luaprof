// Command pprof-flamegraph renders a saved pprof profile as a static SVG flame graph.
package main

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"html"
	"io"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/google/pprof/profile"
)

const (
	defaultWidth            = 1200
	frameHeight             = 18
	staticHeaderHeight      = 54
	interactiveHeaderHeight = 80
	footerHeight            = 22
	minimumWidth            = 0.25
	approximateGlyph        = 7.0
)

type metric struct {
	name  string
	unit  string
	index int
}

type treeNode struct {
	name     string
	value    int64
	children map[string]*treeNode
}

type renderOptions struct {
	title       string
	width       int
	palette     string
	interactive bool
}

func main() {
	if err := run(os.Args[1:], os.Stdout, os.Stderr); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return
		}
		fmt.Fprintf(os.Stderr, "pprof-flamegraph: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string, stdout, stderr io.Writer) error {
	flags := flag.NewFlagSet("pprof-flamegraph", flag.ContinueOnError)
	flags.SetOutput(stderr)
	outputPath := flags.String("output", "", "write SVG to this file instead of stdout")
	sampleName := flags.String("sample", "", "pprof sample type to render (default: profile default)")
	title := flags.String("title", "", "SVG title")
	width := flags.Int("width", defaultWidth, "SVG width in pixels")
	palette := flags.String("palette", "auto", "frame palette: auto, hot, or memory")
	interactive := flags.Bool("interactive", false,
		"embed JavaScript for zoom and search (not supported by GitHub previews)")
	flags.Usage = func() {
		fmt.Fprintln(stderr, "Usage: pprof-flamegraph [options] profile.pb.gz")
		flags.PrintDefaults()
	}
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 1 {
		flags.Usage()
		return fmt.Errorf("expected exactly one pprof profile")
	}
	if *width < 320 {
		return fmt.Errorf("width must be at least 320")
	}
	if *palette != "auto" && *palette != "hot" && *palette != "memory" {
		return fmt.Errorf("palette must be auto, hot, or memory")
	}

	input, err := os.Open(flags.Arg(0))
	if err != nil {
		return fmt.Errorf("open profile: %w", err)
	}
	defer input.Close()
	parsed, err := profile.Parse(input)
	if err != nil {
		return fmt.Errorf("parse profile: %w", err)
	}
	selected, err := selectMetric(parsed, *sampleName)
	if err != nil {
		return err
	}
	root, total, err := buildTree(parsed, selected)
	if err != nil {
		return err
	}

	var output io.Writer = stdout
	var file *os.File
	if *outputPath != "" {
		file, err = os.Create(*outputPath)
		if err != nil {
			return fmt.Errorf("create output: %w", err)
		}
		defer file.Close()
		output = file
	}
	return renderSVG(output, root, total, selected, renderOptions{
		title:       *title,
		width:       *width,
		palette:     *palette,
		interactive: *interactive,
	})
}

func selectMetric(parsed *profile.Profile, requested string) (metric, error) {
	if requested == "" {
		requested = parsed.DefaultSampleType
	}
	if requested == "" && len(parsed.SampleType) == 1 {
		requested = parsed.SampleType[0].Type
	}
	for index, sampleType := range parsed.SampleType {
		if sampleType.Type == requested {
			return metric{name: sampleType.Type, unit: sampleType.Unit, index: index}, nil
		}
	}
	available := make([]string, 0, len(parsed.SampleType))
	for _, sampleType := range parsed.SampleType {
		available = append(available, sampleType.Type)
	}
	sort.Strings(available)
	if requested == "" {
		return metric{}, fmt.Errorf("profile has no default sample type; choose one of: %s",
			strings.Join(available, ", "))
	}
	return metric{}, fmt.Errorf("unknown sample type %q; choose one of: %s", requested,
		strings.Join(available, ", "))
}

func buildTree(parsed *profile.Profile, selected metric) (*treeNode, int64, error) {
	root := newTreeNode("")
	for _, sample := range parsed.Sample {
		if selected.index >= len(sample.Value) {
			return nil, 0, fmt.Errorf("sample type %q is missing from a profile sample", selected.name)
		}
		value := sample.Value[selected.index]
		if value < 0 {
			return nil, 0, fmt.Errorf("sample type %q contains negative values; static flame graphs require non-negative values", selected.name)
		}
		if value == 0 {
			continue
		}
		stack := stackNames(sample)
		if len(stack) == 0 {
			stack = []string{"[unknown]"}
		}
		root.value += value
		node := root
		for _, name := range stack {
			child := node.children[name]
			if child == nil {
				child = newTreeNode(name)
				node.children[name] = child
			}
			child.value += value
			node = child
		}
	}
	if root.value == 0 {
		return nil, 0, fmt.Errorf("sample type %q has no positive samples", selected.name)
	}
	return root, root.value, nil
}

func newTreeNode(name string) *treeNode {
	return &treeNode{name: name, children: make(map[string]*treeNode)}
}

func stackNames(sample *profile.Sample) []string {
	var names []string
	for locationIndex := len(sample.Location) - 1; locationIndex >= 0; locationIndex-- {
		location := sample.Location[locationIndex]
		if location == nil {
			names = append(names, "[unknown]")
			continue
		}
		if len(location.Line) == 0 {
			names = append(names, locationName(location))
			continue
		}
		for lineIndex := len(location.Line) - 1; lineIndex >= 0; lineIndex-- {
			line := location.Line[lineIndex]
			if line.Function != nil && line.Function.Name != "" {
				names = append(names, line.Function.Name)
			} else {
				names = append(names, locationName(location))
			}
		}
	}
	return names
}

func locationName(location *profile.Location) string {
	if location.Address != 0 {
		return fmt.Sprintf("0x%x", location.Address)
	}
	return "[unknown]"
}

func renderSVG(output io.Writer, root *treeNode, total int64, selected metric,
	options renderOptions) error {
	if options.width == 0 {
		options.width = defaultWidth
	}
	if options.title == "" {
		options.title = selected.name + " flame graph"
	}
	if options.palette == "auto" {
		if selected.unit == "bytes" || strings.Contains(selected.name, "space") {
			options.palette = "memory"
		} else {
			options.palette = "hot"
		}
	}
	chartTop := staticHeaderHeight
	mode := "Static"
	if options.interactive {
		chartTop = interactiveHeaderHeight
		mode = "Interactive"
	}
	depth := maximumDepth(root)
	height := chartTop + footerHeight + depth*frameHeight
	writer := bufio.NewWriter(output)

	fmt.Fprintf(writer, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
	fmt.Fprintf(writer, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\" overflow=\"hidden\" role=\"img\" aria-labelledby=\"title description\">\n", options.width, height, options.width, height)
	fmt.Fprintf(writer, "<title id=\"title\">%s</title>\n", escape(options.title))
	fmt.Fprintf(writer, "<desc id=\"description\">%s flame graph for pprof sample type %s, total %s.</desc>\n", mode, escape(selected.name), escape(formatValue(total, selected.unit)))
	fmt.Fprintln(writer, "<style>text{font-family:Verdana,sans-serif;fill:#171717}.heading{font-size:16px}.subtitle{font-size:11px;fill:#555}.control{font-size:11px;fill:#0645ad;cursor:pointer;text-decoration:underline}.frame text{font-size:11px;pointer-events:none}.frame rect{stroke:#fff;stroke-width:.5}.frame:hover rect{stroke:#111;stroke-width:1}.frame.matched rect{stroke:#c000c0;stroke-width:2}</style>")
	fmt.Fprintln(writer, "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/>")
	fmt.Fprintf(writer, "<text class=\"heading\" x=\"10\" y=\"23\">%s</text>\n", escape(options.title))
	fmt.Fprintf(writer, "<text class=\"subtitle\" x=\"10\" y=\"42\">sample: %s; total: %s; widths are inclusive sample values</text>\n", escape(selected.name), escape(formatValue(total, selected.unit)))
	if options.interactive {
		fmt.Fprintln(writer, "<text id=\"search\" class=\"control\" x=\"10\" y=\"65\">Search (Ctrl-F)</text>")
		fmt.Fprintln(writer, "<text id=\"reset\" class=\"control\" x=\"112\" y=\"65\">Reset zoom</text>")
		fmt.Fprintf(writer, "<text id=\"matched\" class=\"subtitle\" x=\"%d\" y=\"65\" text-anchor=\"end\"></text>\n", options.width-10)
	}

	fmt.Fprintln(writer, "<g id=\"frames\">")
	drawChildren(writer, root, total, 0, float64(chartTop), float64(options.width), depth,
		selected, options.palette)
	fmt.Fprintln(writer, "</g>")
	if options.interactive {
		fmt.Fprintln(writer, interactiveScript)
	}
	fmt.Fprintln(writer, "</svg>")
	if err := writer.Flush(); err != nil {
		return fmt.Errorf("write SVG: %w", err)
	}
	return nil
}

func drawChildren(writer *bufio.Writer, parent *treeNode, total int64, startX,
	chartY, chartWidth float64, maximumDepth int, selected metric, palette string) {
	children := sortedChildren(parent)
	x := startX
	for _, child := range children {
		width := chartWidth * float64(child.value) / float64(parent.value)
		drawNode(writer, child, total, x, chartY, width, 1, maximumDepth, selected, palette)
		x += width
	}
}

func drawNode(writer *bufio.Writer, node *treeNode, total int64, x, chartY, width float64,
	depth, maximumDepth int, selected metric, palette string) {
	if width < minimumWidth {
		return
	}
	y := chartY + float64(maximumDepth-depth)*frameHeight
	percent := 100 * float64(node.value) / float64(total)
	label := clipLabel(node.name, width)
	fmt.Fprintf(writer, "<g class=\"frame\" data-name=\"%s\">\n", escape(node.name))
	fmt.Fprintf(writer, "<title>%s (%s, %.2f%%)</title>\n", escape(node.name),
		escape(formatValue(node.value, selected.unit)), percent)
	fmt.Fprintf(writer, "<rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%d\" rx=\"1\" fill=\"%s\"/>\n",
		x, y, width, frameHeight-1, frameColor(node.name, palette))
	if label != "" {
		fmt.Fprintf(writer, "<text x=\"%.3f\" y=\"%.3f\">%s</text>\n", x+3, y+12, escape(label))
	}
	fmt.Fprintln(writer, "</g>")

	children := sortedChildren(node)
	childX := x
	for _, child := range children {
		childWidth := width * float64(child.value) / float64(node.value)
		drawNode(writer, child, total, childX, chartY, childWidth, depth+1,
			maximumDepth, selected, palette)
		childX += childWidth
	}
}

func sortedChildren(node *treeNode) []*treeNode {
	children := make([]*treeNode, 0, len(node.children))
	for _, child := range node.children {
		children = append(children, child)
	}
	sort.Slice(children, func(left, right int) bool {
		if children[left].value != children[right].value {
			return children[left].value > children[right].value
		}
		return children[left].name < children[right].name
	})
	return children
}

func maximumDepth(node *treeNode) int {
	maximum := 0
	for _, child := range node.children {
		depth := 1 + maximumDepth(child)
		if depth > maximum {
			maximum = depth
		}
	}
	return maximum
}

func clipLabel(label string, width float64) string {
	limit := int(math.Floor((width - 6) / approximateGlyph))
	if limit <= 0 {
		return ""
	}
	if utf8.RuneCountInString(label) <= limit {
		return label
	}
	if limit <= 3 {
		return ""
	}
	runes := []rune(label)
	return string(runes[:limit-3]) + "..."
}

func frameColor(name, palette string) string {
	var hash uint32 = 2166136261
	for _, value := range []byte(name) {
		hash ^= uint32(value)
		hash *= 16777619
	}
	var base, spread uint32
	switch palette {
	case "memory":
		base, spread = 125, 70
	default:
		base, spread = 12, 52
	}
	hue := base + hash%spread
	lightness := 62 + (hash/97)%15
	return "hsl(" + strconv.FormatUint(uint64(hue), 10) + ",72%," +
		strconv.FormatUint(uint64(lightness), 10) + "%)"
}

func formatValue(value int64, unit string) string {
	if unit == "bytes" {
		return formatScaled(float64(value), []string{"B", "KiB", "MiB", "GiB", "TiB"}, 1024)
	}
	if unit == "nanoseconds" {
		return formatScaled(float64(value), []string{"ns", "us", "ms", "s"}, 1000)
	}
	if unit == "count" || unit == "" {
		return strconv.FormatInt(value, 10)
	}
	return strconv.FormatInt(value, 10) + " " + unit
}

func formatScaled(value float64, units []string, base float64) string {
	unitIndex := 0
	for math.Abs(value) >= base && unitIndex+1 < len(units) {
		value /= base
		unitIndex++
	}
	if unitIndex == 0 {
		return strconv.FormatInt(int64(value), 10) + " " + units[unitIndex]
	}
	return strconv.FormatFloat(value, 'f', 2, 64) + " " + units[unitIndex]
}

func escape(value string) string {
	return html.EscapeString(value)
}

const interactiveScript = `<script type="application/ecmascript"><![CDATA[
(function () {
  "use strict";
  const svg = document.querySelector("svg");
  const frames = document.getElementById("frames");
  const matched = document.getElementById("matched");
  const originalTransform = frames.getAttribute("transform");

  function findFrame(target) {
    while (target && target !== svg) {
      if (target.classList && target.classList.contains("frame")) return target;
      target = target.parentNode;
    }
    return null;
  }

  function resetZoom() {
    if (originalTransform === null) frames.removeAttribute("transform");
    else frames.setAttribute("transform", originalTransform);
  }

  function zoom(frame) {
    const rect = frame.querySelector("rect");
    if (!rect) return;
    const x = Number(rect.getAttribute("x"));
    const width = Number(rect.getAttribute("width"));
    if (!(width > 0)) return;
    const scale = svg.viewBox.baseVal.width / width;
    frames.setAttribute("transform", "matrix(" + scale + " 0 0 1 " + (-x * scale) + " 0)");
  }

  function clearSearch() {
    frames.querySelectorAll(".frame.matched").forEach(function (frame) {
      frame.classList.remove("matched");
    });
    matched.textContent = "";
  }

  function search() {
    const query = window.prompt("Search function name (regular expression):", "");
    if (query === null) return;
    clearSearch();
    if (query === "") return;
    let expression;
    try {
      expression = new RegExp(query, "i");
    } catch (error) {
      matched.textContent = "Invalid regular expression";
      return;
    }
    let count = 0;
    frames.querySelectorAll(".frame").forEach(function (frame) {
      if (expression.test(frame.getAttribute("data-name") || "")) {
        frame.classList.add("matched");
        count++;
      }
    });
    matched.textContent = count + (count === 1 ? " match" : " matches");
  }

  svg.addEventListener("click", function (event) {
    if (event.target.id === "search") {
      search();
      return;
    }
    if (event.target.id === "reset") {
      resetZoom();
      return;
    }
    const frame = findFrame(event.target);
    if (frame) zoom(frame);
  });

  window.addEventListener("keydown", function (event) {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "f") {
      event.preventDefault();
      search();
    } else if (event.key === "Escape") {
      clearSearch();
      resetZoom();
    }
  });
}());
]]></script>`
