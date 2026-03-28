/*
 * km_proto.h — Serial frame protocol for KVM dongle relay
 *
 * Frame format: [0xAA][TYPE:1B][LEN:1B][PAYLOAD:LEN B][CRC8-CCITT:1B]
 *   TYPE 0x01 → keyboard report  (8 B: mods, reserved, 6 keycodes)
 *   TYPE 0x03 → all-keys-release (8 B: zeros, sent on switch to prevent stuck keys)
 *
 * CRC-8 CCITT (init=0xFF) computed over PAYLOAD only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KM_PROTO_H_
#define KM_PROTO_H_

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define KM_FRAME_MAGIC    0xAAU
#define KM_TYPE_KB        0x01U
#define KM_TYPE_RELEASE   0x03U
#define KM_PAYLOAD_KB_LEN 8U
#define KM_FRAME_OVERHEAD 4U   /* magic + type + len + crc */
#define KM_FRAME_KB_LEN   (KM_FRAME_OVERHEAD + KM_PAYLOAD_KB_LEN)  /* 12 bytes */

/* CRC-8 CCITT (poly=0x07, init=0xFF, no reflection) */
static inline uint8_t km_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xFF;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
		}
	}
	return crc;
}

/*
 * Build a KM frame into buf[KM_FRAME_KB_LEN].
 * payload must be KM_PAYLOAD_KB_LEN (8) bytes.
 * type must be KM_TYPE_KB or KM_TYPE_RELEASE.
 */
static inline void km_build_frame(uint8_t *buf, uint8_t type,
				   const uint8_t *payload)
{
	buf[0] = KM_FRAME_MAGIC;
	buf[1] = type;
	buf[2] = KM_PAYLOAD_KB_LEN;
	memcpy(&buf[3], payload, KM_PAYLOAD_KB_LEN);
	buf[3 + KM_PAYLOAD_KB_LEN] = km_crc8(payload, KM_PAYLOAD_KB_LEN);
}

#endif /* KM_PROTO_H_ */
