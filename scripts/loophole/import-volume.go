// import-volume creates a loophole volume and writes raw data from a file into it.
// This works on macOS (no FUSE needed).
//
// Usage:
//
//	go run ./scripts/loophole/import-volume.go -volume qemu-rootfs -file /tmp/qemu-assets/rootfs.ext4 -profile local
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"strconv"
	"strings"

	"github.com/semistrict/loophole/env"
	"github.com/semistrict/loophole/storage"
)

func parseSize(s string) (uint64, error) {
	s = strings.TrimSpace(s)
	s = strings.ToUpper(s)
	multiplier := uint64(1)
	for _, suffix := range []struct {
		s string
		m uint64
	}{
		{"TB", 1 << 40},
		{"GB", 1 << 30},
		{"MB", 1 << 20},
		{"KB", 1 << 10},
	} {
		if strings.HasSuffix(s, suffix.s) {
			multiplier = suffix.m
			s = strings.TrimSuffix(s, suffix.s)
			break
		}
	}
	n, err := strconv.ParseUint(strings.TrimSpace(s), 10, 64)
	if err != nil {
		return 0, err
	}
	return n * multiplier, nil
}

func main() {
	volume := flag.String("volume", "", "volume name")
	file := flag.String("file", "", "path to raw image file")
	profile := flag.String("profile", "", "loophole profile name")
	sizeStr := flag.String("size", "", "volume size (e.g. 1TB, 100GB); defaults to file size")
	flag.Parse()

	if *volume == "" || *file == "" {
		fmt.Fprintf(os.Stderr, "Usage: import-volume -volume NAME -file PATH [-profile PROF]\n")
		os.Exit(1)
	}

	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo})))

	ctx := context.Background()
	dir := env.DefaultDir()

	cfg, err := env.LoadConfig(dir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load config: %v\n", err)
		os.Exit(1)
	}
	inst, err := cfg.Resolve(*profile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "resolve profile: %v\n", err)
		os.Exit(1)
	}

	diskCache, err := storage.OpenPageCacheForProfile(dir, inst)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open page cache: %v\n", err)
		os.Exit(1)
	}
	defer diskCache.Close()

	manager, err := storage.OpenManagerForProfile(ctx, inst, dir, diskCache)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open manager: %v\n", err)
		os.Exit(1)
	}
	defer manager.Close()

	// Get file size for volume creation.
	f, err := os.Open(*file)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open file: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	fi, err := f.Stat()
	if err != nil {
		fmt.Fprintf(os.Stderr, "stat file: %v\n", err)
		os.Exit(1)
	}
	size := uint64(fi.Size())
	if *sizeStr != "" {
		parsed, err := parseSize(*sizeStr)
		if err != nil {
			fmt.Fprintf(os.Stderr, "invalid size %q: %v\n", *sizeStr, err)
			os.Exit(1)
		}
		size = parsed
	}

	// Create volume.
	fmt.Fprintf(os.Stderr, "Creating volume %q (size %d bytes, %.1f MiB)\n", *volume, size, float64(size)/(1<<20))
	vol, err := manager.NewVolume(storage.CreateParams{
		Volume: *volume,
		Size:   size,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "create volume: %v\n", err)
		os.Exit(1)
	}
	if err := vol.AcquireRef(); err != nil {
		fmt.Fprintf(os.Stderr, "acquire ref: %v\n", err)
		os.Exit(1)
	}

	// Write file data into volume.
	buf := make([]byte, 4*1024*1024) // 4 MiB chunks
	var offset uint64
	for {
		n, readErr := f.Read(buf)
		if n > 0 {
			if err := vol.Write(buf[:n], offset); err != nil {
				fmt.Fprintf(os.Stderr, "write at offset %d: %v\n", offset, err)
				os.Exit(1)
			}
			offset += uint64(n)
			fmt.Fprintf(os.Stderr, "\r  %d / %d MiB", offset>>20, size>>20)
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			fmt.Fprintf(os.Stderr, "\nread: %v\n", readErr)
			os.Exit(1)
		}
	}
	fmt.Fprintf(os.Stderr, "\n")

	// Flush and release.
	if err := vol.Flush(); err != nil {
		fmt.Fprintf(os.Stderr, "flush: %v\n", err)
		os.Exit(1)
	}
	if err := vol.ReleaseRef(); err != nil {
		fmt.Fprintf(os.Stderr, "release ref: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "Done. Volume %q ready.\n", *volume)
}
