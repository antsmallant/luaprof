package main

import (
	"bytes"
	"encoding/xml"
	"io"
	"strings"
	"testing"

	"github.com/google/pprof/profile"
)

func TestSelectMetric(t *testing.T) {
	parsed := &profile.Profile{
		SampleType: []*profile.ValueType{
			{Type: "samples", Unit: "count"},
			{Type: "cpu", Unit: "nanoseconds"},
		},
		DefaultSampleType: "cpu",
	}
	selected, err := selectMetric(parsed, "")
	if err != nil {
		t.Fatal(err)
	}
	if selected.name != "cpu" || selected.unit != "nanoseconds" || selected.index != 1 {
		t.Fatalf("selected = %#v", selected)
	}
	if _, err := selectMetric(parsed, "missing"); err == nil ||
		!strings.Contains(err.Error(), "cpu, samples") {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestBuildTreeUsesRootToLeafOrder(t *testing.T) {
	rootFunction := &profile.Function{Name: "main"}
	hotFunction := &profile.Function{Name: "hot"}
	parsed := &profile.Profile{
		Sample: []*profile.Sample{{
			Location: []*profile.Location{
				{Line: []profile.Line{{Function: hotFunction}}},
				{Line: []profile.Line{{Function: rootFunction}}},
			},
			Value: []int64{7},
		}},
	}
	tree, total, err := buildTree(parsed, metric{name: "cpu", unit: "nanoseconds", index: 0})
	if err != nil {
		t.Fatal(err)
	}
	if total != 7 || tree.value != 7 {
		t.Fatalf("total = %d, root = %d", total, tree.value)
	}
	main := tree.children["main"]
	if main == nil || main.value != 7 || main.children["hot"] == nil {
		t.Fatalf("unexpected tree: %#v", tree.children)
	}
}

func TestStackNamesExpandsInlineFramesFromCallerToLeaf(t *testing.T) {
	caller := &profile.Function{Name: "caller"}
	inlined := &profile.Function{Name: "inlined"}
	names := stackNames(&profile.Sample{Location: []*profile.Location{{
		Line: []profile.Line{
			{Function: inlined},
			{Function: caller},
		},
	}}})
	if got, want := strings.Join(names, ";"), "caller;inlined"; got != want {
		t.Fatalf("stack = %q, want %q", got, want)
	}
}

func TestRenderSVGIsValidAndEscaped(t *testing.T) {
	root := newTreeNode("")
	main := newTreeNode("main & entry")
	main.value = 10
	hot := newTreeNode("hot <work>")
	hot.value = 10
	main.children[hot.name] = hot
	root.children[main.name] = main
	root.value = 10
	var output bytes.Buffer
	err := renderSVG(&output, root, 10, metric{name: "inuse_space", unit: "bytes"}, renderOptions{
		title: "Heap & retained",
		width: 640,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(output.String(), "hot &lt;work&gt;") ||
		!strings.Contains(output.String(), "Heap &amp; retained") {
		t.Fatalf("SVG does not escape frame text:\n%s", output.String())
	}
	decoder := xml.NewDecoder(bytes.NewReader(output.Bytes()))
	for {
		if _, err := decoder.Token(); err != nil {
			if err == io.EOF {
				break
			}
			t.Fatalf("invalid SVG: %v", err)
		}
	}
}

func TestBuildTreeRejectsNegativeValues(t *testing.T) {
	parsed := &profile.Profile{Sample: []*profile.Sample{{Value: []int64{-1}}}}
	if _, _, err := buildTree(parsed, metric{name: "cpu", index: 0}); err == nil {
		t.Fatal("buildTree accepted a negative value")
	}
}
