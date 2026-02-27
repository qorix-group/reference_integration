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
use std::net::Ipv4Addr;
use std::net::{SocketAddr, SocketAddrV4, UdpSocket};
use std::time::Duration;

const MAX_UDP_PAYLOAD: usize = 9 * 1024;
static mut RX_BUFFER: [u8; 1024 * 1024 * 10] = [0u8; 1024 * 1024 * 10];
static mut TX_BUFFER: [u8; 1024 * 1024 * 10] = [0u8; 1024 * 1024 * 10];
static TX_RX_SIZES: [usize; 16] = [
    1, 100, 500, 1024, 2048, 5120, 10240, 20480, 51200, 102400, 204800, 512000, 1048576, 2097152, 5242880, 10485760,
];
#[allow(static_mut_refs)]
fn handle_client(socket: UdpSocket) -> Result<(), Box<dyn Error>> {
    // Fill tx_buffer with some data to send to client
    unsafe {
        for i in 0..TX_BUFFER.len() {
            TX_BUFFER[i] = (i % 256) as u8;
        }
    }

    let mut reply_size;
    let mut counter = 0;
    let mut tx_duration_ms = [0u128; 16];
    let mut total_bytes_transmitted = [0usize; 16];
    let dummy = SocketAddrV4::new(Ipv4Addr::new(127, 0, 0, 1), 8080);
    let mut peer_addr: SocketAddr = SocketAddr::V4(dummy);
    // Receive and reply until client closes connection
    while counter < 16 {
        let mut bytes_transmitted = 0usize;
        let mut start_time = std::time::Instant::now();
        loop {
            if counter == 0 {
                // Block waiting for a datagram
                let (_, peer) = unsafe { socket.recv_from(&mut RX_BUFFER)? };
                peer_addr = peer;

                counter = 1;
                reply_size = TX_RX_SIZES[counter];
                start_time = std::time::Instant::now();
            } else {
                reply_size = TX_RX_SIZES[counter];
            }
            // Transmit
            let tx_size = reply_size;
            while reply_size > 0 {
                let send_size = if reply_size > MAX_UDP_PAYLOAD {
                    MAX_UDP_PAYLOAD
                } else {
                    reply_size
                };

                // Send reply back
                unsafe {
                    let slice = &TX_BUFFER[0..send_size];
                    socket.send_to(slice, peer_addr)?;
                    reply_size -= slice.len();
                    std::thread::sleep(Duration::from_millis(1));
                }
            }
            bytes_transmitted += tx_size;

            let elapsed = start_time.elapsed();
            let elapsed_ms = elapsed.as_secs() * 1000 + u64::from(elapsed.subsec_millis());
            if elapsed_ms < 1 {
                continue;
            } else {
                tx_duration_ms[counter] = elapsed_ms as u128;
                total_bytes_transmitted[counter] = bytes_transmitted;
                counter += 1;
                break;
            }
        }
    }
    // Print transmission summary
    println!("App: UDP Transmission Summary:");
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
    let socket = UdpSocket::bind(address)?;
    println!("UDP server listening on {}", address);

    handle_client(socket)?;

    Ok(())
}
