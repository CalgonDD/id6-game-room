package main

import (
	"strings"
	"testing"
)

func TestWindowReportKeepsSmallReportsWhole(t *testing.T) {
	got := windowReport("Saved to X", "=== a ===\nline")
	if got != "Saved to X\n\n=== a ===\nline" {
		t.Fatalf("small report changed: %q", got)
	}
}

func TestWindowReportCapsBigReportsAtALineBreak(t *testing.T) {
	report := strings.Repeat("0123456789 abcdefghij\n", 40000) // ~880 KB, like a full pull
	got := windowReport("Saved to X", report)
	if len(got) > diagnosticsWindowLimit+400 {
		t.Fatalf("window text is %d bytes", len(got))
	}
	if !strings.HasPrefix(got, "Saved to X\n\n0123456789") || !strings.Contains(got, "report.txt in the folder above") {
		t.Fatalf("missing summary or pointer: %q", got[len(got)-200:])
	}
	shown := got[len("Saved to X\n\n"):strings.Index(got, "\n\n... (")]
	if strings.Count(shown, "\n")+1 != strings.Count(shown+"\n", "\n") || !strings.HasSuffix(shown, "abcdefghij") {
		t.Fatalf("not cut at a line break: %q", shown[len(shown)-40:])
	}
}
