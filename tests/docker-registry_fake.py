#!/usr/bin/python3

import os
import sys
import argparse
import json
import logging
import ssl

from http.server import SimpleHTTPRequestHandler, HTTPServer

logger = logging.getLogger("Fake Docker Registry")


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        logger.info(">>> GET  %s" % self.path)

        if not self.path.startswith('/v2'):
            self.send_response(404)
            self.end_headers()
            return

        # ping call from a client
        if self.path == '/v2/':
            self.send_response(200)
            self.end_headers()
            return

        # /v2/<name>/manifests/<digest>
        # /v2/<name>/blobs/<digest>
        # <digest> = sha256:<hash>
        digest_indx = self.path.find('sha256')
        if digest_indx == -1:
            self.send_response(404)
            self.end_headers()
            return

        digest = self.path[digest_indx:]
        if not digest.startswith('sha256:'):
            self.send_response(400)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({'err': 'Invalid resource hash: ' + digest}).encode())
            return

        hash = digest[len('sha256:'):]
        path = self.path[len('/v2/'):digest_indx - 1]

        # path_elements = self.path[:digest_indx - 1].split('/')
        #
        # factory = path_elements[2]
        # image = path_elements[3]
        # artifact_type = path_elements[4]
        # digest = path_elements[5]
        logger.info(">>> Path: {}; Digest: {}".format(path, digest))

        full_path = os.path.join(self.server.root_dir, path, hash)
        if not os.path.exists(full_path):
            self.send_response(404)
            self.end_headers()

        self.send_response(200)
        self.send_header('Content-type', 'application/vnd.docker.distribution.manifest.v2+json')
        self.end_headers()
        with open(full_path, 'rb') as f:
            while True:
                data = f.read(1024)
                if not data:
                    break
                self.wfile.write(data)


class FakeDockerRegistry(HTTPServer):
    def __init__(self, addr, root_dir):
        super(HTTPServer, self).__init__(server_address=addr, RequestHandlerClass=Handler)
        self.root_dir = root_dir


def main():
    parser = argparse.ArgumentParser(description='Run a fake Docker Registry')
    parser.add_argument('-p', '--port', type=int, help='server port')
    parser.add_argument('-d', '--dir', type=str, help='registry root dir')

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
