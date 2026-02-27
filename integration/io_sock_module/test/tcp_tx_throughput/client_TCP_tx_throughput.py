import socket
import sys

def tcp_client(server_ip, server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    received_size = 0

    try:
        sock.connect((server_ip, server_port))
        print(f"Connected to {server_ip}:{server_port}")
        prev = 0
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError(f"Connection closed after receiving {received_size} bytes")
            received_size += len(chunk)
            if received_size // (1048576 * 100) > prev:
                prev = received_size // (1048576 * 100)
                print(f"Received {received_size} bytes so far...")

    except socket.error as e:
        print(f"Successfully received {received_size} bytes from server")

    finally:
        sock.close()

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <server_ip>")
        sys.exit(1)

    server_ip = sys.argv[1]
    SERVER_PORT = 12345
    tcp_client(server_ip, SERVER_PORT)
