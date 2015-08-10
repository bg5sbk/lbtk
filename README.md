# lbtk

**lbtk** is a toolkit aiming an easy set-up for stateless and flexible load-balancers. 

## Advocacy

I wanted to a set of load-balancers performing stateless load-balancing.

I keep in mind a few guidelines for the target product:
  * Keep the codebase as light and simple as possible. The lightest a proxy is, the most eligible it is to small devices.
  * Be able to respect a centralized load-balancing policy.
  * Adapt quickly to the status of the targeted services (i.e. up/down, loaded/idle, etc).
  * Manage a huge set of targeted services.

Respecting a centralized policy means "sharing at least something" between the proxies.
To avoid replicating across the proxies a a high-volume-of-updates-on-a-huge-list-of-services, I made the choice to extract the load-balancing logic and data from the proxies and isolate it in separate processes.
So that only few processes are responsible for the polling while the mass of proxies only manage networking tasks.

Here is typically a producer of "load-balanced items" and a set of consumers (the proxies). Because we do not want a tuple to be consumed by more than one consumer, we consider a Queue as a transport, in a "1 to Any" (or "PUSH/PULL") pattern (as opposed to "1 to Many" or PUB/SUB).

### Which technologies ?

The simplest form of producer/consumer pair was a pair of processes connected with a simple pipe.

Starting here, I first inspected [haproxy][ha] to make it consumes a file descriptor. In vain, because this is not at all how [haproxy][ha] has been designed. So I wrote a minimal TCP proxy in [Go][go].

Then the communication management has been delegated to a messenging API. Being a big fan of  [ZeroMQ][zmq] (or [Nanomsg][nn]), I chosed [Nanomsg][nn] because of its pure [Go][go] implementation.
The protocol between the load-balancer and the proxy is then fairly simple: a [Nanomsg][nn] transport, where each message carries a single CIDR address/port couple.

Here came **lbtk**.

### Pros
* The central load-balancing service distributes items. It seems straightforward to assimilate each item to a token.
* The output of the LB service is "not so important", and is directly linear to the connections establishment rate.
* Primary/Secondary queues can be easily setup thanks to [Nanomsg][nn]'s features.

### Cons
* Such a solution will always be more complicated to setup than a good ole [haproxy][ha] on highly static environement
* the provided proxies availability strongly depends on the LB's throughput and availability
* they won't ever be able to perform load-balancing based on any form of hash

## Tools

The solution is divided into 3 main parts: **refresher | generator | proxy**
* ``refreshers`` that generate sets of items
* ``generators`` that produce load-balanced tuples of items in an PUSH queue (one tuple / line)
* ``consumers`` : consumes and uses a PULL queue of load-balanced items. 

Currently, several tools have been developped.

Refreshers:
* **refresh-static** [Go][go] refresher outputting always the same list, received in the command line arguments. Nothing more than a bunch of checks on the input before a loop on printf.
* **refresh-file** [Go][go] monitors a file and outputs its content. Linux specific.
* **refresh-zk** [Go][go] refresher based on [Apache Zookeeper][zk]
* **refresh-etcd** [Go][go] refresher based on [CoreOS Etcd][etcd]
* **refresh-oio-cluster.sh** Shell script wraping [OpenIO SDS][oio]'s cluster command in a loop. To be used on a machine belonging to an [OpenIO SDS][oio] architecture, where an agent runs, or at least from where the conscience is reachable.
* **refresh-oio-proxy.py** Python script periodically contacting an [OpenIO SDS][oio]'s proxy via its HTTP/JSON interface.
* **refresh-dns** [Go][go] refresher performing periodical DNS requests on SRV records.

Generators:
* **gen** generates addresses on a PUSH queue.
By default, each address is chosen according to a pure Round-Robin among a set of addresses.
A command line option activates a Random pooling each time an address is extracted from the set.
These addresses are received on the standard input, and each time a list is received it refreshed the internal set of services.

Consumers / Proxies:
* **proxy-tcp-splice** is a ``splice``/``epoll`` based implementation of a TCP proxy.
This is very Linux specific and targets recent Linux releases.
But it allows working on streams in a zero-copy fashion.
* **proxy-tcp** is [Go][go] implementation of a TCP proxy.
Portable but works on streams in userland space, with one goroutine per stream.

Test services:
* **echo-tcp-splice** is a ``splice``/``epoll`` based implementation of a TCP echo server.
It is very Linux specific, and requires recent kernel releases.
But it allows working on streams with zero-copy operations.
* **echo-tcp** is a [Go][go] implementation of a TCP echo server.
Portable but works on streams in userland space, with one goroutine per stream.

## Examples

### Topology

Let's deploy this sample topology:

![sample topology](https://raw.githubusercontent.com/jfsmig/lbtk/master/docs/sample_topology.svg)

Start a load-balancer performing a simple Round-Robin on ``127.0.0.1:8000`` and ``127.0.0.1:8001``, binding on ``tcp://127.0.0.1:1024`` and serving load-balanced items.
No need of a sophisticated tool here, a sniplet of shell will do the job for us.
```sh
( echo 127.0.0.1:8000 ; echo 127.0.0.1:8001 ) | gen tcp://127.0.0.1:1024
```

Another way to write this would be:
```sh
refresh-static 127.0.0.1:800{0,1} | gen tcp://127.0.0.1:1024
```

Then, start an echo server bond on these two ports, to simulate the two backends. It runs in background and forks as many there are processors.
```sh
echo-tcp-splice -d -f 127.0.0.1:8000 127.0.0.1:8001
```

Eventually, start a TCP proxy consuming its load-balanced items from ``tcp://127.0.0.1:1024``, and serving the front address ``127.0.0.1:8080``.
```sh
proxy-tcp-splice -d -f 127.0.0.1:8080 tcp://127.0.0.1:1024
```

Here is the result:
```sh
# ps fax
...
12808 ?        Ss     0:00 tcp-echo-splice -d -f 127.0.0.1:8000 127.0.0.1:8001
12809 ?        S      1:05  \_ tcp-echo-splice -d -f 127.0.0.1:8000 127.0.0.1:8001
12810 ?        S      1:00  \_ tcp-echo-splice -d -f 127.0.0.1:8000 127.0.0.1:8001
19682 ?        Ss     0:00 tcp-proxy-splice -d -f 127.0.0.1:8080 tcp://127.0.0.1:1024
19683 ?        Sl     0:00  \_ tcp-proxy-splice -d -f 127.0.0.1:8080 tcp://127.0.0.1:1024
19684 ?        Sl     0:00  \_ tcp-proxy-splice -d -f 127.0.0.1:8080 tcp://127.0.0.1:1024
```

## And now what ?

Obviously, the key idea is to make the backend services register in a directory monitored by the refresher...

## TODO
* fix the proxy-tcp.go that doesn't manage well disconnections from the backend or the client
* Add a (configurable) timeout on connections to prevent idle channels to consume all the proxy's slots.
* Make the maximum number of incoming connection configurable. Now this value is computed as 1/4 of the system limit on open file descriptors. "1/4" because there is at most 4 desciptors for 1 tunnel : incoming, backend, pipe.
* Propose an optional bandwidth limitation for a bit of fair QoS.
* Provide handy refreshers for ...
  * based on [Ganglia][ganglia] monitoring
  * based on [Consul.io][consul] monitoring
  * based on [Redis][redis] monitoring
* Provide a proxy for UDP messages, on the same basis

[ha]: http://www.haproxy.org/
[nn]: http://nanomsg.org/
[zmq]: http://zeromq.org/
[zk]: http://zookeeper.apache.org
[etcd]: https://github.com/coreos/etcd
[oio]: http://openio.io
[go]: http://golang.org
[ganglia]: http://ganglia.sourceforge.net/
[consul]: http://consul.io
[redis]: http://redis.io

