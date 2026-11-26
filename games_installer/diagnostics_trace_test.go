package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDiagnosticsCapturesPreviousAndCurrentTraceWithoutHead(t *testing.T) {
	dir := t.TempDir()
	for name, text := range map[string]string{
		"HMI-apps-trace.previous.txt": "previous-model-begin\n",
		"HMI-apps-trace.txt":          "current-model-end\n",
	} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(text), 0600); err != nil {
			t.Fatal(err)
		}
	}
	var traceSection diagSection
	for _, section := range diagSections {
		if section.key == "trace" {
			traceSection = section
		}
	}
	script := diagnosticsScript(traceSection)
	out, err := (shellFixture{dir: dir}).run(script, nil, 1048576)
	if err != nil {
		t.Fatalf("diagnostic trace script: %v\n%s", err, out)
	}
	text := string(out)
	if !strings.Contains(text, "previous-model-begin") || !strings.Contains(text, "current-model-end") {
		t.Fatalf("missing rotated/current trace content: %s", text)
	}
}

// A watchdog dump laid out like the car's: one row per thread of every process, HmiMain3D's rows
// early and more than 400 lines from the end - exactly what a plain `tail -400` used to cut off.
func fakeWatchdogDump() string {
	var b strings.Builder
	b.WriteString("WD ERROR header line\n")
	for tid := 1; tid <= 30; tid++ {
		fmt.Fprintf(&b, "  299050,  258100,opt/hmi/bin/zipped/HmiMain3D ,  %d,bmw::Looper , 11r,CONDVAR ,(0x9c384a0) , 50.000,HMI\n", tid)
	}
	for i := 0; i < 500; i++ {
		fmt.Fprintf(&b, "  299050,  262215,opt/mm/bin/mme-becker ,  %d,worker , 10r,RECEIVE ,35 , 0.000,MME\n", i)
	}
	b.WriteString("LAST LINE\n")
	return b.String()
}

// shellFixture only maps the plugin root; these tests also map /var/dump onto a temp dir.
type dumpFixture struct {
	shellFixture
	dump string
}

func (f dumpFixture) run(command string, input []byte, limit int) ([]byte, error) {
	out, err := f.shellFixture.run(strings.ReplaceAll(command, "/var/dump", testShellPath(f.dump)), input, limit)
	return bytes.ReplaceAll(out, []byte(testShellPath(f.dump)), []byte("/var/dump")), err
}

func TestWatchdogSectionKeepsEveryHmiMain3DRow(t *testing.T) {
	dump := t.TempDir()
	if err := os.WriteFile(filepath.Join(dump, "wd_err_omap_1.log"), []byte(fakeWatchdogDump()), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dump, "wd_err_omap_1_Systemload.log"), []byte("CPU consumed by: HmiMain3D TID 22 bmw::Looper 51%\n"), 0600); err != nil {
		t.Fatal(err)
	}
	var watchdog diagSection
	for _, section := range diagSections {
		if section.key == "watchdog" {
			watchdog = section
		}
	}
	out, err := (dumpFixture{shellFixture{dir: t.TempDir()}, dump}).run(diagnosticsScript(watchdog), nil, 1<<20)
	if err != nil {
		t.Fatalf("watchdog script: %v\n%s", err, out)
	}
	text := string(out)
	for tid := 1; tid <= 30; tid++ {
		if !strings.Contains(text, fmt.Sprintf("HmiMain3D ,  %d,bmw::Looper", tid)) {
			t.Fatalf("HmiMain3D thread %d missing from the report:\n%s", tid, text[:min(len(text), 3000)])
		}
	}
	for _, want := range []string{"WD ERROR header line", "CPU consumed by: HmiMain3D TID 22"} {
		if !strings.Contains(text, want) {
			t.Fatalf("report lost %q", want)
		}
	}
	// The rest of each big log is saved whole by fetchWatchdogLogs; repeating it here is what pushed
	// this section past its 45 s budget on the car.
	if strings.Contains(text, "LAST LINE") || strings.Contains(text, "mme-becker") {
		t.Fatalf("report still repeats the bulk of the watchdog log:\n%s", text[:min(len(text), 3000)])
	}
}

// fakeConn is one fake SSH connection: it times out (not an exit status) on the commands listed.
type fakeConn struct {
	id      int
	timeout map[string]bool
	closed  *[]int
}

func (c fakeConn) run(cmd string, _ []byte, _ int) ([]byte, error) {
	if c.timeout[cmd] {
		return nil, fmt.Errorf("read tcp4: i/o timeout")
	}
	return []byte(fmt.Sprintf("conn%d:%s", c.id, cmd)), nil
}
func (c fakeConn) runLong(cmd string, input []byte, limit int) ([]byte, error) {
	return c.run(cmd, input, limit)
}
func (c fakeConn) close() { *c.closed = append(*c.closed, c.id) }

func TestCarLinkReconnectsAfterATimedOutStep(t *testing.T) {
	var closed []int
	dials := 0
	link := &carLink{
		current: fakeConn{id: 0, timeout: map[string]bool{"slow": true}, closed: &closed},
		dial: func() (linkConn, bool) {
			dials++
			return fakeConn{id: dials, closed: &closed}, true
		},
	}
	if _, err := link.runLong("slow", nil, 0); err == nil {
		t.Fatal("timed-out step reported success")
	}
	out, err := link.run("next", nil, 0)
	if err != nil || string(out) != "conn1:next" {
		t.Fatalf("step after a timeout did not run on a fresh connection: %q %v", out, err)
	}
	if dials != 1 || len(closed) != 1 || closed[0] != 0 {
		t.Fatalf("dead connection not replaced exactly once: dials %d closed %v", dials, closed)
	}
	if _, err := link.run("again", nil, 0); err != nil || dials != 1 {
		t.Fatalf("healthy link redialled: %v dials %d", err, dials)
	}
	link.close()
	if len(closed) != 2 || closed[1] != 1 {
		t.Fatalf("close did not close the current connection: %v", closed)
	}
}

func TestCarLinkGivesUpWhenTheCarCannotBeReached(t *testing.T) {
	var closed []int
	dials := 0
	link := &carLink{
		current: fakeConn{id: 0, timeout: map[string]bool{"slow": true}, closed: &closed},
		dial:    func() (linkConn, bool) { dials++; return nil, false },
	}
	_, _ = link.runLong("slow", nil, 0)
	for i := 0; i < 3; i++ {
		if _, err := link.run("next", nil, 0); err == nil {
			t.Fatal("step ran without a connection")
		}
	}
	if dials != 1 {
		t.Fatalf("each later step waited out its own reconnect: %d dials", dials)
	}
}

func TestFetchWatchdogLogsDownloadsWholeFiles(t *testing.T) {
	dump := t.TempDir()
	full := fakeWatchdogDump()
	for name, text := range map[string]string{"wd_err_omap_7.log": full, "wd_warn_omap_7_mem.log": "mem\n", "not_a_watchdog.log": "x\n"} {
		if err := os.WriteFile(filepath.Join(dump, name), []byte(text), 0600); err != nil {
			t.Fatal(err)
		}
	}
	logs, failed, err := fetchWatchdogLogs(dumpFixture{shellFixture{dir: t.TempDir()}, dump})
	if err != nil || len(failed) != 0 {
		t.Fatalf("fetch: %v, failed %v", err, failed)
	}
	if string(logs["wd_err_omap_7.log"]) != full || string(logs["wd_warn_omap_7_mem.log"]) != "mem\n" {
		t.Fatalf("logs not byte-exact: %d bytes vs %d", len(logs["wd_err_omap_7.log"]), len(full))
	}
	if _, ok := logs["not_a_watchdog.log"]; ok || len(logs) != 2 {
		t.Fatalf("fetched something that is not a watchdog log: %v", len(logs))
	}
}

func TestSameCrashDumpIsRecognisedAsAlreadyPulled(t *testing.T) {
	t.Setenv("AppData", t.TempDir())         // os.UserConfigDir on Windows
	t.Setenv("XDG_CONFIG_HOME", t.TempDir()) // and elsewhere
	sum := digest([]byte("core bytes"))
	if _, seen := corePulledBefore(sum); seen {
		t.Fatal("a never-pulled core reported as seen")
	}
	rememberCore(sum, "2026-11-18_10-00-00", "HmiMain3D.core")
	when, seen := corePulledBefore(sum)
	if !seen || when != "2026-11-18_10-00-00" {
		t.Fatalf("pulled core not recognised: %q %v", when, seen)
	}
	if _, seen := corePulledBefore(digest([]byte("another crash"))); seen {
		t.Fatal("a different core reported as seen")
	}
}
