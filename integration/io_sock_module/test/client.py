import socket
import sys
import time

def recv_all(sock, expected_size, buffer_size=65536):
    """Receive all data from socket"""
    data = b""
    while len(data) < expected_size:
        chunk = sock.recv(min(buffer_size, expected_size - len(data)))
        if not chunk:
            break
        data += chunk
    return data

def send_and_receive(sock, data, expected_response_size):
    """Send data and receive response"""
    start_time = time.time()
    sock.sendall(data)
    response = recv_all(sock, expected_response_size)
    end_time = time.time()
    elapsed_time = end_time - start_time
    return len(response), elapsed_time

def tcp_client(server_ip, server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    try:
        sock.connect((server_ip, server_port))
        print(f"Connected to {server_ip}:{server_port}")
        
        time.sleep(0.1)

        # Step 1 & 2: Send 1-byte with values 1, 2, 3, 4 (100 iterations each)
        print("\n=== Test 1: Sending 1-byte commands ===")
        for byte_value in [1, 2, 3, 4]:
            expected_sizes = {1: 1024, 2: 10*1024, 3: 100*1024, 4: 1024*1024}
            expected_size = expected_sizes[byte_value]
            print(f"\nSending 1-byte with value {byte_value} (expecting {expected_size} bytes back):")
            total_time = 0.0
            for i in range(100):
                data = bytes([byte_value])
                received_bytes, elapsed_time = send_and_receive(sock, data, expected_size)
                total_time += elapsed_time
                #print(f"  Iteration {i+1}: Sent {len(data)} byte(s), Received {received_bytes} bytes, Time: {elapsed_time:.6f} sec")
                time.sleep(0.001)  # Short delay between iterations
            avg_time = total_time / 100
            print(f"  Average time for 100 iterations: {avg_time * 1000:.6f} ms")

        # Step 3 & 4: Send 1KB, 10KB, 100KB, 1MB data (100 iterations each)
        # MSB of first byte is 1, 2, 3, 4 to indicate number of bytes transmitted, LSB 0 to expect 1 byte back
        print("\n=== Test 2: Sending larger data with first byte = 0 ===")
        for byte_value, size_kb, size_bytes in [(16, 1, 1024), (32, 10, 10*1024), (48, 100, 100*1024), (64, 1024, 1024*1024)]:
            print(f"\nSending {size_kb}KB data (expecting 1 byte back):")
            total_time = 0.0
            for i in range(100):
                data = bytes([byte_value]) + b'X' * (size_bytes - 1)  # First byte is byte_value
                received_bytes, elapsed_time = send_and_receive(sock, data, 1)
                total_time += elapsed_time
                #print(f"  Iteration {i+1}: Sent {len(data)} bytes, Received {received_bytes} bytes, Time: {elapsed_time:.6f} sec")
                time.sleep(0.001)  # Short delay between iterations
            avg_time = total_time / 100
            print(f"  Average time for 100 iterations: {avg_time * 1000:.6f} ms")


        print("\n=== All tests completed ===")

    except socket.error as e:
        print(f"Socket error: {e}")

    finally:
        sock.close()

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <server_ip>")
        sys.exit(1)

    server_ip = sys.argv[1]
    SERVER_PORT = 12345
    tcp_client(server_ip, SERVER_PORT)
