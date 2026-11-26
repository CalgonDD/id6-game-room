package main

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// isolate points os.UserConfigDir (and so the action log) at a temporary folder.
func isolateConfig(t *testing.T) string {
	dir := t.TempDir()
	if runtime.GOOS == "windows" {
		t.Setenv("AppData", dir)
	} else {
		t.Setenv("XDG_CONFIG_HOME", dir)
	}
	return dir
}

func TestActionLogRecordsActionsButNeverThePassword(t *testing.T) {
	isolateConfig(t)
	in := actionInput{Head: "169.254.199.119", Gateway: "169.254.78.76", Password: "hunter2-secret",
		Fingerprint: "SHA256:abc", Apps: []string{"GameRoom"}}
	logActionStart("install", in)
	logAction("SSH connect %s attempt %d/%d failed after %d ms: %v", in.Head, 1, 4, 20001, "ssh: handshake failed: i/o timeout")
	logActionEvent("install", inspection{Result: "Installed and verified GameRoom.\nRestart iDrive."})
	path, err := actionLogPath()
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	for _, want := range []string{"START install head=169.254.199.119", "apps=[GameRoom]", "trusted_key=true",
		"attempt 1/4 failed after 20001 ms", "EVENT install", `Installed and verified GameRoom.\nRestart iDrive.`} {
		if !strings.Contains(text, want) {
			t.Fatalf("log lacks %q:\n%s", want, text)
		}
	}
	if strings.Contains(text, "hunter2") {
		t.Fatalf("the password reached the log:\n%s", text)
	}
	if lines := strings.Count(text, "\n"); lines != 3 {
		t.Fatalf("multi-line results must stay one log line each, got %d lines:\n%s", lines, text)
	}
}

func TestActionLogRotatesAndIsCopiedIntoAPull(t *testing.T) {
	isolateConfig(t)
	path, err := actionLogPath()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(strings.Repeat("x", actionLogLimit+1)+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	logAction("after rotation")
	if _, err := os.Stat(path + ".previous"); err != nil {
		t.Fatalf("no .previous after passing the limit: %v", err)
	}
	if current, _ := os.ReadFile(path); !strings.Contains(string(current), "after rotation") || len(current) > 200 {
		t.Fatalf("current log not restarted: %d bytes", len(current))
	}
	long := strings.Repeat("y", actionLogLine*3)
	logAction("%s", long)
	pull := t.TempDir()
	if line := copyActionLog(pull); !strings.Contains(line, "installer-actions.log") {
		t.Fatalf("summary line: %q", line)
	}
	copied, err := os.ReadFile(filepath.Join(pull, "installer-actions.log"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(copied), "after rotation") || !strings.Contains(string(copied), "more bytes)") {
		t.Fatal("copy lacks the current log or the long entry was not cut")
	}
}
