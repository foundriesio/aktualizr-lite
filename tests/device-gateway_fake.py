#!/usr/bin/python3

import os
import sys
import argparse
import json
import logging
import ssl

from http.server import SimpleHTTPRequestHandler, HTTPServer

logger = logging.getLogger("Fake Device Gateway")


class Handler(SimpleHTTPRequestHandler):
    TreehubPrefix = "/treehub/"
    TufRepoPrefix = "/repo/"
    AuthPrefix = "/hub-creds/"
    RegistryAuthPrefix = "/token-auth/"
    RegistryPrefix = "/v2/"
    def do_POST(self):
        self.send_response(200)
        self.end_headers()

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
            self._tuf_dump_headers()
            self.send_response(200)
            self.end_headers()
            self._tuf_serve_metadata_file(path)

    def _tuf_serve_metadata_file(self, uri):
        with open(uri, 'rb') as source:
            while True:
                data = source.read(1024)
                if not data:
                    break
                self.wfile.write(data)

    def _tuf_dump_headers(self):
        headers = {}
        for header_name, header_value in self.headers.items():
            headers[header_name] = header_value

        with open(self.server.headers_file, "w") as f:
            json.dump(headers, f)

    def _ostree_repo(self):
        return self.server.ostree_repo

    def _tuf_repo(self):
        return os.path.join(self.server.tuf_repo, 'repo')


class FakeDeviceGateway(HTTPServer):
    def __init__(self, addr, ostree_repo, tuf_repo, headers_file, mtls=None):
        self.ostree_repo = ostree_repo
        self.tuf_repo = tuf_repo
        self.headers_file = headers_file
        super(HTTPServer, self).__init__(server_address=addr, RequestHandlerClass=Handler)

        if mtls:
            server_key = os.path.join(mtls, "pkey.pem")
            server_cert = os.path.join(mtls, "server.crt")
            ca_cert = os.path.join(mtls, "ca.crt")
            # make Device Gateway check a device/client certificate
            tls_param = ssl.CERT_REQUIRED

            self.socket = ssl.wrap_socket(self.socket, certfile=server_cert, keyfile=server_key, ca_certs=ca_cert,
                                          server_side=True, cert_reqs=tls_param)


def main():
    parser = argparse.ArgumentParser(description='Run a fake Device Gateway')
    parser.add_argument('-p', '--port', type=int, help='server port')
    parser.add_argument('-o', '--ostree', help='OSTree repo directory')
    parser.add_argument('-t', '--tuf-repo', help='TUF repo directory')
    parser.add_argument('-j', '--headers-file', help='File to dump request headers to')
    parser.add_argument('-s', '--mtls', default=None,
                        help='Enables mTLS (HTTP over mTLS) and specifies directory with certs/key')

    args = parser.parse_args()

    try:
        httpd = FakeDeviceGateway(('', args.port), args.ostree, args.tuf_repo, args.headers_file, args.mtls)
        httpd.serve_forever()
    except KeyboardInterrupt:
        httpd.server_close()


if __name__ == "__main__":
    sys.exit(main())
