#!/usr/bin/python3

import os
import sys
import argparse
import json
import logging
import ssl

from http.server import SimpleHTTPRequestHandler, HTTPServer

logger = logging.getLogger("Fake Docker Daemon")


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        logger.info(">>> GET  %s" % self.path)

        self.send_response(200)
        self.end_headers()

    def do_POST(self):
        logger.info(">>> POST  %s" % self.path)

        while True:
            line = self.rfile.readline().strip()
            chunk_length = int(line, 16)
            if chunk_length == 0:
                break

            self.rfile.read(chunk_length)
            self.rfile.readline()

        self.send_response(200)
        self.end_headers()


class FakeDockerRegistry(HTTPServer):
    def __init__(self, addr, root_dir):
        super(HTTPServer, self).__init__(server_address=addr, RequestHandlerClass=Handler)
        self.root_dir = root_dir


def main():
    parser = argparse.ArgumentParser(description='Run a fake Docker Daemon')
    parser.add_argument('-p', '--port', type=int, help='server port')
    parser.add_argument('-d', '--dir', type=str, help='docker daemon root dir')

    args = parser.parse_args()

    try:
        httpd = FakeDockerRegistry(('', args.port), args.dir)
        httpd.serve_forever()
    except KeyboardInterrupt:
        httpd.server_close()


if __name__ == "__main__":
    logger.addHandler(logging.StreamHandler(sys.stderr))
    logger.setLevel(logging.INFO)
    sys.exit(main())
