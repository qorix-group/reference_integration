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

#ifndef DLT_TYPES_H
#define DLT_TYPES_H

/**
 * @file types.h
 * @brief Fixed-width integer aliases for the portable DLT core.
 *
 * The core deliberately includes no system header. The io-sock module build mixes FreeBSD-style
 * headers (usr/include/devs) with QNX headers, and the QNX documentation warns that including
 * QNX headers directly there can break compilation. Using the compiler's predefined type
 * macros keeps the core independent of whichever header universe it is compiled in.
 */

#if !defined(__UINT8_TYPE__) || !defined(__UINT16_TYPE__) || !defined(__UINT32_TYPE__) || \
    !defined(__UINT64_TYPE__) || !defined(__SIZE_TYPE__)
#error "GCC or Clang predefined integer type macros are required"
#endif

namespace dlt
{

using u8 = __UINT8_TYPE__;
using u16 = __UINT16_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;
using usize = __SIZE_TYPE__;

static_assert(sizeof(u8) == 1U, "u8 must be one byte");
static_assert(sizeof(u16) == 2U, "u16 must be two bytes");
static_assert(sizeof(u32) == 4U, "u32 must be four bytes");
static_assert(sizeof(u64) == 8U, "u64 must be eight bytes");

}  // namespace dlt

#endif  // DLT_TYPES_H
