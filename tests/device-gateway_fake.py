#!/usr/bin/python3

import os
import sys
import argparse
import json
import logging

from http.server import SimpleHTTPRequestHandler, HTTPServer

logger = logging.getLogger("Fake Device Gateway")


class Handler(SimpleHTTPRequestHandler):
    TreehubPrefix = "/treehub/"
    TufRepoPrefix = "/repo/"
    AuthPrefix = "/hub-creds/"
    RegistryAuthPrefix = "/token-auth/"
    RegistryPrefix = "/v2/"

    def do_GET(self):
        if self.path.startswith(self.TufRepoPrefix):
            self.tuf_handler()
        if self.path.startswith(self.TreehubPrefix):
            self.treehub_handler()
        if self.path.startswith(self.AuthPrefix):
            self.auth_handler()
        if self.path.startswith(self.RegistryAuthPrefix):
            self.registry_auth_handler()
        if self.path.startswith(self.RegistryPrefix):
            self.registry_handler()

    def treehub_handler(self):
        logger.info("Treehub: GET request %s" % self.path)
        path = os.path.join(self._ostree_repo(), self.path[len(self.TreehubPrefix):])
        if os.path.exists(path):
            self.send_response_only(200)
            self.end_headers()
            with open(path, 'rb') as source:
                while True:
                    data = source.read(1024)
                    if not data:
                        break
                    self.wfile.write(data)
        else:
            self.send_response_only(404)
            self.end_headers()

    def auth_handler(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'Username': 'foo', 'Secret': 'bar'}).encode())

    def registry_auth_handler(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'Username': 'foo', 'Secret': 'bar'}).encode())

    def registry_handler(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'Username': 'foo', 'Secret': 'bar'}).encode())

    def tuf_handler(self):
        logger.info("TUF: GET request %s" % self.path)
        path = os.path.join(self._tuf_repo(), self.path[1:])
        if not os.path.exists(path):
            self.send_response(404)
            self.end_headers()
        else:
            self.send_response(200)
            self.end_headers()
            self._serve_simple(path)

    def _serve_simple(self, uri):
        with open(uri, 'rb') as source:
            while True:
                data = source.read(1024)
                if not data:
                    break
                self.wfile.write(data)

    def _ostree_repo(self):
        return self.server.ostree_repo

    def _tuf_repo(self):
        return os.path.join(self.server.tuf_repo, 'repo')


class FakeDeviceGateway(HTTPServer):
    def __init__(self, addr, ostree_repo, tuf_repo):
        self.ostree_repo = ostree_repo
        self.tuf_repo = tuf_repo
        super(HTTPServer, self).__init__(server_address=addr, RequestHandlerClass=Handler)


def main():
    parser = argparse.ArgumentParser(description='Run a fake Device Gateway')
    parser.add_argument('-p', '--port', type=int, help='server port')
    parser.add_argument('-o', '--ostree', help='OSTree repo directory')
    parser.add_argument('-t', '--tuf-repo', help='TUF repo directory')

    args = parser.parse_args()

    try:
        httpd = FakeDeviceGateway(('', args.port), args.ostree, args.tuf_repo)
        httpd.serve_forever()
    except KeyboardInterrupt:
        httpd.server_close()


if __name__ == "__main__":
    sys.exit(main())
