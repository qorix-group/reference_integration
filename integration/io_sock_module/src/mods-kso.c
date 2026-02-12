/********************************************************************************
 * Copyright (c) 2025 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

/*
 * io-sock sample module - a simple module (kso - kernel socket) that creates a TCP server socket, accepts connection, receive data and send some data back to the client.
 */
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/kthread.h>

#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/mbuf.h>
#include <netinet/in.h>
#include <sys/mutex.h>

#include <qnx/qnx_modload.h>

#define DEMO_PORT 12345
#define BACKLOG   4

int mod_ver = IOSOCK_VERSION_CUR;

SYSCTL_INT(_qnx_module, OID_AUTO, kso, CTLFLAG_RD, &mod_ver, 0,
            "Version");

/* Thread control variables */
static struct thread *kso_thread = NULL;
static volatile int kso_thread_run = 0;
static volatile int kso_accept_pending = 0;
static struct socket *kso_srv_socket = NULL;
static struct socket *kso_accepted_socket = NULL;
static struct mtx kso_mtx;
static char recv_buf[1024 * 1024];  /* 1MB receive buffer for testing */
static char send_data[1024 * 1024];  /* Send buffer filled with pattern data */

/* 
* QNX does not expose UIO definition, so it is defined here according to FreeBSD.
* It is a workaround now, since soreceive() requires a non-NULL uio even for mbuf mode, otherwise soreceive() stuck forever.
* It is also mentioned in the FreeBSD man page that uio must not be null even for mbuf mode (where uio_resid is used).
*/
enum uio_rw { UIO_READ_E = 0, UIO_WRITE_E = 1 };
enum uio_seg { UIO_USERSPACE_E = 0, UIO_SYSSPACE_E = 1 };

struct uio {
    struct iovec *uio_iov;    /* scatter/gather list */
    int          uio_iovcnt;  /* length of scatter/gather list */
    off_t        uio_offset;  /* offset in target object */
    ssize_t      uio_resid;   /* remaining bytes to process */
    enum uio_seg uio_segflg;  /* address space */
    enum uio_rw  uio_rw;      /* operation */
    struct thread *uio_td;    /* owner */
};


/**
 * MTX_SYSINIT() - Initialize a mutex at system startup
 * Declares and initializes a mutex during kernel initialization
 * @param name: Variable name for the mutex
 * @param mtx: Pointer to the mutex structure
 * @param desc: Description string for the mutex
 * @param type: Mutex type (MTX_DEF for default, MTX_SPIN for spin mutex)
 */
MTX_SYSINIT(kso_mtx, &kso_mtx, "kso module mutex", MTX_DEF);

/**
 * Socket upcall handler for listening socket.
 * Called automatically when a new connection is available.
 * Wakes up the worker thread to process the connection.
 * 
 * This function is invoked by the network stack when a connection
 * completes the TCP handshake and is placed in the socket's
 * completion queue (sol_comp). It runs in interrupt context or
 * network thread context, so it should be quick and non-blocking.
 * 
 * @param so Listening socket with pending connection
 * @param arg User-provided argument (unused)
 * @param waitflag Wait flags indicating calling context
 * @return SU_OK on success
 */
static int
kso_accept_upcall(struct socket *so, void *arg, int waitflag)
{
    //printf("kso: accept upcall triggered - new connection available!\n");
    
    /* Signal that an accept event is pending */
    /**
     * mtx_lock() - Acquire a mutex lock
     * Blocks until the mutex is available if held by another thread
     * @param mtx: Pointer to the mutex to lock
     */
    mtx_lock(&kso_mtx);
    kso_accept_pending = 1;
    
    /**
     * wakeup() - Wake up all threads sleeping on the specified wait channel
     * Threads sleeping via msleep() or tsleep() on this channel are awakened
     * @param chan: Wait channel identifier (arbitrary memory address)
     */
    wakeup((const void *)&kso_thread_run);
    
    /**
     * mtx_unlock() - Release a mutex lock
     * @param mtx: Pointer to the mutex to unlock
     */
    mtx_unlock(&kso_mtx);
    
    return (SU_OK);
}



/**
 * Process accepted connections.
 * Called by worker thread when accept upcall signals new connections.
 * Accepts one connection and registers receive upcall on it.
 * 
 * @param listen_so Listening socket
 * @return 0 on success, error code otherwise
 */
static int
kso_process_accepts(struct socket *listen_so)
{
    struct socket *accepted_so;
    struct sockaddr *peer = NULL;
    int error;
    /**
     * SOLISTEN_LOCK() - Acquire the listening socket's lock
     * Protects access to the socket's connection queue (sol_comp)
     * @param so: Listening socket to lock
     */
    SOLISTEN_LOCK(listen_so);
    
    /* Check if there's a pending connection in the completion queue */
    if (TAILQ_EMPTY(&listen_so->sol_comp)) {
        SOLISTEN_UNLOCK(listen_so);
        //printf("kso: no pending connections\n");
        return EAGAIN;
    }
    
    /* Remove the first completed connection from the queue */
    accepted_so = TAILQ_FIRST(&listen_so->sol_comp);
    TAILQ_REMOVE(&listen_so->sol_comp, accepted_so, so_list);
    if (listen_so->sol_qlen > 0)
        listen_so->sol_qlen--;
    
    /**
     * SOLISTEN_UNLOCK() - Release the listening socket's lock
     * @param so: Listening socket to unlock
     */
    SOLISTEN_UNLOCK(listen_so);
    
    /**
     * soaccept() - Complete the accept operation for a socket
     * Finalizes the connection and retrieves the peer address
     * @param so: Socket to accept
     * @param peer: Output pointer to receive peer's sockaddr (allocated by kernel)
     * @return 0 on success, error code otherwise
     */
    error = soaccept(accepted_so, &peer);
    if (error != 0) {
        //printf("kso: accept failed with error %d\n", error);
        soclose(accepted_so);
        return error;
    }

    //printf("kso: accepted connection successfully\n");
    if (peer != NULL) {
        free(peer, M_TEMP);
        peer = NULL;
    }

    /* Store accepted socket globally */
    kso_accepted_socket = accepted_so;
    
    /* Set socket buffer sizes to 1MB */
    int rcvbuf_size = 1024 * 1024;  /* 1MB */
    int sndbuf_size = 1024 * 1024;  /* 1MB */
    
    //printf("kso: setting socket buffer sizes to 1MB\n");
    /**
     * soreserve() - Reserve socket buffer space
     * @param so: Socket to configure
     * @param sndcc: Send buffer size in bytes
     * @param rcvcc: Receive buffer size in bytes
     * @return 0 on success, error code otherwise
     */
    error = soreserve(accepted_so, sndbuf_size, rcvbuf_size);
    if (error != 0) {
        //printf("kso: failed to set buffer sizes, error %d (continuing anyway)\n", error);
    }
        
    return 0;
}

/**
 * Process received data on the accepted connection.
 * Continuously receives data and sends responses until connection closes or module unloads.
 * Response size is determined by the first byte of each received message.
 * 
 * @return 0 on success, error code otherwise
 */
static int
kso_process_receive(void)
{
    struct socket *accepted_so = kso_accepted_socket;
    struct mbuf *recv_mbuf = NULL;
    struct uio ruio;
    struct mbuf *control = NULL;
    struct mbuf *send_mbuf = NULL;
    int flags = 0;
    int recv_len;
    int send_len = 1;
    int error;
    int received_size = 0;
    int expected_size = 0;
    
    if (accepted_so == NULL) {
        //printf("kso: no accepted socket to receive from\n");
        return EINVAL;
    }

    /* Continuous receive/transmit loop */
    //printf("kso: entering receive/transmit loop\n");
    /* Fill with a pattern (incrementing bytes) */
    for (int i = 0; i < sizeof(send_data); i++) {
        send_data[i] = (char)(i & 0xFF);
    }
    while (kso_thread_run) {
        recv_mbuf = NULL;
        control = NULL;
        flags = 0;
        
        /* This is needed for soreceive() even in mbuf mode */
        memset(&ruio, 0, sizeof(ruio));
        ruio.uio_resid = sizeof(recv_buf);

        /**
         * soreceive() - Receive data from a socket using mbuf-only mode
         * @param so: Socket to receive from
         * @param psa: Output pointer for source address (NULL if not needed)
         * @param uio: uio structure for data reception (must not be NULL, even in mbuf mode)
         * @param mp0: Output pointer for mbuf chain containing received data
         * @param controlp: Output pointer for control message (NULL if not needed)
         * @param flagsp: Pointer to flags (MSG_DONTWAIT, MSG_PEEK, etc.)
         * @return 0 on success, error code otherwise
         * The mbuf chain is allocated by the kernel and returned in mp0.
         */
        //printf("kso: receiving data...\n");
        error = soreceive(accepted_so, NULL, &ruio, &recv_mbuf, &control, &flags);
        if (error != 0) {
            //printf("kso: receive failed with error %d\n", error);
            break;
        }
    
        /* Clean up control message if any */
        if (control != NULL) {
            /**
             * m_freem() - Free an mbuf chain
             * Releases all mbufs in the chain back to the mbuf pool
             * @param m: Mbuf chain to free
             */
            m_freem(control);
            control = NULL;
        }
        
        /* Check if we received any data */
        if (recv_mbuf == NULL) {
            //printf("kso: received NULL mbuf (connection closed by client)\n");
            break;
        }
    
        /**
         * m_length() - Calculate total length of mbuf chain
         * @param m: Mbuf chain
         * @param last: Output pointer for last mbuf in chain (can be NULL)
         * @return Total number of bytes in the mbuf chain
         */
        recv_len = m_length(recv_mbuf, NULL);
        //printf("kso: mbuf chain length = %d bytes\n", recv_len);
        
        if (recv_len <= 0) {
            //printf("kso: received zero-length data (connection closed)\n");
            m_freem(recv_mbuf);
            break;
        }
    
        /* Copy data from mbuf to buffer */
        /**
         * m_copydata() - Copy data from an mbuf chain to a buffer
         * @param m: Source mbuf chain
         * @param off: Offset into the mbuf chain to start copying from
         * @param len: Number of bytes to copy
         * @param cp: Destination buffer
         */
        m_copydata(recv_mbuf, 0, recv_len, recv_buf);
        
        //printf("kso: received %d bytes, first byte = %u\n", recv_len, recv_buf[0]);
        
        /* Free the receive mbuf */
        m_freem(recv_mbuf);
        recv_mbuf = NULL;
        
        /* Determine response size and number of bytes to receive based on first byte */
        if (received_size == 0)
        {
            /* Check MSB of first byte to determine expected size */
            switch (recv_buf[0] & 0xF0) {
            case 0x00:
                expected_size = 1;              /* 1 byte */
                break;
            case 0x10:
                expected_size = 1024;           /* 1KB */
                break;
            case 0x20:
                expected_size = 10 * 1024;      /* 10KB */
                break;
            case 0x30:
                expected_size = 100 * 1024;     /* 100KB */
                break;
            case 0x40:
                expected_size = 1024 * 1024;    /* 1MB */
                break;
            default:
                expected_size = 1;              /* Default to 1 byte for unknown values */
                break;
            }

            /* Check LSB of first byte to determine transmit size */
            switch (recv_buf[0] & 0x0F) {
            case 0:
                send_len = 1;              /* 1 byte */
                break;
            case 1:
                send_len = 1024;           /* 1KB */
                break;
            case 2:
                send_len = 10 * 1024;      /* 10KB */
                break;
            case 3:
                send_len = 100 * 1024;     /* 100KB */
                break;
            case 4:
                send_len = 1024 * 1024;    /* 1MB */
                break;
            default:
                send_len = 1;              /* Default to 1 byte for unknown values */
                break;
            }
        }

        received_size += recv_len;
        if (received_size < expected_size) {
            continue; // Go back to receive more data until we reach the expected size
        }
        else
        {
            //printf("kso: received expected total of %d bytes\n", received_size);
            received_size = 0; // Reset for next message
        }
        
        //printf("kso: preparing to send %d bytes\n", send_len);
        // For less than 1MB responses, we can use a single mbuf (with cluster if needed). For 1MB responses, we use m_gethdr which can automatically build mbuf chains.
        if (send_len < (1024 * 1024))
        {
            /**
             * m_get() - Allocate a single mbuf
             * @param how: M_WAITOK (can sleep) or M_NOWAIT (don't sleep)
             * @param type: Mbuf type (MT_DATA for data, MT_HEADER for packet header)
             * @return Pointer to allocated mbuf or NULL on failure
             */
            send_mbuf = m_get(M_WAITOK, MT_DATA);
            if (send_mbuf == NULL) {
                //printf("kso: failed to allocate mbuf for send\n");
                error = ENOBUFS;
                break;
            }
        
            /* Check if we need a cluster for large data */
            if (send_len > MLEN) {
                /**
                 * m_cljget() - Attach a cluster to an mbuf
                 * @param m: Mbuf to attach cluster to
                 * @param how: M_WAITOK or M_NOWAIT
                 * @param size: Cluster size (MCLBYTES, MJUMPAGESIZE, etc.)
                 * @return Pointer to mbuf on success, NULL on failure
                 */
                m_cljget(send_mbuf, M_WAITOK, MCLBYTES);
                if ((send_mbuf->m_flags & M_EXT) == 0) {
                    //printf("kso: failed to allocate mbuf cluster\n");
                    m_freem(send_mbuf);
                    error = ENOBUFS;
                    break;
                }
            }
    
            /**
             * m_copyback() - Copy data into an mbuf chain, extending if necessary
             * @param m: Mbuf chain
             * @param off: Offset to copy to
             * @param len: Length of data to copy
             * @param buf: Source buffer
             */
            m_copyback(send_mbuf, 0, send_len, send_data);
            send_mbuf->m_len = send_len;
            send_mbuf->m_pkthdr.len = send_len;
        }
        else /* >= 1MB */
        {
            /**
             * m_gethdr() - Allocate an mbuf with packet header
             * @param how: M_WAITOK (can sleep) or M_NOWAIT (don't sleep)
             * @param type: Mbuf type (MT_DATA for data)
             * @return Pointer to allocated mbuf or NULL on failure
             */
            send_mbuf = m_gethdr(M_WAITOK, MT_DATA);
            if (send_mbuf == NULL) {
                //printf("kso: failed to allocate mbuf for send\n");
                error = ENOBUFS;
                break;
            }

            /**
             * Initialize mbuf lengths to zero before m_copyback
             */
            send_mbuf->m_len = 0;
            send_mbuf->m_pkthdr.len = 0;
        
            /**
             * m_copyback() - Copy data into an mbuf chain, extending if necessary
             * Automatically builds mbuf chains for large data by allocating
             * additional mbufs and clusters as needed
             * @param m: Mbuf chain (will be extended as needed)
             * @param off: Offset to copy to (0 to start from beginning)
             * @param len: Length of data to copy
             * @param buf: Source buffer
             */
            m_copyback(send_mbuf, 0, send_len, send_data);
        }
        
        /**
         * sosend() - Send data through a socket using mbuf
         * @param so: Socket to send through
         * @param addr: Destination address (NULL for connected sockets)
         * @param uio: uio structure (NULL to use mbuf mode)
         * @param top: mbuf chain to send (consumed by sosend on success or error)
         * @param control: Control message mbuf (NULL if not needed)
         * @param flags: Send flags (MSG_DONTWAIT, MSG_OOB, etc.)
         * @param td: Thread performing the send operation
         * @return 0 on success, error code otherwise
         * 
         * The mbuf chain is freed by sosend regardless of success or failure
         */
        //printf("kso: sending response (%d bytes)...\n", send_len);
        error = sosend(accepted_so, NULL, NULL, send_mbuf, NULL, 0, curthread);
        if (error != 0) {
            //printf("kso: send failed with error %d\n", error);
            /* sosend already freed the mbuf */
            break;
        }
        
        //printf("kso: response sent successfully\n");
    }
    
    /* Cleanup */
    //printf("kso: exiting receive/transmit loop\n");
    
    /**
     * soclose() - Close a socket and release resources
     * Decrements reference count and frees socket if no more references
     * @param so: Socket to close
     * @return 0 on success, error code otherwise
     */
    soclose(accepted_so);
    kso_accepted_socket = NULL;
    //printf("kso: connection closed\n");
    
    return error;
}

/**
 * Kernel thread function that runs continuously.
 * Waits for one connection, processes it, and then exits.
 * 
 * @param arg Unused argument pointer
 */
static void
kso_thread_func(void *arg)
{
    int error;
    struct thread *td = curthread;
    struct ucred *cr = td->td_ucred;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(DEMO_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    /**
     * socreate() - Create a new socket
     * @param dom: Protocol family (PF_INET for IPv4)
     * @param aso: Output pointer to receive the created socket
     * @param type: Socket type (SOCK_STREAM for TCP, SOCK_DGRAM for UDP)
     * @param proto: Protocol number (IPPROTO_TCP, IPPROTO_UDP, or 0 for default)
     * @param cred: Credentials of the creating process/thread
     * @param td: Thread descriptor of the caller
     * @return 0 on success, error code otherwise
     */
    //printf("kso: creating socket\n");
    error = socreate(PF_INET, &kso_srv_socket, SOCK_STREAM, IPPROTO_TCP, cr, td);
    if (error) {
        //printf("kso: socket creation failed: %d\n", error);
        return;
    }

    /**
     * sobind() - Bind a socket to a local address
     * Associates the socket with a specific IP address and port number
     * @param so: Socket to bind
     * @param nam: sockaddr structure containing the address to bind to
     * @param td: Thread descriptor of the caller
     * @return 0 on success, error code otherwise (e.g., EADDRINUSE if port already in use)
     */
    //printf("kso: binding socket to port %d\n", DEMO_PORT);
    error = sobind(kso_srv_socket, (struct sockaddr *)&sin, td);
    if (error) {
        //printf("kso: bind failed: %d\n", error);
        soclose(kso_srv_socket);
        kso_srv_socket = NULL;
        return;
    }
    
    /**
     * solisten() - Mark a socket as a listening socket
     * Prepares the socket to accept incoming connections
     * @param so: Socket to set in listening mode
     * @param backlog: Maximum length of pending connection queue
     * @param td: Thread descriptor of the caller
     * @return 0 on success, error code otherwise
     */
    //printf("kso: listening on socket (backlog=%d)\n", BACKLOG);
    error = solisten(kso_srv_socket, BACKLOG, td);
    if (error) {
        //printf("kso: listen failed: %d\n", error);
        soclose(kso_srv_socket);
        kso_srv_socket = NULL;
        return;
    }
    
    /**
     * solisten_upcall_set() - Register an upcall function for listen socket events
     * The upcall is invoked when new connections arrive in the completion queue
     * @param so: Listening socket
     * @param func: Upcall function pointer (or NULL to clear)
     * @param arg: User-defined argument passed to the upcall function
     */
    //printf("kso: registering accept upcall\n");
    solisten_upcall_set(kso_srv_socket, kso_accept_upcall, NULL);

    /* Wait for connection event - sleep until accept upcall wakes us */
    //printf("kso: waiting for incoming connection...\n");
    mtx_lock(&kso_mtx);
    
    while (!kso_accept_pending && kso_thread_run) {
        /**
         * msleep() - Sleep on a wait channel with a mutex
         * Atomically releases the mutex and puts the thread to sleep
         * Reacquires the mutex before returning
         * @param chan: Wait channel (arbitrary address used as identifier)
         * @param mtx: Mutex to release during sleep (reacquired on wakeup)
         * @param priority: Sleep priority (PWAIT, PRIBIO, etc.)
         * @param wmesg: Wait message (shown in process status)
         * @param timo: Timeout in ticks (0 for no timeout, use hz for seconds)
         * @return 0 if woken by wakeup(), EWOULDBLOCK on timeout, or other error
         */
        msleep((const void *)&kso_thread_run, &kso_mtx, PWAIT, "kso_wait", hz * 30);
    }
    
    kso_accept_pending = 0;
    mtx_unlock(&kso_mtx);
    
    /* Accept the connection */
    if (kso_thread_run && kso_srv_socket != NULL) {
        error = kso_process_accepts(kso_srv_socket);
        if (error == 0) {
            //printf("kso: connection accepted, starting receive/transmit loop...\n");
        } else {
            //printf("kso: connection accept failed: %d\n", error);
            goto cleanup;
        }
    } else {
        goto cleanup;
    }
    
    /* Process received data continuously */
    if (kso_thread_run && kso_accepted_socket != NULL) {
        error = kso_process_receive();
        //printf("kso: receive/transmit loop exited\n");
    }

cleanup:

    /* Cleanup - clear upcall and close listening socket */
    //printf("kso: thread cleanup - clearing upcalls and closing socket\n");
    if (kso_srv_socket != NULL) {
        /* Clear the upcall before closing */
        solisten_upcall_set(kso_srv_socket, NULL, NULL);
        soclose(kso_srv_socket);
        kso_srv_socket = NULL;
    }

    //printf("kso: thread finished\n");
    
    /**
     * kthread_exit() - Terminate the current kernel thread
     * Does not return - thread is removed from the system
     */
    kthread_exit();
}

/**
 * Module event handler for MOD_LOAD and MOD_UNLOAD events.
 * 
 * @param mod Module handle
 * @param what Event type (MOD_LOAD, MOD_UNLOAD, etc.)
 * @param arg Event-specific argument (unused)
 * @return 0 on success, error code otherwise
 */
static int
module_event_handler(module_t mod, int what, void *arg __unused)
{
    int error = 0;
    
    switch (what) {
    case MOD_LOAD:
        //printf("kso: MOD_LOAD - creating kernel thread\n");
        kso_thread_run = 1;
        
        /**
         * kthread_add() - Create a new kernel thread
         * Creates and starts a kernel thread that runs in kernel space
         * @param func: Function to execute in the new thread
         * @param arg: Argument to pass to the thread function
         * @param p: Process to attach thread to (NULL for system process)
         * @param newtdp: Output pointer to receive thread descriptor (or NULL)
         * @param flags: Creation flags (0 for default)
         * @param pages: Number of stack pages (0 for default)
         * @param fmt: printf-style format string for thread name
         * @return 0 on success, error code otherwise
         */
        error = kthread_add(kso_thread_func, NULL, NULL, &kso_thread,
                           0, 0, "kso_worker");
        if (error) {
            //printf("kso: Failed to create kernel thread: %d\n", error);
            kso_thread_run = 0;
            return error;
        }
        
        //printf("kso: MOD_LOAD complete - thread started\n");
        break;
        
    case MOD_UNLOAD:
        //printf("kso: MOD_UNLOAD - stopping kernel thread\n");
        
        if (kso_thread != NULL) {
            /* Signal thread to stop */
            kso_thread_run = 0;
            
            /**
             * wakeup() - Wake up threads sleeping on a wait channel
             * All threads sleeping on the specified channel are awakened
             * @param chan: Wait channel (same address used in msleep/tsleep)
             */
            mtx_lock(&kso_mtx);
            wakeup((const void *)&kso_thread_run);
            mtx_unlock(&kso_mtx);
            
            /**
             * pause() - Sleep for a specified time interval
             * Simple blocking sleep without a wakeup channel
             * @param wmesg: Wait message string (shown in process status)
             * @param timo: Timeout in ticks (hz = 1 second)
             * @return 0 on timeout, error code if interrupted
             */
            //printf("kso: waiting for thread to exit\n");
            pause("kso_stop", hz * 2);  /* Wait up to 2 seconds */
            
            kso_thread = NULL;
        }
        
        //printf("kso: MOD_UNLOAD complete\n");
        break;
        
    default:
        //printf("kso: Unsupported module event:%d\n", what);
        return (EOPNOTSUPP);
    }

    return (0);
}

static moduledata_t modkso_moduledata = {
    "module_kso",
    module_event_handler,
    NULL
};

MODULE_VERSION(modkso, 1);
DECLARE_MODULE(modkso, modkso_moduledata, SI_SUB_PSEUDO, SI_ORDER_ANY);

struct _iosock_module_version iosock_module_version =
    IOSOCK_MODULE_VER_SYM_INIT;

static void
modkso_uninit(void *arg)
{
}
/*
 * If code is added to kso_uninit, then SI_SUB_DUMMY needs to be
 * changed to SI_SUB_DRIVERS.  With SI_SUB_DUMMY, kso_uninit does
 * not get called.
 */
SYSUNINIT(modkso_uninit, SI_SUB_DUMMY, SI_ORDER_ANY, modkso_uninit, NULL);
