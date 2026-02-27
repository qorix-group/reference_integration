import socket
import sys
import time

def recv_all(sock, expected_size, buffer_size=65536, server_addr=None):
    """Receive all data from UDP socket"""
    received_size = 0
    while received_size < expected_size:
        chunk, addr = sock.recvfrom(buffer_size)
        if server_addr and addr != server_addr:
            continue  # Ignore packets from unexpected sources
        if not chunk:
            raise ConnectionError(f"Connection closed after receiving {received_size}/{expected_size} bytes")
        received_size += len(chunk)
        #print(f"Received chunk of {len(chunk)} bytes, total received: {received_size}/{expected_size} bytes", flush=True)
    if received_size != expected_size:
        raise ValueError(f"Expected {expected_size} bytes but received {received_size} bytes")
    return received_size

def send_and_receive(sock, data, expected_response_size, server_addr):
    """Send UDP data and receive response"""
    start_time = time.time()
    send_size = len(data)
    max_payload_size = 10 * 1024
    while send_size > 0:
        chunk_size = send_size
        if send_size > max_payload_size:
            chunk_size = max_payload_size
        sock.sendto(data[0:chunk_size], server_addr)
        send_size -= chunk_size
    response = recv_all(sock, expected_response_size, server_addr=server_addr)
    end_time = time.time()
    elapsed_time = end_time - start_time
    if response != expected_response_size:
        raise ValueError(f"Expected {expected_response_size} bytes but got {response} bytes")
    return response, elapsed_time

def udp_client(server_ip, server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_addr = (server_ip, server_port)
    sock.settimeout(5)

    try:
        print(f"UDP client ready for {server_ip}:{server_port}")
        time.sleep(0.1)

        # Initialize dictionaries to accumulate times for each test case
        test1_times = {1: [], 2: [], 3: [], 4: [], 5: []}
        test2_times = {16: [], 32: [], 48: [], 64: [], 80: []}

        # Test configurations
        test1_expected_sizes = {1: 1024, 2: 10*1024, 3: 100*1024, 4: 1024*1024, 5: 1024*1024 * 10}
        test2_configs = [(16, 1, 1024), (32, 10, 10*1024), (48, 100, 100*1024), (64, 1024, 1024*1024), (80, 1024 * 10, 1024*1024 * 10)]

        print("\n=== Running 1 iterations of Test 1 & Test 2 ===")
        print("\nstart time:", time.time())

        # Main loop: 1000 iterations
        for iteration in range(1):
            if (iteration + 1) % 1 == 0:
                print(f"Iteration {iteration + 1}/1", flush=True)

            # Test 1: Send 1-byte with values 1, 2, 3, 4, 5
            for byte_value in [1, 2, 3, 4, 5]:
                expected_size = test1_expected_sizes[byte_value]
                data = bytes([byte_value])
                received_bytes, elapsed_time = send_and_receive(sock, data, expected_size, server_addr)
                test1_times[byte_value].append(elapsed_time)
                time.sleep(0.001)  # 1ms delay to prevent batching
                print(f"Received {received_bytes} bytes", flush=True)

            # Test 2: Send larger data (1KB, 10KB, 100KB, 1MB)
            for byte_value, size_kb, size_bytes in test2_configs:
                data = bytes([byte_value]) + b'X' * (size_bytes - 1)
                received_bytes, elapsed_time = send_and_receive(sock, data, 1, server_addr)
                test2_times[byte_value].append(elapsed_time)
                time.sleep(0.001)  # 1ms delay to prevent batching
                print(f"Sent {size_bytes} bytes", flush=True)

        # Print Test 1 averages
        print("\n=== Test 1 Results: Sending 1-byte commands ===")
        for byte_value in [1, 2, 3, 4, 5]:
            expected_size = test1_expected_sizes[byte_value]
            avg_time = sum(test1_times[byte_value]) / len(test1_times[byte_value])
            print(f"Byte value {byte_value} (expecting {expected_size} bytes): {avg_time * 1000000:.3f} us average")

        # Print Test 2 averages
        print("\n=== Test 2 Results: Sending larger data ===")
        for byte_value, size_kb, size_bytes in test2_configs:
            avg_time = sum(test2_times[byte_value]) / len(test2_times[byte_value])
            print(f"Sending {size_kb}KB (expecting 1 byte): {avg_time * 1000000:.3f} us average")

        print("\nend time:", time.time())
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
    udp_client(server_ip, SERVER_PORT)
