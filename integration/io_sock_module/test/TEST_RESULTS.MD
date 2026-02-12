# Test Environment

### **Target:** QEMU_x86_64 QNX8.0 with,

a) `custom io-sock module:`
This module loads during system startup and listens on TCP port 12345. When a host‑side client connects, it exchanges data of varying sizes.

Reception: Incoming data is read from the `mbuf`, copied into a static RX buffer, and the `mbuf` is then released.

Transmission: After receiving data, the module inspects the first byte to determine the transmit size, copies the corresponding data from a pre‑initialized static TX buffer into an `mbuf`, and initiates transmission.

b) `application:`
Once the system is running, the test application is downloaded and executed on the target. It communicates with the host-side client via the standard QNX socket stack (libsocket -> io-sock), listening on TCP port 5000. After a connection is established, it exchanges data of various sizes.
For both RX and TX paths, the application uses static buffers directly within the API functions—no additional copying is performed.


### **Host:** x86_64 Ubuntu 24.04
A `client.py` script connects to the target, exchanges data of different sizes, and measures the elapsed time from the start of transmission to the end of reception.

# Tests

For each data size, the `average time over 100 iterations` is measured (in microseconds), representing the `duration from start of transmission to the end of reception`.

### Test 1: Clinet sends 1-byte command and receives larger data from target (Focuses transmission path of application & io-sock module)

| RX Size      | Application (us) | io-sock module (us) | Difference (us) | io-sock Performance |
|--------------|------------------|---------------------|-----------------|---------------------|
|   1 KB       |  346.243         |  322.807            |     23.436      |   6.8% faster       |
|  10 KB       |  290.580         |   80.321            |    210.259      |  72.4% faster       |
| 100 KB       |  443.578         |  103.626            |    339.952      |  76.6% faster       |
|   1 MB       | 2592.273         | 1715.813            |    876.460      |  33.8% faster       |

Average io-sock performance: 47.4% faster

### Test 2: Client sends larger data and receives 1 byte from target (Focuses reception path of application & io-sock module)

| TX Size      | Application (us) | io-sock module (us) | Difference (us) | io-sock Performance |
|--------------|------------------|---------------------|-----------------|---------------------|
|   1 KB       |  287.790         |   44.239            |    243.551      |  84.6% faster       |
|  10 KB       |  293.660         |   39.868            |    253.792      |  86.4% faster       |
| 100 KB       |  432.916         |   70.674            |    362.242      |  83.7% faster       |
|   1 MB       | 1188.154         |  241.880            |    946.274      |  79.6% faster       |

Average io-sock performance: 83.58% faster

## Summary

The overall performance of io-sock module is much better and the solution `Option 2` (described in README.MD) can be considered.
