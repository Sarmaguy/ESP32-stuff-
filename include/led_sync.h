#pragma once
#include <stdint.h>

// Shared master<->slave LED sync protocol. Both main.cpp and slave_main.cpp
// include this so the wire format can't silently drift between the two builds.

#define LED_SYNC_UDP_PORT    4210
#define LED_SYNC_VERSION     1
#define LED_SYNC_ACK_VERSION 2

// Master -> slave: full LED state (UDP unicast + broadcast, ~20Hz in music mode)
struct __attribute__((packed)) UdpLedPkt {
  uint8_t  version;    // = LED_SYNC_VERSION
  uint8_t  on;         // 1=on, 0=off
  uint8_t  r, g, b;
  uint8_t  brightness;
  uint16_t seq;
};

// Slave -> master: liveness ack, sent once per processed batch of packets.
// The master uses these (source IP + recency) to know which slaves are online.
struct __attribute__((packed)) UdpAckPkt {
  uint8_t  version;    // = LED_SYNC_ACK_VERSION
  uint16_t seq;        // seq of the last UdpLedPkt the slave applied
};
