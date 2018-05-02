package main

import (
	"io"
	"path/filepath"
	"os"
	"bufio"
	"bytes"
	"strings"
)

func main() {
	root := os.Args[1]
	subs := os.Args[2]

	submap := make(map[string]string)
	f, err := os.Open(subs)
	if err != nil {
		panic(err)
	}
	sc := bufio.NewReaderSize(f, 1 << 24)
	for {
		line, err := sc.ReadString('\n')
		if err != nil {
			if err != io.EOF {
				panic(err)
			}
			break
		}
		split := strings.Split(line, ":")
		submap[split[0]] = split[1]
	}

	err = filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if !info.Mode().IsRegular() || len(path) < 6 || path[len(path)-6:] != ".ninja" {
			return nil
		}

		f, err := os.Open(path)
		if err != nil {
			panic(err)
		}
		sc := bufio.NewReaderSize(f, 1 << 24)
		var buf bytes.Buffer
		for {
			line, err := sc.ReadString('\n')
			if err != nil {
				if err != io.EOF {
					panic(err)
				}
				break
			}
			if len(line) > 6 && line[0:6] == "build " {
				split := strings.Split(line, ":")
				s, ok := submap[split[0]]
				if ok {
					buf.WriteString(split[0] + ":" + s)
				} else {
					buf.WriteString(line)
				}
			} else {
				buf.WriteString(line)
			}
		}
		f.Close()

		f, err = os.Create(path)
		if err != nil {
			panic(err)
		}
		f.Write(buf.Bytes())
		f.Close()

		return nil
	})
	if err != nil {
		panic(err)
	}
}
