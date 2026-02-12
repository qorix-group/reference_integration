# QNX 8.0 custom io-sock Module

This proof-of-concept explores methods to enhance TCP/IP communication performance through a custom io-sock module implementation. The core concept leverages shared memory for data exchange between the application and the custom io-sock module, eliminating one data copy operation that currently occurs during IPC (message passing) between libsocket and io-sock.

## Explored Options

### Option 1: Direct C++ mw:com Integration

This approach attempted to integrate the existing C++ mw:com implementation directly into the custom io-sock module. However, this proved infeasible because the existing implementation relies on C++ STL and APIs that are incompatible with the io-sock module environment, which requires the use of kernel-level interfaces.

### Option 2: Shared Memory Architecture (Feasible)

**Control Flow:**
```
application -----> libcustom_socket ------> io-sock (custom module)
```
The `libcustom_socket` library utilizes QNX message passing interfaces to transmit shared memory handles and socket information.

**Data Flow:**
```
application <-------> shared memory <----------> io-sock (custom module)
```

This architecture is viable: the application owns the shared memory, which the io-sock module accesses for data operations. Shared memory access within the io-sock module has been validated and functions correctly.

## Current Implementation Status

The `libcustom_socket` library and associated IPC communication mechanisms remain unimplemented, limiting further exploration of this approach.

A simplified io-sock module with a test application has been developed to measure communication performance with/without libsocket. For test setup and detailed performance metrics, refer to `test/TEST_RESULTS.MD`.

The source code is available under src & test folders.

## References

a. https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.io_sock/topic/about.html

b. https://man.freebsd.org/cgi/man.cgi?query=socket&apropos=0&sektion=9&manpath=FreeBSD+13.5-STABLE&format=html

c. https://man.freebsd.org/cgi/man.cgi?query=mbuf&apropos=0&sektion=9&manpath=FreeBSD+13.5-STABLE&format=html

d. https://man.freebsd.org/cgi/man.cgi?query=uio&apropos=0&sektion=9&manpath=FreeBSD+13.5-STABLE&format=html
