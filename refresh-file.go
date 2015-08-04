package main

import (
	"bufio"
	. "github.com/golang/exp/inotify"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"strings"
	"time"
)

const (
	flags  = IN_DONT_FOLLOW | IN_MODIFY | IN_DELETE_SELF | IN_MOVE | IN_MOVE_SELF
	blanks = "\t\r\n "
)

func main() {
	flag.Parse()
	if flag.NArg() != 1 {
		log.Fatal("No path in CLI arguments")
	} else if w, err := NewWatcher(); err != nil {
		log.Fatal("inotify init error : ", err)
	} else {
		defer w.Close()
		for {
			watch(w, flag.Arg(0))
			time.Sleep(1 * time.Second)
		}
	}
}

func watch(w *Watcher, path string) {
	if err := w.AddWatch(path, flags); err != nil {
		log.Printf("inotify watch error [%v]: %s\n", path, err)
		return
	}
	defer w.RemoveWatch(path)

	for {
		select {
		case evt := <-w.Event:
			log.Printf("inotify event : %v\n", evt)
			if 0 != evt.Mask&IN_DELETE_SELF {
				return
			}
			if 0 != evt.Mask&IN_MODIFY {
				reload(path)
			}
		case err := <-w.Error:
			log.Printf("inotify error : %v\n", err)
		}
	}
}

func reload(path string) {
	if fin, err := os.Open(path); err != nil {
		log.Printf("Open(%v) error : %v\n", path, err)
	} else {
		defer fin.Close()
		log.Printf("Reloading [%v]\n", path)
		r := bufio.NewReader(fin)
		any := false
		for {
			if line, err := r.ReadString('\n'); err != nil {
				if err != io.EOF {
					log.Printf("Read error from [%v] : %v\n", path, err)
				}
				break
			} else {
				line = strings.TrimLeft(line, blanks)
				line = strings.TrimRight(line, blanks)
				if strings.HasPrefix(line, "#") {
					continue
				}
				if _, err := net.ResolveTCPAddr("tcp", line); err == nil {
					fmt.Printf("%s\n", line)
					any = true
				}
			}
		}
		if any {
			fmt.Printf("\n")
		}
	}
}
