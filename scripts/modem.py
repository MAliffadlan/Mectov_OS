import socket
import urllib.request
import urllib.error
from html.parser import HTMLParser
import time

class TextParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.text = []
        self.ignore = False

    def handle_starttag(self, tag, attrs):
        if tag in ['script', 'style', 'head', 'title', 'meta']:
            self.ignore = True

    def handle_endtag(self, tag):
        if tag in ['script', 'style', 'head', 'title', 'meta']:
            self.ignore = False
        elif tag in ['p', 'div', 'br', 'li', 'h1', 'h2', 'h3']:
            self.text.append('\n')

    def handle_data(self, data):
        if not self.ignore:
            stripped = data.strip()
            if stripped:
                self.text.append(stripped + ' ')

def fetch_url_text(url):
    if not url.startswith('http'):
        url = 'http://' + url
    try:
        req = urllib.request.Request(
            url, 
            data=None, 
            headers={
                'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_9_3) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/35.0.1916.47 Safari/537.36'
            }
        )
        response = urllib.request.urlopen(req, timeout=5)
        html = response.read().decode('utf-8', errors='ignore')
        
        parser = TextParser()
        parser.feed(html)
        raw_text = "".join(parser.text)
        
        # Clean up multiple newlines
        lines = [line.strip() for line in raw_text.split('\n') if line.strip()]
        return "\n".join(lines[:100]) # Limit to 100 lines so we don't overwhelm the OS
    except Exception as e:
        return f"Error fetching URL: {str(e)}"

def start_modem():
    print("[*] Starting Mectov Serial Modem Proxy...")
    
    # Try connecting to QEMU's serial port (TCP 4444)
    while True:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(('127.0.0.1', 4444))
            print("[+] Connected to Mectov OS (QEMU Serial Port)!")
            break
        except ConnectionRefusedError:
            print("Waiting for Mectov OS to start QEMU...")
            time.sleep(2)

    buffer = ""
    while True:
        try:
            data = s.recv(1024)
            if not data:
                break
            
            # Decode and accumulate
            text = data.decode('ascii', errors='ignore')
            buffer += text
            
            if '\n' in buffer:
                command = buffer.strip()
                buffer = ""
                
                if command.startswith("GET "):
                    url = command[4:].strip()
                    print(f"[*] Mectov OS requested: {url}")
                    
                    content = fetch_url_text(url)
                    
                    # Send response back to Mectov
                    response = f"--- START ---\n{content}\n--- END ---\n"
                    
                    # Send in chunks to avoid overwhelming the serial port buffer
                    for chunk in [response[i:i+64] for i in range(0, len(response), 64)]:
                        s.sendall(chunk.encode('ascii', errors='ignore'))
                        time.sleep(0.01) # Small delay for Mectov OS to process
                    
                    print(f"[+] Sent {len(response)} bytes back to Mectov OS.")

        except Exception as e:
            print(f"[-] Error: {e}")
            break

if __name__ == "__main__":
    start_modem()
