package main

import (
	"errors"
	"fmt"
	"hash/fnv"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The real SSH transport reads directly from disk. The byte-slice interface
// remains unchanged for existing apps and small bundled files.
func (r remote) runReaderDeadline(cmd string, input io.Reader, limit int, timeout time.Duration) ([]byte, error) {
	_ = r.socket.SetDeadline(time.Now().Add(timeout))
	s, err := r.client.NewSession()
	if err != nil {
		return nil, err
	}
	defer s.Close()
	out, stderr := bounded{limit: limit}, bounded{limit: 2048}
	s.Stdin = input
	s.Stdout, s.Stderr = &out, &stderr
	if err = s.Run(cmd); err != nil {
		if detail := strings.TrimSpace(stderr.data.String()); detail != "" {
			return out.data.Bytes(), fmt.Errorf("%w: %s", err, detail)
		}
		return out.data.Bytes(), err
	}
	return out.data.Bytes(), nil
}

type pspStreamRunner interface {
	runReaderDeadline(string, io.Reader, int, time.Duration) ([]byte, error)
}

func putPSPGames(r remoteRunner, tx string, games []ownGame) error {
	streamer, ok := r.(pspStreamRunner)
	if !ok {
		return errors.New("this connection cannot stream PSP game files")
	}
	ops := tx + "/" + fileOpsName
	dir := "PSP/yours"
	stageDir := gamesStaging + "/" + strings.TrimPrefix(tx[strings.LastIndex(tx, "/")+1:], backupPrefix) + "-PSP-yours"
	staged := false
	defer func() {
		if staged {
			_, _ = r.run("rm -rf "+shellQuote(stageDir), nil, 512)
		}
	}()
	var todo []stagedFile
	for i, g := range games {
		if g.Game != "PSP" || g.Sub != "yours" || g.member != "" || filepath.Base(g.Name) != g.Name || g.Name == "." || g.Name == ".." {
			return errors.New("invalid PSP upload destination")
		}
		item, err := func() (stagedFile, error) {
			f, err := os.Open(g.source)
			if err != nil {
				return stagedFile{}, err
			}
			defer f.Close()
			st, err := f.Stat()
			if err != nil {
				return stagedFile{}, err
			}
			if !st.Mode().IsRegular() || st.Size() != g.Size || st.Size() > ownMaxDisc {
				return stagedFile{}, errors.New("the chosen PSP file changed; check it again")
			}
			// Recheck content on this same handle before hashing/uploading.
			checked := ownGame{}
			ext := strings.ToLower(filepath.Ext(g.source))
			classifyPSP(&checked, strings.TrimSuffix(filepath.Base(g.source), filepath.Ext(g.source)), ext, f, st.Size())
			if checked.Problem != "" {
				return stagedFile{}, errors.New(checked.Problem)
			}
			hash := fnv.New64a()
			n, err := io.Copy(hash, f)
			if err != nil || n != st.Size() {
				return stagedFile{}, errors.New("could not read the complete PSP file")
			}
			want := fmt.Sprintf("%016x", hash.Sum64())
			target := gamesRoot + "/" + dir + "/" + g.Name
			if got, err := remoteHashSized(r, ops, target, int(st.Size())); err == nil && got == want {
				gameNote("PSP: %s is already on the hard drive, unchanged.", g.Name)
				return stagedFile{}, nil
			}
			if !staged {
				script := fmt.Sprintf("test -d %[1]s && test -w %[1]s || exit 3\ntest -d %[2]s || mkdir %[2]s || exit 1\nmkdir %[3]s || exit 1\n", shellQuote(shareMount), shellQuote(gamesStaging), shellQuote(stageDir))
				if _, err := r.run("/bin/sh -s", []byte(script), 256); err != nil {
					return stagedFile{}, fmt.Errorf("could not prepare HDD staging: %w", err)
				}
				staged = true
			}
			if _, err := f.Seek(0, io.SeekStart); err != nil {
				return stagedFile{}, err
			}
			path := fmt.Sprintf("%s/f%04d", stageDir, i)
			sentHash := fnv.New64a()
			source := &io.LimitedReader{R: io.TeeReader(f, sentHash), N: st.Size() + 1}
			budget := 30*time.Second + time.Duration(st.Size()/(1<<20))*time.Second
			if _, err := streamer.runReaderDeadline("umask 022; set -C; cat > "+shellQuote(path), source, 512, budget); err != nil {
				return stagedFile{}, err
			}
			if source.N != 1 || fmt.Sprintf("%016x", sentHash.Sum64()) != want {
				return stagedFile{}, errors.New("PSP file changed while being uploaded; nothing published")
			}
			if got, err := remoteHashSized(r, ops, path, int(st.Size())); err != nil || got != want {
				return stagedFile{}, errors.New("PSP upload readback hash differs")
			}
			return stagedFile{path, g.Name, want, int(st.Size())}, nil
		}()
		if err != nil {
			return fmt.Errorf("could not upload %s: %w", g.Name, err)
		}
		if item.staged != "" {
			todo = append(todo, item)
		}
	}
	if len(todo) == 0 {
		return nil
	}
	total := 0
	for _, item := range todo {
		total += item.size
	}
	out, err := runTimed(r, "/bin/sh -s", gameFilesScript(ops, dir, todo), 1024, 45*time.Second+3*carTime(total))
	if err != nil {
		return err
	}
	if strings.Contains(string(out), "mode=ro") {
		check := []byte("if test -w " + shellQuote(gamesMount) + "; then echo rw; else echo ro; fi\n")
		state, err := r.run("/bin/sh -s", check, 128)
		if err != nil || strings.TrimSpace(string(state)) != "ro" {
			return errors.New("PSP files were copied, but the hard drive could not be verified read-only again. Restart iDrive before continuing")
		}
	}
	gameNote("PSP: %d game(s) verified and stored on the hard drive.", len(todo))
	return nil
}
