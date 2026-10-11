/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
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
 * Copyright (c) 2026 Qorix
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DLT_TOOLS_POSIX_UDP_TRANSPORT_H
#define DLT_TOOLS_POSIX_UDP_TRANSPORT_H

/**
 * @file posix_udp_transport.h
 * @brief UDP transport for the portable core that sends through an ordinary BSD socket with sendmsg().
 *
 * This is the userspace counterpart of src/qnx/qnx_native_udp_transport.c: the same frames, but every
 * datagram crosses the process boundary into io-sock (on QNX through message passing). Used by
 * ring_forwarder to measure what that hop costs. POSIX only, builds on Linux and QNX.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstring>

#include "dlt/types.h"

namespace dlt
{
namespace tools
{

/**
 * @brief Transport contract (transport.h) implemented with a UDP socket and sendmsg().
 *
 * No socket options are set, like the io-sock module: the stack's defaults, routing table and
 * multicast interface selection apply. Not thread safe, not copyable.
 */
class PosixUdpTransport
{
  public:
    PosixUdpTransport() = default;
    PosixUdpTransport(const PosixUdpTransport&) = delete;
    PosixUdpTransport& operator=(const PosixUdpTransport&) = delete;

    ~PosixUdpTransport()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    /**
     * @brief Opens a UDP socket aimed at `ip:port`.
     *
     * @param ip   Dotted IPv4 address, for example the multicast group 239.255.42.99.
     * @param port UDP destination port.
     * @return true on success.
     */
    bool Open(const char* ip, unsigned port) noexcept
    {
        std::memset(&dst_, 0, sizeof(dst_));
        dst_.sin_family = AF_INET;
        dst_.sin_port = htons(static_cast<unsigned short>(port));
        if (inet_pton(AF_INET, ip, &dst_.sin_addr) != 1)
        {
            return false;
        }
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        return fd_ >= 0;
    }

    /**
     * @brief Sends one datagram with a single sendmsg() call.
     *
     * @return true if the whole datagram was accepted by the stack.
     */
    bool Send(const u8* data, usize size) noexcept
    {
        iovec vector{};
        vector.iov_base = const_cast<u8*>(data);
        vector.iov_len = size;

        msghdr message{};
        message.msg_name = &dst_;
        message.msg_namelen = sizeof(dst_);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;

        ++calls_;
        const ssize_t sent = ::sendmsg(fd_, &message, 0);
        if (sent != static_cast<ssize_t>(size))
        {
            ++errors_;
            return false;
        }
        return true;
    }

    /**
     * @brief Number of sendmsg() calls made so far (one per datagram).
     */
    u64 calls() const noexcept
    {
        return calls_;
    }

    /**
     * @brief Number of sendmsg() calls that failed or sent a short datagram.
     */
    u64 errors() const noexcept
    {
        return errors_;
    }

  private:
    int fd_{-1};
    sockaddr_in dst_{};
    u64 calls_{0U};
    u64 errors_{0U};
};

}  // namespace tools
}  // namespace dlt

#endif  // DLT_TOOLS_POSIX_UDP_TRANSPORT_H
