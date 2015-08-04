package main

import (
	"github.com/gdamore/mangos"
	"github.com/gdamore/mangos/protocol/pull"
	"github.com/gdamore/mangos/transport/ipc"
	"github.com/gdamore/mangos/transport/tcp"
	"errors"
	"flag"
	"io"
	"log"
	"net"
	"strings"
	"sync"
)

var format string = "TCPURL,NNURL[,NNURL...]"
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
	for _, front := range fronts {
		wg.Add(1)
		go func(f *Front) { f.run(); wg.Done() }(front)
	}
	wg.Wait()
	for _, front := range fronts {
		front.clean()
	}
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
	feeder   mangos.Socket
	tokens   chan bool
}

func MakeFront(server *Server, concurrency int, config string) (*Front, error) {

	tokens := strings.Split(config, ",")
	if len(tokens) < 2 {
		return nil, BadFront
	}

	self := &Front{}

	// Prepare the front TCP socket
	if url, err := net.ResolveTCPAddr("tcp", tokens[0]); err != nil {
		return nil, err
	} else if ep, err := net.ListenTCP("tcp", url); err != nil {
		return nil, err
	} else {
		self.endpoint = ep
	}

	// Prepare the Nanomsg feeder
	if nn, err := pull.NewSocket(); err != nil {
		self.endpoint.Close()
		return nil, err
	} else {
		self.feeder = nn
		self.feeder.AddTransport(ipc.NewTransport())
		self.feeder.AddTransport(tcp.NewTransport())
	}
	for i := 1; i < len(tokens); i++ {
		self.feeder.Dial(tokens[i])
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
	self.feeder.Close()
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

func (self *Front) poll() (*net.TCPAddr, error) {
	if burl, err := self.feeder.Recv(); err != nil {
		log.Println("No URL available:", err)
		return nil, err
	} else {
		return net.ResolveTCPAddr("tcp", string(burl))
	}
}

func (self *Front) run() {
	var wg sync.WaitGroup
	for {
		self.consume()
		if cli, err := self.endpoint.AcceptTCP(); err != nil {
			log.Println("Accept() failed:", err)
		} else if tunnel, err := MakeTunnel(self, cli); err != nil {
			log.Println("Incoming connection rejected:", err)
			cli.Close()
			self.produce()
		} else {
			log.Println("Incoming", tunnel)
			wg.Add(1)
			go func(t *Tunnel) {
				defer self.produce()
				defer wg.Done()
				t.run()
			}(tunnel)
		}
	}
	wg.Wait()
}

type Tunnel struct {
	up    *Front
	front *net.TCPConn
	back  *net.TCPConn
}

func MakeTunnel(up *Front, cli *net.TCPConn) (*Tunnel, error) {
	if backAddr, err := up.poll(); err != nil {
		return nil, err
	} else if back, err := net.DialTCP("tcp", nil, backAddr); err != nil {
		return nil, err
	} else {
		self := &Tunnel{}
		self.up = up
		self.front = cli
		self.back = back
		return self, nil
	}
}

func (self *Tunnel) String() string {
	return "Tunnel{" + self.front.RemoteAddr().String() + "->" + self.back.RemoteAddr().String() + "}"
}

func (self *Tunnel) run() {
	defer self.front.Close()
	defer self.back.Close()
	var wg sync.WaitGroup
	wg.Add(1)
	go io.Copy(self.front, self.back)
	io.Copy(self.back, self.front)
	wg.Wait()
}
