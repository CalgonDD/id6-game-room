package main

import (
	"bytes"
	"encoding/json"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPS1CandidateEmbeddedPayloadHasOnlyAllowedGameRoomFiles(t *testing.T) {
	allowed := map[string]bool{}
	for _, name := range append([]string{"GameRoom.manifest", "GameRoom.xml", "GameRoom_ALL_Images.zip", "GameRoom_ALL_Texts.zip", "libGameRoom.so", "freedoom-qnx"}, appPayloadExtras("GameRoom")...) {
		allowed[name] = true
	}
	entries, err := fs.ReadDir(embeddedPayload, "payload/GameRoom")
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if entry.IsDir() || !allowed[entry.Name()] {
			t.Fatalf("unexpected embedded payload: %s", entry.Name())
		}
		delete(allowed, entry.Name())
	}
	if len(allowed) != 0 {
		t.Fatalf("missing embedded files: %v", allowed)
	}
}

func TestEmptyFolderRemovalPreservesHiddenFilesAndLinks(t *testing.T) {
	for _, seed := range []string{`echo keep > "$d/.hidden"`, `mkdir "$d/child"`, `ln -s nowhere "$d/dangling"`} {
		t.Run(seed, func(t *testing.T) {
			dir := t.TempDir()
			live := filepath.Join(dir, "leftover with spaces")
			if err := os.Mkdir(live, 0755); err != nil {
				t.Fatal(err)
			}
			f := shellFixture{dir: dir}
			if _, err := f.run("d="+shellQuote(testShellPath(live))+"; "+seed, nil, 512); err != nil {
				t.Fatal(err)
			}
			if _, err := f.run(emptyDirRemoval(testShellPath(live), hostFileOps), nil, 512); err != nil {
				t.Fatal(err)
			}
			if _, err := os.Stat(live); err != nil {
				t.Fatal("populated directory was removed", err)
			}
			if _, err := f.run(`d=`+shellQuote(testShellPath(live))+`; test -e "$d/.hidden" || test -d "$d/child" || test -L "$d/dangling"`, nil, 512); err != nil {
				t.Fatal("contents lost", err)
			}
		})
	}
}

func TestEmptyFolderRemovalRaceAndFailuresAreSafe(t *testing.T) {
	for _, race := range []bool{true, false} {
		dir := t.TempDir()
		live := filepath.Join(dir, "leftover")
		if err := os.Mkdir(live, 0755); err != nil {
			t.Fatal(err)
		}
		ops := filepath.Join(dir, "helper with spaces")
		script := "#!/bin/sh\nexit 23\n"
		if race {
			script = "#!/bin/sh\nprintf keep > \"$2/arrived\"\nexec " + shellQuote(hostFileOps) + " \"$@\"\n"
		}
		if err := os.WriteFile(ops, []byte(script), 0755); err != nil {
			t.Fatal(err)
		}
		f := shellFixture{dir: dir}
		if _, err := f.run(emptyDirRemoval(testShellPath(live), testShellPath(ops)), nil, 512); err == nil {
			t.Fatal("removal failure was hidden")
		}
		if _, err := os.Stat(live); err != nil {
			t.Fatal("directory disappeared", err)
		}
		if race {
			data, err := os.ReadFile(filepath.Join(live, "arrived"))
			if err != nil || string(data) != "keep" {
				t.Fatal("concurrent file was deleted", err)
			}
		}
	}
}

func TestEmptyFolderRemovalDoesNotFollowDirectorySymlink(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "target")
	if err := os.Mkdir(target, 0755); err != nil {
		t.Fatal(err)
	}
	f := shellFixture{dir: dir}
	link := testShellPath(filepath.Join(dir, "link"))
	if _, err := f.run("ln -s "+shellQuote(testShellPath(target))+" "+shellQuote(link), nil, 512); err != nil {
		t.Fatal(err)
	}
	if _, err := f.run(emptyDirRemoval(link, hostFileOps), nil, 512); err != nil {
		t.Fatal(err)
	}
	if _, err := f.run("test -L "+shellQuote(link), nil, 512); err != nil {
		t.Fatal("link was removed", err)
	}
	if _, err := os.Stat(target); err != nil {
		t.Fatal("target was removed", err)
	}
}

func TestPS1CandidateLanguageIncludesPlayStation(t *testing.T) {
	files, err := filepath.Glob("web/i18n/*.json")
	if err != nil {
		t.Fatal(err)
	}
	for _, file := range files {
		data, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		var labels map[string]string
		if err := json.Unmarshal(data, &labels); err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(labels["subtitle"], "PlayStation") {
			t.Errorf("missing PS1 candidate offer in %s", file)
		}
	}
}

func TestRecoveryDiagnosticsCaptureArchiveAndExposeReadFailure(t *testing.T) {
	dir := t.TempDir()
	archive := filepath.Join(dir, "fileArchive.log")
	if err := os.WriteFile(archive, []byte("recovery clue\n"), 0644); err != nil {
		t.Fatal(err)
	}
	start := strings.Index(gamesDiagnostics, "echo '=== games: recovery evidence fileArchive.log")
	if start < 0 {
		t.Fatal("missing recovery section")
	}
	script := bytes.ReplaceAll([]byte(gamesDiagnostics[start:]), []byte("/var/dump/fileArchive.log"), []byte(testShellPath(archive)))
	f := shellFixture{dir: dir}
	out, err := f.run("/bin/sh -s", script, 8192)
	if err != nil || !strings.Contains(string(out), "recovery clue") || !strings.Contains(string(out), "fileArchive_read_status=0") {
		t.Fatalf("archive capture: %q %v", out, err)
	}
	failed := append([]byte("tail() { echo simulated-read-error; return 7; }\n"), script...)
	out, err = f.run("/bin/sh -s", failed, 8192)
	if err != nil || !strings.Contains(string(out), "fileArchive_read_status=7") || !strings.Contains(string(out), "recovery evidence end") {
		t.Fatalf("failed read status lost: %q %v", out, err)
	}
}
