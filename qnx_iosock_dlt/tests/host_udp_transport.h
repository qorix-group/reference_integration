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

#ifndef DLT_TESTS_HOST_UDP_TRANSPORT_H
#define DLT_TESTS_HOST_UDP_TRANSPORT_H

/**
 * @file host_udp_transport.h
 * @brief Linux-only helpers shared by the host harnesses: a UDP transport and a monotonic clock.
 *
 * This is NOT the QNX transport. It lets the portable core be checked end to end without a QNX
 * target.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstring>

#include "dlt/types.h"

namespace dlt
{
namespace host
{

/**
 * @brief Transport contract implemented with a plain BSD UDP socket.
 */
class LinuxUdpTransport
{
  public:
    /**
     * @brief Opens a UDP socket aimed at `ip:port`.
     *
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
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        return fd_ >= 0;
    }

    /**
     * @brief Sends one datagram.
     */
    bool Send(const u8* data, usize size) noexcept
    {
        const ssize_t sent = sendto(fd_, data, size, 0, reinterpret_cast<const sockaddr*>(&dst_), sizeof(dst_));
        return sent == static_cast<ssize_t>(size);
    }

    ~LinuxUdpTransport()
    {
        if (fd_ >= 0)
        {
            close(fd_);
        }
    }

  private:
    int fd_{-1};
    sockaddr_in dst_{};
};

/**
 * @brief CLOCK_MONOTONIC in nanoseconds.
 */
inline u64 MonotonicNanos() noexcept
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<u64>(now.tv_sec) * 1000000000ULL) + static_cast<u64>(now.tv_nsec);
}

}  // namespace host
}  // namespace dlt

#endif  // DLT_TESTS_HOST_UDP_TRANSPORT_H
