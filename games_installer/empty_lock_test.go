package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func TestReleaseOnlyPreservesIncompleteTransactionsAndApps(t *testing.T) {
	f, dir := orphanLockFixture(t)
	backup := filepath.Join(dir, "AppInstaller-Backup-fedcba9876543210", "old-GameRoom")
	if err := os.MkdirAll(backup, 0755); err != nil {
		t.Fatal(err)
	}
	clue := filepath.Join(backup, "preserve.txt")
	if err := os.WriteFile(clue, []byte("recovery evidence"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(filepath.Join(dir, "Plugins/GameRoom/GameRoom.manifest")); err != nil {
		t.Fatal(err)
	}
	state, _, err := inspectInstallLock(f, "")
	if err != nil || state != "review" {
		t.Fatalf("full repair guard changed: %q %v", state, err)
	}
	_, err = install(f, "169.254.21.3", panel{1280, 480}, false, apps)
	blocked, ok := err.(*blockedInstall)
	if !ok || blocked.canRecover || !blocked.canUnlockEmpty {
		t.Fatalf("missing release-only option: %v", err)
	}
	if err := releaseEmptyInstallLock(f); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "ID6-install-lock")); !os.IsNotExist(err) {
		t.Fatal("lock remains", err)
	}
	if got, err := os.ReadFile(clue); err != nil || string(got) != "recovery evidence" {
		t.Fatal("backup changed", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "Plugins/GameRoom")); err != nil {
		t.Fatal("live directory changed", err)
	}
	if work, _ := filepath.Glob(filepath.Join(dir, "GameRoom-Unlock-*")); len(work) != 0 {
		t.Fatalf("helper folder left behind: %v", work)
	}
}

type lockRaceFixture struct {
	shellFixture
	inject bool
}

func (f lockRaceFixture) run(cmd string, input []byte, limit int) ([]byte, error) {
	if f.inject && bytes.Contains(input, []byte("echo released-empty-lock")) {
		if err := os.WriteFile(filepath.Join(f.dir, "ID6-install-lock/.arrived"), []byte("owner"), 0644); err != nil {
			return nil, err
		}
	}
	return f.shellFixture.run(cmd, input, limit)
}
func TestReleaseEmptyLockRefusesChangedLockAndHelperFailure(t *testing.T) {
	for _, scenario := range []string{"nonempty", "raced", "helper-failed"} {
		t.Run(scenario, func(t *testing.T) {
			fixture, dir := orphanLockFixture(t)
			if scenario == "nonempty" {
				if err := os.WriteFile(filepath.Join(dir, "ID6-install-lock/.owner"), nil, 0644); err != nil {
					t.Fatal(err)
				}
			}
			fixture.failUnlock = scenario == "helper-failed"
			f := lockRaceFixture{fixture, scenario == "raced"}
			if err := releaseEmptyInstallLock(f); err == nil {
				t.Fatal("lock was unexpectedly released")
			}
			if _, err := os.Stat(filepath.Join(dir, "ID6-install-lock")); err != nil {
				t.Fatal("lock removed", err)
			}
		})
	}
}
