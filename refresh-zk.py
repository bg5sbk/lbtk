#!/usr/bin/python

import sys, time, threading, logging, signal
import zookeeper

running = threading.Event()

def sighandler_stop (sig,frame):
	running.clear()

def children_content (zh, base, ids):
	for cid in ids:
		data = zookeeper.get(zh, base+'/'+cid)
		yield str(data[0])

def watch (zh, base):
	change = threading.Event()
	def on_change (a0, a1, a2, a3):
		change.set()
	change.set()
	while running.isSet():
		change.wait(1)
		if not running.isSet():
			return
		if not change.isSet():
			continue
		change.clear()
		children = zookeeper.get_children(zh, base, on_change)
		for c in children_content(zh, base, children):
			sys.stdout.write(c)
			sys.stdout.write("\n")
		sys.stdout.write("\n")

def main ():
	logging.basicConfig(level=logging.DEBUG)
	signal.signal(signal.SIGINT, sighandler_stop)
	signal.signal(signal.SIGTERM, sighandler_stop)
	signal.signal(signal.SIGQUIT, sighandler_stop)
	running.set()
	url = sys.argv[1]
	basedir = sys.argv[2]
	zh = zookeeper.init(url, None, 1000)
	assert(zh is not None)

	while running.isSet():
		try:
			watch(zh, basedir)
		except zookeeper.NoNodeException as e:
			logging.debug("Node not found : %s", e)
			time.sleep(1)

if __name__ == "__main__":
	main()

