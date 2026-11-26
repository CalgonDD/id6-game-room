package main

import (
	"errors"
	"fmt"
	"strings"
)

// This is a separate, owner-invoked operation. It releases no files other than
// the empty lock and never repairs/publishes/deletes an app transaction.
const emptyLockGuard = `test -d "$lock" && test ! -L "$lock" || exit 1
for member in "$lock"/* "$lock"/.[!.]* "$lock"/..?*; do
    if test -e "$member" || test -L "$member"; then exit 1; fi
done
`

func emptyInstallLock(r remoteRunner) (bool, error) {
	script := "lock=" + shellQuote(installLock) + "\n" + emptyLockGuard + "echo empty-lock\n"
	out, err := r.run("/bin/sh -s", []byte(script), 256)
	if err != nil {
		return false, err
	}
	return strings.TrimSpace(string(out)) == "empty-lock", nil
}
func releaseEmptyInstallLock(r remoteRunner) error {
	empty, err := emptyInstallLock(r)
	if err != nil || !empty {
		return errors.New("the lock is missing, nonempty or not a real directory; nothing was removed")
	}
	// Random private helper folder is not an app/backup transaction. Large game data
	// never goes here. Its only content is the trusted embedded file helper.
	work := root + fmt.Sprintf("/GameRoom-Unlock-%016x", randomNonce())
	if _, err = r.run("umask 077; mkdir "+shellQuote(work), nil, 512); err != nil {
		return fmt.Errorf("could not prepare lock helper: %w", err)
	}
	defer r.run("rm -rf "+shellQuote(work), nil, 512)
	if err = stageFileOps(r, work); err != nil {
		return fmt.Errorf("could not stage lock helper: %w", err)
	}
	script := "lock=" + shellQuote(installLock) + "\nops=" + shellQuote(work+"/"+fileOpsName) + "\n" + emptyLockGuard + `"$ops" unlock "$lock" || exit 1
echo released-empty-lock
`
	out, err := r.run("/bin/sh -s", []byte(script), 512)
	if err != nil || strings.TrimSpace(string(out)) != "released-empty-lock" {
		return errors.New("the lock could not be released; installed files and backup folders were not changed")
	}
	return nil
}
