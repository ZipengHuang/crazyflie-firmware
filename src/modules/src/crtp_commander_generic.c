/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie Firmware
 *
 * Copyright (C) 2011-2017 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */
#include <stdbool.h>
#include <string.h>

#include "crtp_commander.h"

#include "log.h"
#include "commander.h"
#include "crtp.h"
#include "FreeRTOS.h"

#include "num.h"

/* The generic commander format contains a packet type and data that has to be
 * decoded into a setpoint_t structure. The aim is to make it future-proof
 * by easily allowing the addition of new packets for future use cases.
 *
 * The packet format is:
 * +------+==========================+
 * | TYPE |     DATA                 |
 * +------+==========================+
 *
 * The type is defined bellow together with a decoder function that should take
 * the data buffer in and fill up a setpoint_t structure.
 * The maximum data size is 29 bytes.
 */

/* To add a new packet:
 *   1 - Add a new type in the packetType_e enum.
 *   2 - Implement a decoder function with good documentation about the data
 *       structure and the intent of the packet.
 *   3 - Add the decoder function to the packetDecoders array.
 *   4 - Pull-request your change :-)
 */

static uint32_t debugCount;
static float testValueX;
static float testValueY;
static float testValueZ;

typedef void (*packetDecoder_t)(setpoint_t *setpoint, uint8_t type, const void *data, size_t datalen);

/* ---===== 1 - packetType_e enum =====--- */
enum packet_type {
  stopType          = 0,
  velocityWorldType = 1,
  rateType = 2,
  fullControlType = 3,
};

/* ---===== 2 - Decoding functions =====--- */
/* The setpoint structure is reinitialized to 0 before being passed to the
 * functions
 */

/* stopDecoder
 * Keeps setopoint to 0: stops the motors and fall
 */
static void stopDecoder(setpoint_t *setpoint, uint8_t type, const void *data, size_t datalen)
{
  return;
}

/* velocityDecoder
 * Set the Crazyflie velocity in the world coordinate system
 */
struct velocityPacket_s {
  float vx;        // m in the world frame of reference
  float vy;        // ...
  float vz;        // ...
  float yawrate;  // rad/s
} __attribute__((packed));
static void velocityDecoder(setpoint_t *setpoint, uint8_t type, const void *data, size_t datalen)
{
  const struct velocityPacket_s *values = data;

  ASSERT(datalen == sizeof(struct velocityPacket_s));

  setpoint->mode.x = modeVelocity;
  setpoint->mode.y = modeVelocity;
  setpoint->mode.z = modeVelocity;

  setpoint->velocity.x = values->vx;
  setpoint->velocity.y = values->vy;
  setpoint->velocity.z = values->vz;

  setpoint->mode.yaw = modeVelocity;

  setpoint->xmode = 0b010;
  setpoint->ymode = 0b010;
  setpoint->zmode = 0b010;

  setpoint->x[1] = values->vx;
  setpoint->y[1] = values->vy;
  setpoint->z[1] = values->vz;

  setpoint->attitudeRate.yaw = values->yawrate;
}


/* fullControlDecoder
 * Used to send control setpoint in position, velocity and acceleration
 */
typedef struct {
  uint16_t packetHasExternalReference:1;
  uint16_t setEmergency:1;
  uint16_t resetEmergency:1;
  uint16_t controlModeX:3;
  uint16_t controlModeY:3;
  uint16_t controlModeZ:3;
  uint16_t :4;
} __attribute__((packed)) fullControlPacketHeader_t; // size 2

typedef struct
{
  fullControlPacketHeader_t header; // size 2
  uint16_t x[3];
  uint16_t y[3];
  uint16_t z[3];
  uint16_t yaw[2];
} __attribute__((packed)) fullControlPacket_t;
static void fullControlDecoder(setpoint_t *setpoint, uint8_t type, const void *data, size_t datalen)
{
  ASSERT(datalen == sizeof(fullControlPacket_t));

  fullControlPacketHeader_t *header = (fullControlPacketHeader_t*)data;
  fullControlPacket_t *packet = (fullControlPacket_t *) data;

  setpoint->xmode = header->controlModeX;
  setpoint->ymode = header->controlModeY;
  setpoint->zmode = header->controlModeZ;
  setpoint->setEmergency = packet->header.setEmergency;
  setpoint->resetEmergency = packet->header.resetEmergency;

  for (int i = 0; i<3; i++) { setpoint->x[i] = half2single(packet->x[i]); }
  for (int i = 0; i<3; i++) { setpoint->y[i] = half2single(packet->y[i]); }
  for (int i = 0; i<3; i++) { setpoint->z[i] = half2single(packet->z[i]); }
  for (int i = 0; i<2; i++) { setpoint->yaw[i] = half2single(packet->yaw[i]); }
  testValueX = setpoint->x[0];
  testValueY = setpoint->y[0];
  testValueZ = setpoint->z[0];
  debugCount++;
}

 /* ---===== 3 - packetDecoders array =====--- */
const static packetDecoder_t packetDecoders[] = {
  [stopType]          = stopDecoder,
  [velocityWorldType] = velocityDecoder,
  [fullControlType]   = fullControlDecoder,
};

/* Decoder switch */
void crtpCommanderGenericDecodeSetpoint(setpoint_t *setpoint, CRTPPacket *pk)
{
  static int nTypes = -1;

  ASSERT(pk->size > 0);

  if (nTypes<0) {
    nTypes = sizeof(packetDecoders)/sizeof(packetDecoders[0]);
  }

  uint8_t type = pk->data[0];

  memset(setpoint, 0, sizeof(setpoint_t));

  if (type<nTypes && (packetDecoders[type] != NULL)) {
    packetDecoders[type](setpoint, type, ((char*)pk->data)+1, pk->size-1);
  }
}

LOG_GROUP_START(spdebug)
LOG_ADD(LOG_UINT32, packetsReceived, &debugCount)
LOG_ADD(LOG_FLOAT, x, &testValueX)
LOG_ADD(LOG_FLOAT, y, &testValueY)
LOG_ADD(LOG_FLOAT, z, &testValueZ)
LOG_GROUP_STOP(spdebug)
