package main

import (
	"os"
	"path/filepath"
	"testing"
)

func orphanLockFixture(t *testing.T) (shellFixture, string) {
	t.Helper()
	dir := t.TempDir()
	for _, sub := range []string{"ID6-install-lock", "Plugins/GameRoom"} {
		if err := os.MkdirAll(filepath.Join(dir, sub), 0755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(dir, "Plugins/GameRoom/GameRoom.manifest"), []byte("unchanged manifest"), 0644); err != nil {
		t.Fatal(err)
	}
	return shellFixture{dir: dir}, dir
}

func TestOrphanEmptyLockRequiresExplicitRecoveryAndRetainsLiveFiles(t *testing.T) {
	f, dir := orphanLockFixture(t)
	state, _, err := inspectInstallLock(f, "")
	if err != nil || state != "staged-only" {
		t.Fatalf("orphan state=%q err=%v", state, err)
	}
	lock := filepath.Join(dir, "ID6-install-lock")
	if _, err := os.Stat(lock); err != nil {
		t.Fatal("inspection removed lock", err)
	}
	_, cleanup, err := ownWorkFolder(f)
	if cleanup != nil {
		t.Fatal("blocked upload unexpectedly owns a work folder")
	}
	blocked, ok := err.(*blockedInstall)
	if !ok || !blocked.canRecover {
		t.Fatalf("upload did not offer recovery: %v", err)
	}
	tx := filepath.Join(dir, "AppInstaller-Backup-0123456789abcdef")
	if err := os.Mkdir(tx, 0755); err != nil {
		t.Fatal(err)
	}
	state, _, err = inspectInstallLock(f, root+"/AppInstaller-Backup-0123456789abcdef")
	if err != nil || state != "cleared" {
		t.Fatalf("recovery=%q err=%v", state, err)
	}
	if _, err := os.Stat(lock); !os.IsNotExist(err) {
		t.Fatal("lock remains", err)
	}
	if got, err := os.ReadFile(filepath.Join(dir, "Plugins/GameRoom/GameRoom.manifest")); err != nil || string(got) != "unchanged manifest" {
		t.Fatal("live app changed", err)
	}
}

func TestOrphanLockRefusesAmbiguousOrNonemptyState(t *testing.T) {
	for _, scenario := range []string{"lock-member", "missing-manifest", "incomplete-transaction", "publication", "bad-complete-marker"} {
		t.Run(scenario, func(t *testing.T) {
			f, dir := orphanLockFixture(t)
			tx := filepath.Join(dir, "AppInstaller-Backup-fedcba9876543210")
			var err error
			switch scenario {
			case "lock-member":
				err = os.WriteFile(filepath.Join(dir, "ID6-install-lock/.owner"), []byte("active"), 0644)
			case "missing-manifest":
				err = os.Remove(filepath.Join(dir, "Plugins/GameRoom/GameRoom.manifest"))
			default:
				err = os.Mkdir(tx, 0755)
				if err == nil && scenario == "publication" {
					err = os.Mkdir(filepath.Join(tx, "old-GameRoom"), 0755)
				}
				if err == nil && scenario == "bad-complete-marker" {
					err = os.Mkdir(filepath.Join(tx, "complete"), 0755)
				}
			}
			if err != nil {
				t.Fatal(err)
			}
			state, _, err := inspectInstallLock(f, "")
			if err != nil || state != "review" {
				t.Fatalf("state=%q err=%v", state, err)
			}
			state, _, err = inspectInstallLock(f, root+"/AppInstaller-Backup-0123456789abcdef")
			if err != nil || state != "review" {
				t.Fatalf("recovery bypassed guard: %q %v", state, err)
			}
			if _, err := os.Stat(filepath.Join(dir, "ID6-install-lock")); err != nil {
				t.Fatal("lock was removed", err)
			}
		})
	}
}

func TestCompletedTransactionDoesNotStrandOrphanLock(t *testing.T) {
	f, dir := orphanLockFixture(t)
	tx := filepath.Join(dir, "AppInstaller-Backup-fedcba9876543210")
	if err := os.MkdirAll(filepath.Join(tx, "old-GameRoom"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(tx, "complete"), nil, 0644); err != nil {
		t.Fatal(err)
	}
	state, _, err := inspectInstallLock(f, "")
	if err != nil || state != "staged-only" {
		t.Fatalf("completed state=%q err=%v", state, err)
	}
	if _, err := os.Stat(filepath.Join(tx, "old-GameRoom")); err != nil {
		t.Fatal("backup changed", err)
	}
}
