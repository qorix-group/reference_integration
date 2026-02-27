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

static mut RX_BUFFER: [u8; 1024 * 1024 * 10] = [0u8; 1024 * 1024 * 10];
static mut TX_BUFFER: [u8; 1024 * 1024 * 10] = [0u8; 1024 * 1024 * 10];
static TX_RX_SIZES: [usize; 16] = [
    1, 100, 500, 1024, 2048, 5120, 10240, 20480, 51200, 102400, 204800, 512000, 1048576, 2097152, 5242880, 10485760,
];
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
    let mut counter = 1;
    let mut tx_duration_ms = [0u128; 16];
    let mut total_bytes_transmitted = [0usize; 16];
    // Receive and reply until client closes connection
    while counter < 16 {
        let mut bytes_transmitted = 0usize;
        let start_time = std::time::Instant::now();
        'repeat_transmit: loop {
            if counter == 0 {
                // Receive data from client
                let bytes_read = unsafe { stream.read(&mut RX_BUFFER)? };
                if bytes_read == 0 {
                    println!("Client closed connection");
                    return Ok(());
                }

                if received_bytes == 0 {
                    // Check MSB of first byte to determine expected size
                    expected_bytes = TX_RX_SIZES[unsafe { RX_BUFFER[0] >> 4 } as usize];

                    // Check LSB of first byte to determine transmit size
                    // if 0, just send 1 byte
                    reply_size = TX_RX_SIZES[unsafe { RX_BUFFER[0] & 0x0F } as usize];
                }

                received_bytes += bytes_read;
                if received_bytes < expected_bytes {
                    continue; // Keep reading until we get the expected number of bytes
                } else {
                    received_bytes = 0; // Reset for next message
                }
            } else {
                reply_size = TX_RX_SIZES[counter];
            }

            // Send reply back
            unsafe {
                stream.write_all(&TX_BUFFER[..reply_size])?;
            }
            stream.flush()?;
            bytes_transmitted += reply_size;

            let elapsed = start_time.elapsed();
            let elapsed_ms = elapsed.as_secs() * 1000 + u64::from(elapsed.subsec_millis());
            if elapsed_ms < 5000 {
                continue 'repeat_transmit;
            } else {
                tx_duration_ms[counter] = elapsed_ms as u128;
                total_bytes_transmitted[counter] = bytes_transmitted;
                counter += 1;
                break;
            }
        }
    }
    // Print transmission summary
    println!("App: TCP Transmission Summary:");
    let mut bytes_transmitted: usize = 0;
    let mut total_time: u128 = 0;
    for i in 1..counter {
        bytes_transmitted += total_bytes_transmitted[i];
        total_time += tx_duration_ms[i];
        println!(
            "App: Size: {:8} bytes, Total Transmitted: {:9} bytes, Duration: {:4} ms, Throughput: {:6} bytes/ms",
            TX_RX_SIZES[i],
            total_bytes_transmitted[i],
            tx_duration_ms[i],
            if tx_duration_ms[i] > 0 {
                total_bytes_transmitted[i] as u128 / tx_duration_ms[i]
            } else {
                0
            }
        );
    }
    if total_time > 0 {
        println!(
            "App: Average Throughput: {} bytes/ms, for total {} bytes transmitted in {} ms",
            bytes_transmitted as u128 / total_time,
            bytes_transmitted,
            total_time
        );
    } else {
        println!("App: No valid transmission time measured.");
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let address = "0.0.0.0:5000";
    let listener = TcpListener::bind(address)?;

    println!("App: TCP server listening on {}", address);

    let (stream, _) = listener.accept()?;

    handle_client(stream)?;

    Ok(())
}
