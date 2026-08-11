import socket
import urllib.request
import urllib.parse
from html.parser import HTMLParser
import threading

class TextParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.text = []
        self.ignore = False

    def handle_starttag(self, tag, attrs):
        if tag in ['script', 'style', 'head', 'title', 'meta', 'nav', 'footer']:
            self.ignore = True

    def handle_endtag(self, tag):
        if tag in ['script', 'style', 'head', 'title', 'meta', 'nav', 'footer']:
            self.ignore = False
        elif tag in ['p', 'div', 'br', 'li', 'h1', 'h2', 'h3', 'tr']:
            self.text.append('\n')

    def handle_data(self, data):
        if not self.ignore:
            stripped = data.strip()
            if stripped:
                self.text.append(stripped + ' ')

def fetch_url_text(host, path):
    url = f"https://{host}{path}"
    print(f"[*] Gateway fetching: {url}")
    try:
        req = urllib.request.Request(
            url,
            headers={
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36'
            }
        )
        with urllib.request.urlopen(req, timeout=8) as response:
            html = response.read().decode('utf-8', errors='ignore')
            
        parser = TextParser()
        parser.feed(html)
        raw_text = "".join(parser.text)
        
        # Clean up multiple newlines
        lines = []
        for line in raw_text.split('\n'):
            line_str = line.strip()
            if line_str:
                lines.append(line_str)
        
        content = "\n".join(lines[:120]) # Limit to 120 lines to fit safely in guest buffer
        if not content:
            content = "[Gateway] Page is empty or could not be parsed."
        return content
    except Exception as e:
        return f"[Gateway] Error fetching URL: {str(e)}"

def handle_client(client_socket):
    try:
        request_data = client_socket.recv(4096).decode('utf-8', errors='ignore')
        if not request_data:
            client_socket.close()
            return
            
        lines = request_data.split('\r\n')
        if len(lines) < 1:
            client_socket.close()
            return
            
        req_line = lines[0].split(' ')
        if len(req_line) < 2:
            client_socket.close()
            return
            
        path = req_line[1]
        host = ""
        for line in lines:
            if line.lower().startswith('host:'):
                host = line.split(':', 1)[1].strip()
                break
                
        if not host:
            host = "example.com"
            
        if ':' in host:
            host = host.split(':', 1)[0]
            
        # Fetch clean text
        content = f"--- MECTOV OS GATEWAY ---\nHost: {host}\nPath: {path}\n-------------------------\n\n"
        content += fetch_url_text(host, path)
        
        # Send raw body back to client
        client_socket.sendall(content.encode('utf-8', errors='ignore'))
    except Exception as e:
        print(f"[-] Connection handler error: {e}")
    finally:
        client_socket.close()

def start_gateway():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', 8888))
    server.listen(5)
    print("[*] Mectov Web Gateway Proxy listening on 127.0.0.1:8888...")
    
    while True:
        try:
            client, addr = server.accept()
            t = threading.Thread(target=handle_client, args=(client,))
            t.daemon = True
            t.start()
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"[-] Server accept error: {e}")

if __name__ == "__main__":
    start_gateway()
