#!/usr/bin/python

import sys, urllib2, json, argparse, logging, threading, time, signal

def get_services (host, ns, srvtype):
	host, ns, srvtype = str(host), str(ns), str(srvtype)
	url = 'http://'+host+'/v2.0/cs/'+ns+'/'+srvtype
	logging.debug("Refreshing from [%s]", url)
	cnx = urllib2.urlopen(url)
	try:
		encoded = cnx.read()
		decoded = json.loads(encoded)
		for srv in decoded:
			sys.stdout.write(str(srv['addr']))
			sys.stdout.write("\n")
		sys.stdout.write("\n")
	finally:
		cnx.close()

def main ():
	logging.basicConfig(level=logging.DEBUG)
	parser = argparse.ArgumentParser(description="Refreshes a load-balancing set with information from a Recurrant's metacd.")
	parser.add_argument("--proxy", metavar='proxy', action='store')
	parser.add_argument("ns", metavar='ns')
	parser.add_argument("srvtype", metavar='srvtype')
	args = parser.parse_args()
	running = threading.Event()
	running.set()
	def sighandler_stop (sig,frame):
		running.clear()
	signal.signal(signal.SIGINT, sighandler_stop)
	signal.signal(signal.SIGTERM, sighandler_stop)
	signal.signal(signal.SIGQUIT, sighandler_stop)
	if args.proxy is None:
		args.proxy = "proxy:1234"
	while running.isSet():
		get_services(args.proxy, args.ns, args.srvtype)
		time.sleep(1)
			
if __name__ == '__main__':
	main()

