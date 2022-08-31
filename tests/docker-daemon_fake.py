#!/usr/bin/python3

import os
import sys
import argparse
import json
import logging
import ssl

from http.server import SimpleHTTPRequestHandler, HTTPServer
from socketserver import UnixStreamServer

logger = logging.getLogger("Fake Docker Daemon")


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        logger.info(">>> GET  %s" % self.path)
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'Arch': 'amd64'}).encode())

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

    def address_string(self):
        return ""


class FakeDockerRegistry(HTTPServer):
    def __init__(self, addr, root_dir):
        super(HTTPServer, self).__init__(server_address=addr, RequestHandlerClass=Handler)
        self.root_dir = root_dir


class DockerDaemonMock(UnixStreamServer):
    def __init__(self, addr, root_dir):
        super(UnixStreamServer, self).__init__(server_address=addr, RequestHandlerClass=Handler)
        self.root_dir = root_dir


def main():
    parser = argparse.ArgumentParser(description='Run a fake Docker Daemon')
    parser.add_argument('-p', '--port', type=int, help='server port')
    parser.add_argument('-d', '--dir', type=str, help='docker daemon root dir')
    parser.add_argument('-u', '--unix-sock', type=str, help='docker daemon root dir', default=None)

    args = parser.parse_args()

    try:
        if args.unix_sock:
            httpd = DockerDaemonMock(args.unix_sock, args.dir)
        else:
            httpd = FakeDockerRegistry(('', args.port), args.dir)
        httpd.serve_forever()
    except KeyboardInterrupt:
        httpd.server_close()


if __name__ == "__main__":
    logger.addHandler(logging.StreamHandler(sys.stderr))
    logger.setLevel(logging.INFO)
    sys.exit(main())
