package main

import (
	"github.com/gdamore/mangos"
	"github.com/gdamore/mangos/protocol/push"
	"github.com/gdamore/mangos/transport/ipc"
	"github.com/gdamore/mangos/transport/tcp"
	"bufio"
	"flag"
	"log"
	"math/rand"
	"os"
	"strings"
)

var totrim string = "\r\n\t "

func input(out chan []string) {
	reader := bufio.NewReader(os.Stdin)
	buffer := make([]string, 0)
	for {
		l, err := reader.ReadString('\n')
		l = strings.TrimRight(strings.TrimLeft(l, totrim), totrim)
		if err != nil {
			out <- buffer
			close(out)
			log.Println("input error:", err)
			return
		} else if l == "" {
			out <- buffer
			buffer = make([]string, 0)
		} else {
			buffer = append(buffer, l)
		}
	}
}

func reload(in chan []string, out chan string, poll func(tab []string) string) {
	var next string
	var tab []string
	for {
		if next == "" {
			tab = <-in
			next = poll(tab)
		} else {
			select {
			case tab = <-in:
				log.Println("Array reloaded with", len(tab), "items")
			case out <- next:
				next = ""
				next = poll(tab)
			}
		}
	}
}

func output(in chan string, out mangos.Socket) {
	if out == nil { panic("Invalid socket"); }
	for item := range in {
		out.Send([]byte(item))
	}
}

func Gen(out mangos.Socket, poll func([]string) string) {
	if out == nil { panic("Invalid socket"); }
	p0 := make(chan []string)
	p1 := make(chan string)
	go input(p0)
	go reload(p0, p1, poll)
	output(p1, out)
}

func main() {
	how_rand := flag.Bool("rand", false, "")
	flag.Parse()
	if flag.NArg() < 1 {
		log.Fatal("Missing arguments: at least one endpoint to bind to")
	}

	var err error
	var out mangos.Socket
	if out, err = push.NewSocket(); err != nil {
		log.Fatal("Nanomsg socket creation failure: ", err)
	} else {
		defer out.Close()
		out.AddTransport(ipc.NewTransport())
		out.AddTransport(tcp.NewTransport())
		for i:=0; i<flag.NArg() ;i++ {
			if err := out.Listen(flag.Arg(i)); err != nil {
				log.Fatal("Nanomsg listen() error: ", err)
			}
		}
	}

	switch {
		case *how_rand:
			Gen(out, func(tab []string) string {
				if len(tab) <= 0 {
					return ""
				}
				return tab[rand.Intn(len(tab))]
			})
		default:
			i := 0
			Gen(out, func(tab []string) string {
				if len(tab) <= 0 {
					return ""
				}
				i = (i + 1) % len(tab)
				return tab[i]
			})
	}
}
