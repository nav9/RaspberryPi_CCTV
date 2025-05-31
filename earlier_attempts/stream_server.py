# stream_server.py
from http.server import HTTPServer, BaseHTTPRequestHandler
import os
import time

PORT = 8090
PIPE_FILE = '/tmp/stream.ts'

class StreamHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/':
            self.send_response(200)
            self.send_header('Content-Type', 'video/mp2t')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()

            # Wait for file to exist
            while not os.path.exists(PIPE_FILE):
                print("[INFO] Waiting for pipe to appear...")
                time.sleep(0.5)    
            try:
                with open(PIPE_FILE, 'rb', buffering=0) as f:
                    while True:
                        print("[INFO] Waiting for stream data…")
                        chunk = f.read(1316)  # 7 MPEG-TS packets
                        print(f"[INFO] Read {len(chunk)} bytes")
                        if chunk:
                            self.wfile.write(chunk)
                        else:
                            time.sleep(0.01)                            
            except BrokenPipeError:
                print("[INFO] Client disconnected")
            except Exception as e:
                print(f"[ERROR] Streaming exception: {e}")
        else:
            self.send_cors_error(404)
            
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.end_headers()
            

    def send_cors_error(self, code):
        self.send_response(code)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(f"Error {code}".encode())



if __name__ == '__main__':
    httpd = HTTPServer(('0.0.0.0', PORT), StreamHandler)
    print(f"[INFO] Stream server started on port {PORT}")
    httpd.serve_forever()
