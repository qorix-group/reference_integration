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

For each data size, the `average time over 1000 iterations` is measured (in microseconds), representing the `duration from start of transmission to the end of reception`.

### Test 1: Clinet sends 1-byte command and receives larger data from target (Focuses transmission path of application & io-sock module)

| RX Size      | Application (us) | io-sock module (us) | Difference (us) | io-sock Performance |
|--------------|------------------|---------------------|-----------------|---------------------|
|   1 KB       |      494.621     |      379.877        |    114.744      |  23.20% faster      |
|  10 KB       |      485.978     |      369.119        |    116.859      |  24.05% faster      |
| 100 KB       |      561.010     |      509.584        |     51.426      |   9.17% faster      |
|   1 MB       |     3142.549     |     2537.309        |    605.240      |  19.26% faster      |
|  10 MB       |    83928.562     |    81963.072        |   1965.490      |   2.34% faster      |

Average io-sock performance: 15.60% faster

### Test 2: Client sends larger data and receives 1 byte from target (Focuses reception path of application & io-sock module)

| TX Size      | Application (us) | io-sock module (us) | Difference (us) | io-sock Performance |
|--------------|------------------|---------------------|-----------------|---------------------|
|   1 KB       |      764.279     |      604.316        |    159.963      |  20.93% faster      |
|  10 KB       |      505.916     |      461.927        |     43.989      |   8.69% faster      |
| 100 KB       |      600.514     |      494.736        |    105.778      |  17.61% faster      |
|   1 MB       |     1464.065     |     1291.539        |    172.526      |  11.78% faster      |
|  10 MB       |     9498.362     |     8583.879        |    914.483      |   9.63% faster      |

Average io-sock performance: 13.73% faster

### Note

**TCP:** For data larger than 16KB, it is divided into 16KB chunks and sent.

**UDP:** For data larger than 9KB, it has to be divided into 9KB chunks and sent. The test results for UDP is not captured here.

