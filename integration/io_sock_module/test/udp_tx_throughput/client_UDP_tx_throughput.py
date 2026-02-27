import socket
import sys


def udp_client(server_ip, server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_addr = (server_ip, server_port)
    sock.settimeout(5)
    total_bytes_received = 0

    try:
        # Send a single byte with value 1 to the server (broadcast or direct)
        data = bytes([1])
        sock.sendto(data, server_addr)
        print(f"Sent UDP packet with value 1 to {server_ip}:{server_port}")
        # Receive UDP data infinitely
        prev = 0
        
        while True:
            chunk, addr = sock.recvfrom(65536)
            if addr != server_addr:
                continue  # Ignore packets from unexpected sources
            if not chunk:
                print("No more data received")
                break
            total_bytes_received += len(chunk)
            if total_bytes_received // (1048576 * 100) > prev:
                prev = total_bytes_received // (1048576 * 100)
                print(f"Received {total_bytes_received} bytes so far...")

        print("Total bytes received: ", total_bytes_received)

    except socket.error as e:
        print("Total bytes received: ", total_bytes_received)

    finally:
        sock.close()

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <server_ip>")
        sys.exit(1)

    server_ip = sys.argv[1]
    SERVER_PORT = 12345
    udp_client(server_ip, SERVER_PORT)
