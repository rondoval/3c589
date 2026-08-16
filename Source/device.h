/*

Copyright (C) 2001-2004 Neil Cafferkey

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston,
MA 02111-1307, USA.

*/

#ifndef DEVICE_H
#define DEVICE_H


#include <exec/types.h>
#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/semaphores.h>
#include <devices/sana2.h>
#include <devices/sana2specialstats.h>
#include <resources/card.h>
#include <libraries/pccard.h>
#include <devices/timer.h>

#ifndef UPINT
typedef ULONG UPINT;
typedef LONG PINT;
#endif

#ifdef __mc68000
#define REG(A) __asm(A)
#else
#define REG(A)
#endif

#define USE_HACKS


struct DevBase
{
   struct Device device;
   APTR seg_list;
   struct ExecBase *sys_base;
   APTR card_base;
   struct UtilityBase *utility_base;
   struct Library *pccard_base;
   struct MinList units;
   struct timerequest timer_request;
};


enum
{
   WRITE_QUEUE,
   ADOPT_QUEUE,
   EVENT_QUEUE,
   GENERAL_QUEUE,
   REQUEST_QUEUE_COUNT
};

#define ADDRESS_SIZE 6
#define HEADER_SIZE 14
#define PREAMBLE_SIZE 4
#define PACKET_DEST 0
#define PACKET_SOURCE 6
#define PACKET_TYPE 12
#define PACKET_DATA 14
#define MTU 1500
#define MAX_PACKET_SIZE ((HEADER_SIZE)+(MTU))

#define STAT_COUNT 3


struct DevUnit
{
   struct MinNode node;
   ULONG unit_num;
   ULONG open_count;
   UWORD flags;
   UWORD transceiver;
   struct Task *task;
   struct MsgPort *request_ports[REQUEST_QUEUE_COUNT];
   struct DevBase *device;
   struct CardHandle *card_handle;
   UBYTE *tuple_buffer;
   UBYTE *rx_buffer;
   UBYTE *tx_buffer;
   volatile UBYTE *config_base;
   volatile UBYTE *io_base;
   ULONG card_removed_signal;
   ULONG card_inserted_signal;
   ULONG range_count;
   UBYTE address[ADDRESS_SIZE];
   UBYTE default_address[ADDRESS_SIZE];
   struct MinList openers;
   struct MinList type_trackers;
   struct MinList multicast_ranges;
   struct Interrupt rx_int;
   struct Interrupt tx_int;
   struct Sana2DeviceStats stats;
   ULONG special_stats[STAT_COUNT];
   struct SignalSemaphore access_lock;
   UWORD rx_filter_cmd;
};


struct Opener
{
   struct MinNode node;
   struct MsgPort read_port;
   BOOL (*rx_function)(APTR REG("a0"),APTR REG("a1"),ULONG REG("d0"));
   BOOL (*tx_function)(APTR REG("a0"),APTR REG("a1"),ULONG REG("d0"));
   ULONG *(*dma_tx_function)(APTR REG("a0"));
   struct Hook *filter_hook;
   struct MinList initial_stats;
};


struct TypeStats
{
   struct MinNode node;
   ULONG packet_type;
   struct Sana2PacketTypeStats stats;
};


struct TypeTracker
{
   struct MinNode node;
   ULONG packet_type;
   struct Sana2PacketTypeStats stats;
   ULONG user_count;
};


struct AddressRange
{
   struct MinNode node;
   ULONG add_count;
   ULONG lower_bound_left;
   ULONG upper_bound_left;
   UWORD lower_bound_right;
   UWORD upper_bound_right;
};


/* Unit flags */

#define UNITF_SHARED (1<<0)
#define UNITF_ONLINE (1<<1)
#define UNITF_HAVECARD (1<<2)
#define UNITF_HAVEADAPTER (1<<3)
#define UNITF_CONFIGURED (1<<4)
#define UNITF_PROM (1<<5)
#define UNITF_ROADRUNNER (1<<6)


IMPORT const TEXT device_name[];

#define HANDLE_PRIORITY 10

#define S2_3C589_WRITEEEPROM 0x998f


/* Endianness macros */

#define BEWord(A)\
   (A)

#define MakeBEWord(A)\
   BEWord(A)

#define LEWord(A)\
   ({\
      UWORD _LEWord_A=(A);\
      _LEWord_A=((_LEWord_A)<<8)|((_LEWord_A)>>8);\
   })

#define MakeLEWord(A)\
   LEWord(A)

#define BELong(A)\
   (A)

#define LELong(A)\
   ({\
      ULONG _LELong_A=(A);\
      _LELong_A=(LEWord(_LELong_A)<<16)|LEWord((_LELong_A)>>16);\
   })

#define MakeLELong(A)\
   LELong(A)


/* I/O macros */

#define ByteIn(address)\
   (*((volatile UBYTE *)(address)))

#define ByteOut(address,value)\
   *((volatile UBYTE *)(address))=(value)

#define WordIn(address)\
   (*((volatile UWORD *)(address)))

#define WordOut(address,value)\
   *((volatile UWORD *)(address))=(value)

#define LongIn(address)\
   (*((volatile ULONG *)(address)))

#define LongOut(address,value)\
   *((volatile ULONG *)(address))=(value)

#define LEWordIn(address)\
   LEWord(WordIn(address))

#define LEWordOut(address,value)\
   WordOut(address,MakeLEWord(value))

#define LELongIn(address)\
   LELong(LongIn(address))

#define LELongOut(address,value)\
   LongOut(address,MakeLELong(value))

#define BEWordOut(address,value)\
   WordOut(address,MakeBEWord(value))


/* Library and device bases */

#define SysBase (base->sys_base)
#define CardResource (base->card_base)
#define UtilityBase (base->utility_base)
#define PCCardBase (base->pccard_base)
#define TimerBase (base->timer_request.tr_node.io_Device)

#ifndef BASE_REG
#define BASE_REG "a6"
#endif

/*register APTR base asm(BASE_REG);*/


#endif
