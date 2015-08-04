package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"time"
)

func main() {
	flag.Parse()
	if flag.NArg() < 1 {
		log.Fatalf("No URL on command line")
	}
	for i := 0; i < flag.NArg(); i++ {
		if _, err := net.ResolveTCPAddr("tcp", flag.Arg(i)); err != nil {
			log.Fatalf("Invalid URL at %v [%v] : %v", i, flag.Arg(i), err)
		}
	}
	for {
		for i := 0; i < flag.NArg(); i++ {
			fmt.Println(flag.Arg(i))
		}
		fmt.Println("")
		time.Sleep(1 * time.Minute)
	}
}
