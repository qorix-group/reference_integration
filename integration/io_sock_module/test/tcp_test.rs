//
// Copyright (c) 2025 Contributors to the Eclipse Foundation
//
// See the NOTICE file(s) distributed with this work for additional
// information regarding copyright ownership.
//
// This program and the accompanying materials are made available under the
// terms of the Apache License Version 2.0 which is available at
// <https://www.apache.org/licenses/LICENSE-2.0>
//
// SPDX-License-Identifier: Apache-2.0
//

use std::error::Error;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};

static mut RX_BUFFER: [u8; 1024 * 1024] = [0u8; 1024 * 1024];
static mut TX_BUFFER: [u8; 1024 * 1024] = [0u8; 1024 * 1024];
#[allow(static_mut_refs)]
fn handle_client(mut stream: TcpStream) -> Result<(), Box<dyn Error>> {
    println!("Client connected from {}", stream.peer_addr()?);

    // Fill tx_buffer with some data to send to client
    unsafe {
        for i in 0..TX_BUFFER.len() {
            TX_BUFFER[i] = (i % 256) as u8;
        }
    }
    let mut expected_bytes = 0;
    let mut received_bytes = 0;
    let mut reply_size = 1;
    //let mut counter = 0;
    // Receive and reply until client closes connection
    loop {
        // Receive data from client
        let bytes_read = unsafe { stream.read(&mut RX_BUFFER)? };
        if bytes_read == 0 {
            println!("Client closed connection");
            return Ok(());
        }

        if received_bytes == 0 {
            // Check MSB of first byte to determine expected size
            expected_bytes = unsafe {
                match RX_BUFFER[0] & 0xF0 {
                    0x00 => 1,
                    0x10 => 1024,
                    0x20 => 10 * 1024,
                    0x30 => 100 * 1024,
                    0x40 => 1024 * 1024,
                    _ => 1,
                }
            };

            // Check LSB of first byte to determine transmit size
            // if 0, just send 1 byte
            reply_size = unsafe {
                match RX_BUFFER[0] & 0x0F {
                    0 => 1,
                    1 => 1024,
                    2 => 10 * 1024,
                    3 => 100 * 1024,
                    4 => 1024 * 1024,
                    _ => 1,
                }
            };
        }

        received_bytes += bytes_read;
        if received_bytes < expected_bytes {
            continue; // Keep reading until we get the expected number of bytes
        } else {
            received_bytes = 0; // Reset for next message
        }
        // Send reply back
        unsafe {
            stream.write_all(&TX_BUFFER[..reply_size])?;
        }
        stream.flush()?;

        //println!("Reply sent to client, counter: {}", counter);
        //counter += 1;
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let address = "0.0.0.0:5000";
    let listener = TcpListener::bind(address)?;

    println!("TCP server listening on {}", address);

    let (stream, _) = listener.accept()?;

    handle_client(stream)?;

    Ok(())
}
