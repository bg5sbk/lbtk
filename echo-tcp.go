package main

import (
	"errors"
	"flag"
	"io"
	"log"
	"net"
	"sync"
)

var format string = "IP:PORT"
var BadFront error = errors.New("Invalid front description, expecting " + format)

func main() {
	flag.Parse()
	server := Server{}
	// TODO replace 8192 by half the system limit on open files
	server.init(8192)

	fronts := make([]*Front, 0)
	for i := 0; i < flag.NArg(); i++ {
		if front, err := MakeFront(&server, 1024, flag.Arg(i)); err != nil {
			log.Fatal("Failed to configure front with", flag.Arg(i), ":", err)
		} else {
			fronts = append(fronts, front)
		}
	}
	if len(fronts) <= 0 {
		log.Fatal("No valid front found in arguments. Expecting " + format)
	}

	var wg sync.WaitGroup

	ontunnel := func(f *Front, cli *net.TCPConn) {
		defer wg.Done()
		defer f.produce()
		io.Copy(cli, cli)
		cli.Close()
	}
	onfront := func(f *Front) {
		defer wg.Done()
		defer f.clean()
		for {
			f.consume()
			if cli, err := f.endpoint.AcceptTCP(); err != nil {
				log.Println("Accept() failed:", err)
			} else {
				wg.Add(1)
				go ontunnel(f, cli)
			}
		}
	}

	for _, front := range fronts {
		wg.Add(1)
		go onfront(front)
	}
	wg.Wait()
}

type Server struct {
	running bool
	tokens  chan bool
}

func (self *Server) init(concurrent uint) {
	self.running = true
	self.tokens = make(chan bool, concurrent)
	for ; concurrent > 0; concurrent-- {
		self.tokens <- true
	}
}

func (self *Server) consume() {
	<-self.tokens
}

func (self *Server) produce() {
	self.tokens <- true
}

type Front struct {
	server   *Server
	endpoint *net.TCPListener
	tokens   chan bool
}

func MakeFront(server *Server, concurrency int, config string) (*Front, error) {

	self := &Front{}

	// Prepare the front TCP socket
	if url, err := net.ResolveTCPAddr("tcp", config); err != nil {
		return nil, err
	} else if ep, err := net.ListenTCP("tcp", url); err != nil {
		return nil, err
	} else {
		self.endpoint = ep
	}

	// Eventually, finalize the structure
	self.server = server
	self.tokens = make(chan bool, concurrency)
	for ; concurrency > 0; concurrency-- {
		self.tokens <- true
	}

	return self, nil
}

func (self *Front) clean() {
	self.endpoint.Close()
	close(self.tokens)
}

func (self *Front) consume() {
	self.server.consume()
	<-self.tokens
}

func (self *Front) produce() {
	self.tokens <- true
	self.server.produce()
}
