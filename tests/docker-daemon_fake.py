#!/usr/bin/python3
import io
import os
import sys
import argparse
import json
import logging
import ssl
import tarfile

from http.server import SimpleHTTPRequestHandler, HTTPServer
from socketserver import UnixStreamServer

logger = logging.getLogger("Fake Docker Daemon")

API_VERSION = "1.44"

class Handler(SimpleHTTPRequestHandler):
    def do_HEAD(self):
        logger.info(">>> HEAD  %s" % self.path)
        self.send_response(200)
        self.send_header('Api-Version:', API_VERSION)
        self.end_headers()

    def do_GET(self):
        logger.info(">>> GET  %s" % self.path)
        if self.path.startswith('/_ping'):
            self.send_response(200)
            self.send_header('Content-type', 'text/plain; charset=utf-8')
            self.send_header('Api-Version:', API_VERSION)
            self.end_headers()
        elif self.path.find('/images/json') != -1:
            dockerd_response = []
            try:
                with open(os.path.join(self.server.root_dir, "images.json"), "r") as f:
                    images = json.load(f)
                    for image in images:
                        dockerd_response.append({
                            "RepoDigests": [image]
                        })
            except FileNotFoundError:
                pass
            except Exception as exc:
                logger.error(exc)

            logger.info(">>> sending response  %s" % json.dumps(dockerd_response))
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(dockerd_response).encode())
        else:
            self.send_response(200)
            self.send_header('Api-Version:', API_VERSION)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({'Arch': 'amd64'}).encode())

    def do_POST(self):
        logger.info(">>> POST  %s" % self.path)

        if self.path.find('/images/load') != -1:
            self.post_image()
            return

        if not (self.path.startswith('/containers/prune') or self.path.startswith('/images/prune')):
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

    def post_image(self):
        if 'content-length' in self.headers:
            data_len = int(self.headers.get('content-length', 0))
            with tarfile.open(fileobj=io.BytesIO(self.rfile.read(data_len)), mode="r|") as tar_stream:
                for member in tar_stream:
                    if member.name == "manifest.json":
                        tar_stream.extract(member, self.server.root_dir, set_attrs=False)
        else:
            # the tarred manifest is transferred as chunked stream
            with io.BytesIO() as tarred_load_manifest:
                # extract the load image tarred manifest from the chunked stream passed as a http request body
                while True:
                    line = self.rfile.readline().strip()
                    chunk_length = int(line, 16)
                    if chunk_length == 0:
                        break
                    data = self.rfile.read(chunk_length)
                    tarred_load_manifest.write(data)
                    self.rfile.readline()

                # seek to the beginning of the im-memory tar
                tarred_load_manifest.seek(0)
                # extract and store the image load manifest
                with tarfile.open(fileobj=tarred_load_manifest, mode='r') as t:
                    for member in t:
                        if member.name == "manifest.json":
                            t.extract(member, self.server.root_dir, set_attrs=False)
        try:
            with open(os.path.join(self.server.root_dir, "manifest.json")) as mf:
                lm = json.load(mf)
        except FileNotFoundError:
            self.send_response(400)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({'error': 'no `manifest.json` in the TAR stream'}).encode())
            return
        logger.info(f">>> Got load manifest: {lm[0]}")

        if "x-failure-injection" in lm[0]:
            logger.info(f">>> Failure injection detected: {lm[0]['x-failure-injection']}")
            if lm[0]["x-failure-injection"] == "500":
                self.send_response(500)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({'error': 'Failed to process the load request'}).encode())
                return
            else:
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({'error': 'Some image load failure'}).encode())
                return

        try:
            with open(os.path.join(self.server.root_dir, "images.json"), "r") as f:
                images = json.load(f)
                if not images:
                    images = {}
        except FileNotFoundError:
            images = {}
        except Exception as exc:
            logger.error(exc)

        try:
            image_uri = lm[0]["RepoTags"][0]
        except KeyError:
            self.send_response(400)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({'error': 'missing `RepoTags` in the manifest.json'}).encode())
            return

        # Make the `docker-compose_fake think that the image is installed/pulled/loaded
        images[image_uri] = True
        with open(os.path.join(self.server.root_dir, "images.json"), "w+") as f:
            json.dump(images, f)

        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'stream': image_uri}).encode())


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
