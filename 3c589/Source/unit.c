/*

File: unit.c
Author: Neil Cafferkey
Copyright (C) 2001-2010 Neil Cafferkey

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


#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/errors.h>

#include <proto/exec.h>
#include <proto/alib.h>
#include <proto/cardres.h>
#include <proto/utility.h>
#include <proto/pccard.h>
#include <proto/timer.h>

#include "device.h"
#include "etherlink3.h"

#include "unit_protos.h"
#include "request_protos.h"


#define TASK_PRIORITY 0
#define STACK_SIZE 4096
#define MAX_TUPLE_SIZE 0xff
#define TUPLE_BUFFER_SIZE (MAX_TUPLE_SIZE+8)
#define INT_MASK\
   (EL3INTF_RXCOMPLETE|EL3INTF_TXAVAIL|EL3INTF_TXCOMPLETE)


IMPORT struct ExecBase *AbsExecBase;

static BOOL InitialiseCard(struct DevUnit *unit,struct DevBase *base);
static VOID InitialiseAdapter(struct DevUnit *unit,struct DevBase *base);
static struct AddressRange *FindMulticastRange(struct DevUnit *unit,
   ULONG lower_bound_left,UWORD lower_bound_right,ULONG upper_bound_left,
   UWORD upper_bound_right,struct DevBase *base);
static VOID CardRemovedInt(struct DevUnit *unit REG("a1"));
static VOID CardInsertedInt(struct DevUnit *unit REG("a1"));
static UBYTE CardStatusInt(UBYTE mask REG("d0"),
   struct DevUnit *unit REG("a1"));
static VOID RxInt(struct DevUnit *unit REG("a1"));
static VOID CopyPacket(struct DevUnit *unit,struct IOSana2Req *request,
   UWORD packet_size,UWORD packet_type,BOOL all_read,struct DevBase *base);
static BOOL AddressFilter(struct DevUnit *unit,UBYTE *address,
   struct DevBase *base);
static VOID TxInt(struct DevUnit *unit REG("a1"));
static VOID TxError(struct DevUnit *unit,struct DevBase *base);
static VOID ReportEvents(struct DevUnit *unit,ULONG events,
   struct DevBase *base);
static VOID UnitTask();
static UWORD ReadEEPROM(volatile UBYTE *io_base,UWORD index);



/****i* 3c589.device/GetUnit ***********************************************
*
*   NAME
*	GetUnit -- Get a unit by number.
*
*   SYNOPSIS
*	unit = GetUnit(unit_num)
*
*	struct DevUnit *GetUnit(ULONG);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

struct DevUnit *GetUnit(ULONG unit_num,struct DevBase *base)
{
   struct DevUnit *unit;

   unit=FindUnit(unit_num,base);

   if(unit==NULL)
   {
      unit=CreateUnit(unit_num,base);
      if(unit!=NULL)
         AddTail((APTR)&base->units,(APTR)unit);
   }

   return unit;
}



/****i* 3c589.device/FindUnit **********************************************
*
*   NAME
*	FindUnit -- Find a unit by number.
*
*   SYNOPSIS
*	unit = FindUnit(unit_num)
*
*	struct DevUnit *FindUnit(ULONG);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

struct DevUnit *FindUnit(ULONG unit_num,struct DevBase *base)
{
   struct DevUnit *unit,*tail;
   BOOL found;

   unit=(APTR)base->units.mlh_Head;
   tail=(APTR)&base->units.mlh_Tail;
   found=FALSE;

   while((unit!=tail)&&!found)
   {
      if(unit->unit_num==unit_num)
         found=TRUE;
      else
         unit=(APTR)unit->node.mln_Succ;
   }

   if(!found)
      unit=NULL;

   return unit;
}



/****i* 3c589.device/CreateUnit ********************************************
*
*   NAME
*	CreateUnit -- Create a unit.
*
*   SYNOPSIS
*	unit = CreateUnit(unit_num)
*
*	struct DevUnit *CreateUnit(ULONG);
*
*   FUNCTION
*	Creates a new unit.
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

struct DevUnit *CreateUnit(ULONG unit_num,struct DevBase *base)
{
   BOOL success=TRUE;
   struct DevUnit *unit;
   struct Task *task;
   struct MsgPort *port;
   UBYTE i;
   struct CardHandle *card_handle;
   APTR stack;
   struct Interrupt *card_removed_int,*card_inserted_int,*card_status_int;

   unit=AllocMem(sizeof(struct DevUnit),MEMF_CLEAR);
   if(unit==NULL)
      success=FALSE;

   if(success)
   {
      unit->unit_num=unit_num;
      unit->device=base;

      InitSemaphore(&unit->access_lock);

      /* Set up packet filter command */

      unit->rx_filter_cmd=EL3CMD_SETRXFILTER|EL3CMD_SETRXFILTERF_BCAST
         |EL3CMD_SETRXFILTERF_UCAST;

      /* Create the message ports for queuing requests */

      for(i=0;i<REQUEST_QUEUE_COUNT;i++)
      {
         unit->request_ports[i]=port=AllocMem(sizeof(struct MsgPort),
            MEMF_PUBLIC|MEMF_CLEAR);
         if(port==NULL)
            success=FALSE;

         if(success)
         {
            NewList(&port->mp_MsgList);
            port->mp_Flags=PA_IGNORE;
            port->mp_SigTask=&unit->tx_int;
         }
      }

      card_handle=unit->card_handle=
         AllocMem(sizeof(struct CardHandle),MEMF_PUBLIC|MEMF_CLEAR);
      unit->tuple_buffer=
         AllocVec(TUPLE_BUFFER_SIZE,MEMF_ANY);
      unit->rx_buffer=AllocVec((MAX_PACKET_SIZE+3)&~3,MEMF_PUBLIC);
      unit->tx_buffer=AllocVec(MAX_PACKET_SIZE,MEMF_PUBLIC);
      if((card_handle==NULL)||(unit->tuple_buffer==NULL)||
         (unit->rx_buffer==NULL)||(unit->tx_buffer==NULL))
         success=FALSE;
   }

   if(success)
   {
      NewList((APTR)&unit->openers);
      NewList((APTR)&unit->type_trackers);
      NewList((APTR)&unit->multicast_ranges);

      /* Initialise transmit and receive interrupts */

      unit->rx_int.is_Node.ln_Name=(TEXT *)device_name;
      unit->rx_int.is_Node.ln_Pri=16;
      unit->rx_int.is_Code=RxInt;
      unit->rx_int.is_Data=unit;

      unit->tx_int.is_Node.ln_Name=(TEXT *)device_name;
      unit->tx_int.is_Code=TxInt;
      unit->tx_int.is_Data=unit;

      unit->request_ports[WRITE_QUEUE]->mp_Flags=PA_SOFTINT;
   }

   if(success)
   {
      /* Set up card handle */

      card_handle->cah_CardNode.ln_Pri=HANDLE_PRIORITY;
      card_handle->cah_CardNode.ln_Name=(APTR)device_name;
      card_handle->cah_CardFlags=CARDF_IFAVAILABLE;

      card_handle->cah_CardRemoved=card_removed_int=
         AllocVec(sizeof(struct Interrupt),MEMF_PUBLIC|MEMF_CLEAR);

      card_handle->cah_CardInserted=card_inserted_int=
         AllocVec(sizeof(struct Interrupt),MEMF_PUBLIC|MEMF_CLEAR);

      card_handle->cah_CardStatus=card_status_int=
         AllocVec(sizeof(struct Interrupt),MEMF_PUBLIC|MEMF_CLEAR);

      if((card_removed_int==NULL)
         ||(card_inserted_int==NULL)
         ||(card_status_int==NULL))
         success=FALSE;
   }

   if(success)
   {
      /* Try to gain access to card */

      card_removed_int->is_Code=CardRemovedInt;
      card_removed_int->is_Data=unit;
      card_inserted_int->is_Code=CardInsertedInt;
      card_inserted_int->is_Data=unit;
      card_status_int->is_Code=(APTR)CardStatusInt;
      card_status_int->is_Data=unit;

      if(OwnCard(card_handle)!=0)
         success=FALSE;
   }

   if(success)
   {
      unit->flags|=UNITF_HAVECARD;
      if(!InitialiseCard(unit,base))
         success=FALSE;

      /* Create a new task */

      unit->task=task=AllocMem(sizeof(struct Task),MEMF_PUBLIC|MEMF_CLEAR);
      if(task==NULL)
         success=FALSE;
   }

   if(success)
   {
      stack=AllocMem(STACK_SIZE,MEMF_PUBLIC);
      if(stack==NULL)
         success=FALSE;
   }

   if(success)
   {
      /* Initialise and start task */

      task->tc_Node.ln_Type=NT_TASK;
      task->tc_Node.ln_Pri=TASK_PRIORITY;
      task->tc_Node.ln_Name=(APTR)device_name;
      task->tc_SPUpper=stack+STACK_SIZE;
      task->tc_SPLower=stack;
      task->tc_SPReg=stack+STACK_SIZE;
      NewList(&task->tc_MemEntry);

      if(AddTask(task,UnitTask,NULL)==NULL)
         success=FALSE;
   }

   /* Send the unit to the new task */

   if(success)
      task->tc_UserData=unit;

   if(!success)
   {
      DeleteUnit(unit,base);
      unit=NULL;
   }

   return unit;
}



/****i* 3c589.device/DeleteUnit ********************************************
*
*   NAME
*	DeleteUnit -- Delete a unit.
*
*   SYNOPSIS
*	DeleteUnit(unit)
*
*	VOID DeleteUnit(struct DevUnit *);
*
*   FUNCTION
*	Deletes a unit.
*
*   INPUTS
*	unit - Device unit (can be NULL).
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

VOID DeleteUnit(struct DevUnit *unit,struct DevBase *base)
{
   UBYTE i;
   struct CardHandle *card_handle;
   struct Task *task;

   if(unit!=NULL)
   {
      task=unit->task;
      if(task!=NULL)
      {
         if(task->tc_SPLower!=NULL)
         {
            RemTask(task);
            FreeMem(task->tc_SPLower,STACK_SIZE);
         }
         FreeMem(task,sizeof(struct Task));
      }

      for(i=0;i<REQUEST_QUEUE_COUNT;i++)
      {
         if(unit->request_ports[i]!=NULL)
            FreeMem(unit->request_ports[i],sizeof(struct MsgPort));
      }

      FreeVec(unit->tuple_buffer);

      card_handle=unit->card_handle;
      if(card_handle!=NULL)
      {
         if((unit->flags&UNITF_HAVECARD)!=0)
         {
            if((unit->flags&UNITF_HAVEADAPTER)!=0)
               GoOffline(unit,base);

            unit->flags&=~UNITF_HAVEADAPTER;
            CardMiscControl(card_handle,0);
            CardResetCard(card_handle);
            ReleaseCard(card_handle,CARDF_REMOVEHANDLE);
         }

         FreeVec(card_handle->cah_CardStatus);
         FreeVec(card_handle->cah_CardInserted);
         FreeVec(card_handle->cah_CardRemoved);

         FreeMem(card_handle,sizeof(struct CardHandle));
      }

      FreeVec(unit->tx_buffer);
      FreeVec(unit->rx_buffer);

      FreeMem(unit,sizeof(struct DevUnit));
   }

   return;
}



/****i* 3c589.device/InitialiseCard ****************************************
*
*   NAME
*	InitialiseCard -- .
*
*   SYNOPSIS
*	success = InitialiseCard(unit)
*
*	BOOL InitialiseCard(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit
*
*   RESULT
*	success
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static BOOL InitialiseCard(struct DevUnit *unit,struct DevBase *base)
{
   BOOL success=TRUE;
   struct CardMemoryMap *card_map;
   struct CardHandle *card_handle;
   UBYTE config_value,i,window_count,*tuple_buffer;
   const struct TagItem *tuple_tags=NULL;
   ULONG *io_bases,*io_lengths,io_base=0,config_base_offset;
   UWORD maker,product;

   /* Wake up card */

   card_handle=unit->card_handle;
   CardMiscControl(card_handle,CARD_ENABLEF_DIGAUDIO|CARD_DISABLEF_WP);

   /* Get card's make and model */

   tuple_buffer=unit->tuple_buffer;
   if(CopyTuple(card_handle,tuple_buffer,PCCARD_TPL_MANFID,MAX_TUPLE_SIZE))
   {
      tuple_tags=PCCard_GetTupleInfo(tuple_buffer);
      if(tuple_tags!=NULL)
      {
         maker=GetTagData(PCCARD_Maker,0,tuple_tags);
         product=GetTagData(PCCARD_Product,0,tuple_tags);
      }
   }

   /* Check this is a card we can use */

   if((tuple_tags==NULL)||!((maker==0x101)&&((product==0x35)||
      (product==0x3d)||(product==0x562)||(product==0x589)||
      (product==0x556)||(product==0x574))))
      success=FALSE;

   if((product==0x556)||(product==0x574))
      unit->flags|=UNITF_ROADRUNNER;

   /* Get configuration data */

   if(!CopyTuple(card_handle,tuple_buffer,PCCARD_TPL_CONFIG,MAX_TUPLE_SIZE))
      success=FALSE;

   if(success)
   {
      PCCard_FreeTupleInfo(tuple_tags);
      tuple_tags=PCCard_GetTupleInfo(tuple_buffer);
      if(tuple_tags==NULL)
         success=FALSE;
   }

   if(success)
   {
      config_base_offset=GetTagData(PCCARD_RegisterBase,0,tuple_tags);

      PCCard_FreeTupleInfo(tuple_tags);
      tuple_tags=NULL;

      /* Get IO base */

      if(!CopyTuple(card_handle,tuple_buffer,PCCARD_TPL_CFTABLEENTRY,
         MAX_TUPLE_SIZE))
         success=FALSE;
   }

   if(success)
   {
      tuple_tags=PCCard_GetTupleInfo(tuple_buffer);
      if(tuple_tags==NULL)
         success=FALSE;
   }

   if(success)
   {
      config_value=GetTagData(PCCARD_ModeNo,0,tuple_tags);
      if(product = 0x562 && config_value == 9)
         config_value = 7;

      io_bases=(APTR)GetTagData(PCCARD_IOWinBases,NULL,tuple_tags);
      if(io_bases==NULL)
         success=FALSE;
   }

   if(success)
   {
      io_lengths=(APTR)GetTagData(PCCARD_IOWinLengths,NULL,tuple_tags);

      window_count=GetTagData(PCCARD_IOWinCount,0,tuple_tags);

      for(i=0;(i<window_count)&&(io_base==0);i++)
         if(io_lengths[i]==16)
            io_base=io_bases[i];
   }

   PCCard_FreeTupleInfo(tuple_tags);

   /* Configure card */

   if(success)
   {
      card_map=GetCardMap();
      unit->config_base=card_map->cmm_AttributeMemory+config_base_offset;

      unit->io_base=card_map->cmm_IOMemory+io_base;
      unit->config_base[PCCARD_REG_COR]=config_value;
      unit->config_base[PCCARD_REG_CCSR]|=PCCARD_REG_CCSRF_AUDIOENABLE;

      InitialiseAdapter(unit,base);
      unit->flags|=UNITF_HAVEADAPTER;
   }

   return success;
}



/****i* 3c589.device/InitialiseAdapter *************************************
*
*   NAME
*	InitialiseAdapter -- .
*
*   SYNOPSIS
*	InitialiseAdapter(unit)
*
*	VOID InitialiseAdapter(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static VOID InitialiseAdapter(struct DevUnit *unit,struct DevBase *base)
{
   volatile UBYTE *io_base;
   UBYTE *p,i;
   UWORD address_part,config,link,media,new_media,medium,media_status;

   /* Select IO addresses and interrupt */

   io_base=unit->io_base;
   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|0);
   LEWordOut(io_base+EL3REG_RESCONFIG,0x3f00);

   /* Get default MAC address */

   p=unit->default_address;

   for(i=0;i<ADDRESS_SIZE/2;i++)
   {
      address_part=ReadEEPROM(io_base,EL3EEPROM_ALTADDRESS0+i);
      *p++=address_part>>8;
      *p++=address_part&0xff;
   }

   /* Get available transceivers */

   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|0);
   media=LEWordIn(io_base+EL3REG_CONFIG);

   /* Get transceivers with an active link */

   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|4);
   link=0;
   media_status=LEWordIn(io_base+EL3REG_MEDIA);
   if((media_status&EL3REG_MEDIAF_BEAT)!=0)
      link|=EL3REG_CONFIGF_HASTP;
   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|0);

   new_media=media&link;
   if((new_media)!=0)
      media=new_media;
   else
   {
      /* Get transceiver choice from EEPROM */

      config=ReadEEPROM(io_base,EL3EEPROM_ADDRCONFIG)&
         (EL3REG_ADDRCONFIG_XCVRMASK|EL3REG_ADDRCONFIGF_AUTOSELECT);

      if(config==EL3REG_ADDRCONFIG_XCVRTP)
         new_media=EL3REG_CONFIGF_HASTP;
      else if(config==EL3REG_ADDRCONFIG_XCVRAUI)
         new_media=EL3REG_CONFIGF_HASAUI;
      else if(config==EL3REG_ADDRCONFIG_XCVRCOAX)
         new_media=EL3REG_CONFIGF_HASCOAX;
      else
         new_media=EL3REG_CONFIGF_HASTP|EL3REG_CONFIGF_HASCOAX|
            EL3REG_CONFIGF_HASAUI;

      new_media&=media;
      if(new_media!=0)
         media=new_media;
   }

   /* Prioritised choice from remaining transceivers */

   if((media&EL3REG_CONFIGF_HASTP)!=0)
      medium=EL3REG_ADDRCONFIG_XCVRTP;
   else if((media&EL3REG_CONFIGF_HASCOAX)!=0)
      medium=EL3REG_ADDRCONFIG_XCVRCOAX;
   else
      medium=EL3REG_ADDRCONFIG_XCVRAUI;
   unit->transceiver=medium;

   return;
}



/****i* 3c589.device/ConfigureAdapter **************************************
*
*   NAME
*	ConfigureAdapter -- .
*
*   SYNOPSIS
*	ConfigureAdapter(unit)
*
*	VOID ConfigureAdapter(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

VOID ConfigureAdapter(struct DevUnit *unit,struct DevBase *base)
{
   volatile UBYTE *io_base;
   UBYTE i;

   /* Set MAC address */

   io_base=unit->io_base;
   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|2);

   for(i=0;i<ADDRESS_SIZE;i++)
      ByteOut(io_base+EL3REG_ADDRESS0+i,unit->address[i]);

   /* Decide on promiscuous mode */

   if((unit->flags&UNITF_PROM)!=0)
      unit->rx_filter_cmd|=EL3CMD_SETRXFILTERF_PROM;

   /* Select chosen transceiver */

   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|0);
   LEWordOut(io_base+EL3REG_ADDRCONFIG,unit->transceiver);
   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|1);

   /* Go online */

   GoOnline(unit,base);

   /* Return */

   return;
}



/****i* 3c589.device/GoOnline **********************************************
*
*   NAME
*	GoOnline -- .
*
*   SYNOPSIS
*	GoOnline(unit)
*
*	VOID GoOnline(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

VOID GoOnline(struct DevUnit *unit,struct DevBase *base)
{
   volatile UBYTE *io_base;
   UWORD transceiver;

   /* Choose interrupts */

   unit->flags|=UNITF_ONLINE;
   io_base=unit->io_base;
   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SETINTMASK|INT_MASK);
   LEWordOut(io_base+EL3REG_COMMAND,(EL3CMD_SETZEROMASK)|0xff);

   /* Enable the transceiver */

   transceiver=unit->transceiver;

   if(transceiver==EL3REG_ADDRCONFIG_XCVRTP)
   {
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|4);
      LEWordOut(io_base+EL3REG_MEDIA,
         EL3REG_MEDIAF_BEATENABLE|EL3REG_MEDIAF_JABBERENABLE);
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|1);
   }
   else if(transceiver==EL3REG_ADDRCONFIG_XCVRCOAX)
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_STARTCOAX|0);

   LEWordOut(io_base+EL3REG_COMMAND,unit->rx_filter_cmd);
   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_RXENABLE);
   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_TXENABLE);

   /* Record start time and report Online event */

   GetSysTime(&unit->stats.LastStart);
   ReportEvents(unit,S2EVENT_ONLINE,base);

   return;
}



/****i* 3c589.device/GoOffline *********************************************
*
*   NAME
*	GoOffline -- .
*
*   SYNOPSIS
*	GoOffline(unit)
*
*	VOID GoOffline(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

VOID GoOffline(struct DevUnit *unit,struct DevBase *base)
{
   volatile UBYTE *io_base;

   io_base=unit->io_base;
   unit->flags&=~UNITF_ONLINE;

   if((unit->flags&UNITF_HAVEADAPTER)!=0)
   {
      /* Stop interrupts */

      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SETINTMASK|0);
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_ACKINT|EL3INTF_ANY);

      /* Stop transmission and reception */

      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_RXDISABLE);
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_TXDISABLE);

      /* Turn off media functions */

      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|4);
      LEWordOut(io_base+EL3REG_MEDIA,0);
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SELECTWINDOW|1);
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_STOPCOAX|0);
   }

   /* Flush pending read and write requests */

   FlushUnit(unit,WRITE_QUEUE,S2ERR_OUTOFSERVICE,base);

   /* Report Offline event and return */

   ReportEvents(unit,S2EVENT_OFFLINE,base);
   return;
}



/****i* 3c589.device/AddMulticastRange *************************************
*
*   NAME
*	AddMulticastRange -- .
*
*   SYNOPSIS
*	success = AddMulticastRange(unit,lower_bound,upper_bound)
*
*	BOOL AddMulticastRange(struct DevUnit *,UBYTE *,UBYTE *);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

BOOL AddMulticastRange(struct DevUnit *unit,UBYTE *lower_bound,
   UBYTE *upper_bound,struct DevBase *base)
{
   struct AddressRange *range;
   ULONG lower_bound_left,upper_bound_left;
   UWORD lower_bound_right,upper_bound_right;

   lower_bound_left=BELong(*((ULONG *)lower_bound));
   lower_bound_right=BEWord(*((UWORD *)(lower_bound+4)));
   upper_bound_left=BELong(*((ULONG *)upper_bound));
   upper_bound_right=BEWord(*((UWORD *)(upper_bound+4)));

   range=FindMulticastRange(unit,lower_bound_left,lower_bound_right,
      upper_bound_left,upper_bound_right,base);

   if(range!=NULL)
      range->add_count++;
   else
   {
      range=AllocMem(sizeof(struct AddressRange),MEMF_PUBLIC);
      if(range!=NULL)
      {
         range->lower_bound_left=lower_bound_left;
         range->lower_bound_right=lower_bound_right;
         range->upper_bound_left=upper_bound_left;
         range->upper_bound_right=upper_bound_right;
         range->add_count=1;

         Disable();
         AddTail((APTR)&unit->multicast_ranges,(APTR)range);
         Enable();

         if(unit->range_count++==0)
         {
            unit->rx_filter_cmd|=EL3CMD_SETRXFILTERF_MCAST;
            LEWordOut(unit->io_base+EL3REG_COMMAND,unit->rx_filter_cmd);
         }
      }
   }

   return range!=NULL;
}



/****i* 3c589.device/RemMulticastRange *************************************
*
*   NAME
*	RemMulticastRange -- .
*
*   SYNOPSIS
*	found = RemMulticastRange(unit,lower_bound,upper_bound)
*
*	BOOL RemMulticastRange(struct DevUnit *,UBYTE *,UBYTE *);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

BOOL RemMulticastRange(struct DevUnit *unit,UBYTE *lower_bound,
   UBYTE *upper_bound,struct DevBase *base)
{
   struct AddressRange *range;
   ULONG lower_bound_left,upper_bound_left;
   UWORD lower_bound_right,upper_bound_right;

   lower_bound_left=BELong(*((ULONG *)lower_bound));
   lower_bound_right=BEWord(*((UWORD *)(lower_bound+4)));
   upper_bound_left=BELong(*((ULONG *)upper_bound));
   upper_bound_right=BEWord(*((UWORD *)(upper_bound+4)));

   range=FindMulticastRange(unit,lower_bound_left,lower_bound_right,
      upper_bound_left,upper_bound_right,base);

   if(range!=NULL)
   {
      if(--range->add_count==0)
      {
         Disable();
         Remove((APTR)range);
         Enable();
         FreeMem(range,sizeof(struct AddressRange));

         if(--unit->range_count==0)
         {
            unit->rx_filter_cmd&=~EL3CMD_SETRXFILTERF_MCAST;
            LEWordOut(unit->io_base+EL3REG_COMMAND,unit->rx_filter_cmd);
         }
      }
   }

   return range!=NULL;
}



/****i* 3c589.device/FindMulticastRange *************************************
*
*   NAME
*	FindMulticastRange -- .
*
*   SYNOPSIS
*	range = FindMulticastRange(unit,lower_bound_left,
*	    lower_bound_right,upper_bound_left,upper_bound_right)
*
*	struct AddressRange *FindMulticastRange(struct DevUnit *,ULONG,
*	    UWORD,ULONG,UWORD);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static struct AddressRange *FindMulticastRange(struct DevUnit *unit,
   ULONG lower_bound_left,UWORD lower_bound_right,ULONG upper_bound_left,
   UWORD upper_bound_right,struct DevBase *base)
{
   struct AddressRange *range,*tail;
   BOOL found;

   range=(APTR)unit->multicast_ranges.mlh_Head;
   tail=(APTR)&unit->multicast_ranges.mlh_Tail;
   found=FALSE;

   while((range!=tail)&&!found)
   {
      if((lower_bound_left==range->lower_bound_left)&&
         (lower_bound_right==range->lower_bound_right)&&
         (upper_bound_left==range->upper_bound_left)&&
         (upper_bound_right==range->upper_bound_right))
         found=TRUE;
      else
         range=(APTR)range->node.mln_Succ;
   }

   if(!found)
      range=NULL;

   return range;
}



/****i* 3c589.device/FindTypeStats *****************************************
*
*   NAME
*	FindTypeStats -- .
*
*   SYNOPSIS
*	stats = FindTypeStats(unit,list,
*	    packet_type)
*
*	struct TypeStats *FindTypeStats(struct DevUnit *,struct MinList *,
*	    ULONG);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

struct TypeStats *FindTypeStats(struct DevUnit *unit,struct MinList *list,
   ULONG packet_type,struct DevBase *base)
{
   struct TypeStats *stats,*tail;
   BOOL found;

   stats=(APTR)list->mlh_Head;
   tail=(APTR)&list->mlh_Tail;
   found=FALSE;

   while((stats!=tail)&&!found)
   {
      if(stats->packet_type==packet_type)
         found=TRUE;
      else
         stats=(APTR)stats->node.mln_Succ;
   }

   if(!found)
      stats=NULL;

   return stats;
}



/****i* 3c589.device/FlushUnit *********************************************
*
*   NAME
*	FlushUnit -- .
*
*   SYNOPSIS
*	FlushUnit(unit)
*
*	VOID FlushUnit(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

VOID FlushUnit(struct DevUnit *unit,UBYTE last_queue,BYTE error,
   struct DevBase *base)
{
   struct IORequest *request;
   UBYTE i;
   struct Opener *opener,*tail;

   /* Abort queued requests */

   for(i=0;i<=last_queue;i++)
   {
      while((request=(APTR)GetMsg(unit->request_ports[i]))!=NULL)
      {
         request->io_Error=IOERR_ABORTED;
         ReplyMsg((APTR)request);
      }
   }

#if 1
   opener=(APTR)unit->openers.mlh_Head;
   tail=(APTR)&unit->openers.mlh_Tail;

   /* Flush every opener's read queue */

   while(opener!=tail)
   {
      while((request=(APTR)GetMsg(&opener->read_port))!=NULL)
      {
         request->io_Error=error;
         ReplyMsg((APTR)request);
      }
      opener=(APTR)opener->node.mln_Succ;
   }

#else
   opener=request->ios2_BufferManagement;
   while((request=(APTR)GetMsg(&opener->read_port))!=NULL)
   {
      request->io_Error=IOERR_ABORTED;
      ReplyMsg((APTR)request);
   }
#endif

   /* Return */

   return;
}



/****i* 3c589.device/CardRemovedInt ****************************************
*
*   NAME
*	CardRemovedInt -- .
*
*   SYNOPSIS
*	CardRemovedInt(unit)
*
*	VOID CardRemovedInt(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static VOID CardRemovedInt(struct DevUnit *unit REG("a1"))
{
   struct DevBase *base;

   /* Record loss of card and get our task to call ReleaseCard() */

   base=unit->device;
   unit->flags&=~(UNITF_HAVEADAPTER|UNITF_HAVECARD|UNITF_ONLINE);
   Signal(unit->task,unit->card_removed_signal);

   return;
}



/****i* 3c589.device/CardInsertedInt ***************************************
*
*   NAME
*	CardInsertedInt -- .
*
*   SYNOPSIS
*	CardInsertedInt(unit)
*
*	VOID CardInsertedInt(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static VOID CardInsertedInt(struct DevUnit *unit REG("a1"))
{
   struct DevBase *base;

   base=unit->device;

   Signal(unit->task,unit->card_inserted_signal);

   return;
}



/****i* 3c589.device/CardStatusInt *****************************************
*
*   NAME
*	CardStatusInt -- .
*
*   SYNOPSIS
*	mask = CardStatusInt(mask,unit)
*
*	UBYTE CardStatusInt(UBYTE mask,struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	mask
*	unit
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static UBYTE CardStatusInt(UBYTE mask REG("d0"),
   struct DevUnit *unit REG("a1"))
{
   struct DevBase *base;
   volatile UBYTE *io_base;
   UWORD ints;

   base=unit->device;

   if(((unit->flags&UNITF_HAVEADAPTER)!=0)&&((mask&CARD_STATUSF_IRQ)!=0))
   {
      io_base=unit->io_base;
      ints=LEWordIn(io_base+EL3REG_STATUS);

      if((ints&EL3INTF_ANY)!=0)   /* ??? */
      {
         /* Handle interrupts */

         if((ints&EL3INTF_RXCOMPLETE)!=0)
         {
            LEWordOut(io_base+EL3REG_COMMAND,
               EL3CMD_SETINTMASK|(INT_MASK&~EL3INTF_RXCOMPLETE));
            Cause(&unit->rx_int);
         }
         if((ints&EL3INTF_TXAVAIL)!=0)
         {
            LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_ACKINT|
               EL3INTF_TXAVAIL);
            Cause(&unit->tx_int);
         }
         if((ints&EL3INTF_TXCOMPLETE)!=0)
            TxError(unit,base);

         /* Acknowledge interrupt request */

         LEWordOut(io_base+EL3REG_COMMAND,
            EL3CMD_ACKINT|EL3INTF_ANY);
      }

      /* Work around gayle interrupt bug */

      *((volatile UBYTE *)0xda9000)=(mask^0x2c)|0xc0;
      mask=0;
   }

   return mask;
}



/****i* 3c589.device/RxInt *************************************************
*
*   NAME
*	RxInt -- .
*
*   SYNOPSIS
*	RxInt(unit)
*
*	VOID RxInt(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit - A unit of this device.
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static VOID RxInt(struct DevUnit *unit REG("a1"))
{
   volatile UBYTE *io_base;
   UWORD rx_status,packet_size;
   struct DevBase *base;
   BOOL is_orphan,accepted;
   ULONG packet_type,*p,*end;
   UBYTE *buffer;
   struct IOSana2Req *request,*request_tail;
   struct Opener *opener,*opener_tail;
   struct TypeStats *tracker;

   base=unit->device;
   io_base=unit->io_base;
   buffer=unit->rx_buffer;
   end=(ULONG *)(buffer+HEADER_SIZE);

   while(((rx_status=LEWordIn(io_base+EL3REG_RXSTATUS))
      &EL3REG_RXSTATUSF_INCOMPLETE)==0)
   {
      if((rx_status&EL3REG_RXSTATUSF_ERROR)==0)
      {
         /* Read packet header */

         is_orphan=TRUE;
         packet_size=rx_status&EL3REG_RXSTATUS_SIZEMASK;
         p=(ULONG *)buffer;
         while(p<end)
            *p++=LongIn(io_base+EL3REG_DATA0);

         if(AddressFilter(unit,buffer+PACKET_DEST,base))
         {
            packet_type=BEWord(*((UWORD *)(buffer+PACKET_TYPE)));

            opener=(APTR)unit->openers.mlh_Head;
            opener_tail=(APTR)&unit->openers.mlh_Tail;

            /* Offer packet to every opener */

            while(opener!=opener_tail)
            {
               request=(APTR)opener->read_port.mp_MsgList.lh_Head;
               request_tail=(APTR)&opener->read_port.mp_MsgList.lh_Tail;
               accepted=FALSE;

               /* Offer packet to each request until it's accepted */

               while((request!=request_tail)&&!accepted)
               {
                  if(request->ios2_PacketType==packet_type
                     ||request->ios2_PacketType<=MTU
                     &&packet_type<=MTU)
                  {
                     CopyPacket(unit,request,packet_size,packet_type,
                        !is_orphan,base);
                     accepted=TRUE;
                  }
                  request=
                     (APTR)request->ios2_Req.io_Message.mn_Node.ln_Succ;
               }

               if(accepted)
                  is_orphan=FALSE;
               opener=(APTR)opener->node.mln_Succ;
            }

            /* If packet was unwanted, give it to S2_READORPHAN request */

            if(is_orphan)
            {
               unit->stats.UnknownTypesReceived++;
               if(!IsMsgPortEmpty(unit->request_ports[ADOPT_QUEUE]))
               {
                  CopyPacket(unit,
                     (APTR)unit->request_ports[ADOPT_QUEUE]->
                     mp_MsgList.lh_Head,packet_size,packet_type,
                     FALSE,base);
               }
            }

            /* Update remaining statistics */

            unit->stats.PacketsReceived++;

            tracker=
               FindTypeStats(unit,&unit->type_trackers,packet_type,base);
            if(tracker!=NULL)
            {
               tracker->stats.PacketsReceived++;
               tracker->stats.BytesReceived+=packet_size;
            }
         }
      }
      else
      {
         unit->stats.BadData++;
         ReportEvents(unit,S2EVENT_ERROR|S2EVENT_HARDWARE|S2EVENT_RX,base);
      }

      /* Discard packet */

      Disable();   /* Needed? */
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_RXDISCARD);
      while((LEWordIn(io_base+EL3REG_STATUS)&
         EL3REG_STATUSF_CMDINPROGRESS)!=0);
      Enable();
   }

   /* Return */

   LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SETINTMASK|INT_MASK);
   return;
}



/****i* 3c589.device/CopyPacket ********************************************
*
*   NAME
*	CopyPacket -- .
*
*   SYNOPSIS
*	CopyPacket(unit,request,packet_size,packet_type,
*	    all_read)
*
*	VOID CopyPacket(struct DevUnit *,struct IOSana2Req *,UWORD,UWORD,
*	    BOOL);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static VOID CopyPacket(struct DevUnit *unit,struct IOSana2Req *request,
   UWORD packet_size,UWORD packet_type,BOOL all_read,struct DevBase *base)
{
   volatile UBYTE *io_base;
   struct Opener *opener;
   UBYTE *buffer;
   BOOL filtered=FALSE;
   ULONG *p,*end;

   /* Set multicast and broadcast flags */

   io_base=unit->io_base;
   buffer=unit->rx_buffer;
   request->ios2_Req.io_Flags&=~(SANA2IOF_BCAST|SANA2IOF_MCAST);
   if((*((ULONG *)(buffer+PACKET_DEST))==0xffffffff)&&
      (*((UWORD *)(buffer+PACKET_DEST+4))==0xffff))
      request->ios2_Req.io_Flags|=SANA2IOF_BCAST;
   else if((buffer[PACKET_DEST]&0x1)!=0)
      request->ios2_Req.io_Flags|=SANA2IOF_MCAST;

   /* Set source and destination addresses and packet type */

   CopyMem(buffer+PACKET_SOURCE,request->ios2_SrcAddr,ADDRESS_SIZE);
   CopyMem(buffer+PACKET_DEST,request->ios2_DstAddr,ADDRESS_SIZE);
   request->ios2_PacketType=packet_type;

   /* Read rest of packet */

   if(!all_read)
   {
      p=(ULONG *)(buffer+((PACKET_DATA+3)&~3));
      end=(ULONG *)(buffer+packet_size);
      while(p<end)
         *p++=LongIn(io_base+EL3REG_DATA0);
   }

   if((request->ios2_Req.io_Flags&SANA2IOF_RAW)==0)
   {
      packet_size-=PACKET_DATA;
      buffer+=PACKET_DATA;
   }
#ifdef USE_HACKS
   else
      packet_size+=4;   /* Needed for Shapeshifter & Fusion */
#endif

   request->ios2_DataLength=packet_size;

   /* Filter packet */

   opener=request->ios2_BufferManagement;
   if((request->ios2_Req.io_Command==CMD_READ)&&
      (opener->filter_hook!=NULL))
      if(!CallHookPkt(opener->filter_hook,request,buffer))
         filtered=TRUE;

   if(!filtered)
   {
      /* Copy packet into opener's buffer and reply packet */

      if(!opener->rx_function(request->ios2_Data,buffer,packet_size))
      {
         request->ios2_Req.io_Error=S2ERR_NO_RESOURCES;
         request->ios2_WireError=S2WERR_BUFF_ERROR;
         ReportEvents(unit,
            S2EVENT_ERROR|S2EVENT_SOFTWARE|S2EVENT_BUFF|S2EVENT_RX,base);
      }
      Remove((APTR)request);
      ReplyMsg((APTR)request);

   }

   return;
}



/****i* 3c589.device/AddressFilter *****************************************
*
*   NAME
*	AddressFilter -- .
*
*   SYNOPSIS
*	accept = AddressFilter(unit,address)
*
*	BOOL AddressFilter(struct DevUnit *,UBYTE *);
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static BOOL AddressFilter(struct DevUnit *unit,UBYTE *address,
   struct DevBase *base)
{
   struct AddressRange *range,*tail;
   BOOL accept=TRUE;
   ULONG address_left;
   UWORD address_right;

   /* Check whether address is unicast/broadcast or multicast */

   address_left=BELong(*((ULONG *)address));
   address_right=BEWord(*((UWORD *)(address+4)));

   if(((address_left&0x01000000)!=0)&&
      !((address_left==0xffffffff)&&(address_right==0xffff)))
   {
      /* Check if this multicast address is wanted */

      range=(APTR)unit->multicast_ranges.mlh_Head;
      tail=(APTR)&unit->multicast_ranges.mlh_Tail;
      accept=FALSE;

      while((range!=tail)&&!accept)
      {
         if(((address_left>range->lower_bound_left)||
            (address_left==range->lower_bound_left)&&
            (address_right>=range->lower_bound_right))&&
            ((address_left<range->upper_bound_left)||
            (address_left==range->upper_bound_left)&&
            (address_right<=range->upper_bound_right)))
            accept=TRUE;
         range=(APTR)range->node.mln_Succ;
      }

      if(!accept)
         unit->special_stats[S2SS_ETHERNET_BADMULTICAST&0xffff]++;
   }

   return accept;
}



/****i* 3c589.device/TxInt *************************************************
*
*   NAME
*	TxInt -- .
*
*   SYNOPSIS
*	TxInt(unit)
*
*	VOID TxInt(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit - A unit of this device.
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static VOID TxInt(struct DevUnit *unit REG("a1"))
{
   volatile UBYTE *io_base;
   UWORD packet_size,data_size,send_size;
   struct DevBase *base;
   struct IOSana2Req *request;
   BOOL proceed=TRUE;
   struct Opener *opener;
   ULONG *buffer,*end,wire_error;
   ULONG *(*dma_tx_function)(APTR REG("a0"));
   BYTE error;
   struct MsgPort *port;
   struct TypeStats *tracker;

   base=unit->device;
   io_base=unit->io_base;

   port=unit->request_ports[WRITE_QUEUE];

   while(proceed&&(!IsMsgPortEmpty(port)))
   {
      error=0;

      request=(APTR)port->mp_MsgList.lh_Head;
      data_size=packet_size=request->ios2_DataLength;

      if((request->ios2_Req.io_Flags&SANA2IOF_RAW)==0)
         packet_size+=PACKET_DATA;

      if(LEWordIn(io_base+EL3REG_TXSPACE)>PREAMBLE_SIZE+packet_size)
      {
         /* Write packet preamble */

         LELongOut(io_base+EL3REG_DATA0,packet_size);

         /* Write packet header */

         send_size=(packet_size+3)&(~0x3);
         if((request->ios2_Req.io_Flags&SANA2IOF_RAW)==0)
         {
            LongOut(io_base+EL3REG_DATA0,*((ULONG *)request->ios2_DstAddr));
            WordOut(io_base+EL3REG_DATA0,
               *((UWORD *)(request->ios2_DstAddr+4)));
            WordOut(io_base+EL3REG_DATA0,*((UWORD *)unit->address));
            LongOut(io_base+EL3REG_DATA0,*((ULONG *)(unit->address+2)));
            BEWordOut(io_base+EL3REG_DATA0,request->ios2_PacketType);
            send_size-=PACKET_DATA;
         }

         /* Get packet data */

         opener=(APTR)request->ios2_BufferManagement;
         dma_tx_function=opener->dma_tx_function;
         if(dma_tx_function!=NULL)
            buffer=dma_tx_function(request->ios2_Data);
         else
            buffer=NULL;

         if(buffer==NULL)
         {
            buffer=(ULONG *)unit->tx_buffer;
            if(!opener->tx_function(buffer,request->ios2_Data,data_size))
            {
               error=S2ERR_NO_RESOURCES;
               wire_error=S2WERR_BUFF_ERROR;
               ReportEvents(unit,
                  S2EVENT_ERROR|S2EVENT_SOFTWARE|S2EVENT_BUFF|S2EVENT_TX,
                  base);
            }
         }

         /* Write packet data */

         if(error==0)
         {
            end=buffer+(send_size>>2);
            while(buffer<end)
               LongOut(io_base+EL3REG_DATA0,*buffer++);

            if((send_size&0x3)!=0)
               WordOut(io_base+EL3REG_DATA0,*((UWORD *)buffer));
         }

         /* Reply packet */

         request->ios2_Req.io_Error=error;
         request->ios2_WireError=wire_error;
         Remove((APTR)request);
         ReplyMsg((APTR)request);

         /* Update statistics */

         if(error==0)
         {
            unit->stats.PacketsSent++;

            tracker=FindTypeStats(unit,&unit->type_trackers,
               request->ios2_PacketType,base);
            if(tracker!=NULL)
            {
               tracker->stats.PacketsSent++;
               tracker->stats.BytesSent+=packet_size;
            }
         }
      }
      else
         proceed=FALSE;
   }

   if(proceed)
      unit->request_ports[WRITE_QUEUE]->mp_Flags=PA_SOFTINT;
   else
   {
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_SETTXTHRESH
         |(PREAMBLE_SIZE+packet_size));
      unit->request_ports[WRITE_QUEUE]->mp_Flags=PA_IGNORE;
   }

   return;
}



/****i* 3c589.device/TxError ***********************************************
*
*   NAME
*	TxError -- .
*
*   SYNOPSIS
*	TxError(unit)
*
*	VOID TxError(struct DevUnit *);
*
*   FUNCTION
*
*   INPUTS
*	unit - A unit of this device.
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

VOID TxError(struct DevUnit *unit,struct DevBase *base)
{
   volatile UBYTE *io_base;
   UBYTE tx_status,flags=0;

   io_base=unit->io_base;

   /* Gather all errors */

   while(((tx_status=ByteIn(io_base+EL3REG_TXSTATUS))
      &EL3REG_TXSTATUSF_COMPLETE)!=0)
   {
      flags|=tx_status;
      ByteOut(io_base+EL3REG_TXSTATUS,0);
   }

   /* Restart transmitter if necessary */

   if((flags&EL3REG_TXSTATUSF_JABBER)!=0)
   {
      Disable();
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_TXRESET);
      while((LEWordIn(io_base+EL3REG_STATUS)&
         EL3REG_STATUSF_CMDINPROGRESS)!=0);
      Enable();
   }

   if((flags&(EL3REG_TXSTATUSF_JABBER|EL3REG_TXSTATUSF_OVERFLOW))!=0)
      LEWordOut(io_base+EL3REG_COMMAND,EL3CMD_TXENABLE);

   /* Report the error(s) */

   ReportEvents(unit,S2EVENT_ERROR|S2EVENT_HARDWARE|S2EVENT_TX,base);

   return;
}



/****i* 3c589.device/ReportEvents ******************************************
*
*   NAME
*	ReportEvents -- .
*
*   SYNOPSIS
*	ReportEvents(unit,events)
*
*	VOID ReportEvents(struct DevUnit *,ULONG);
*
*   FUNCTION
*
*   INPUTS
*	unit - A unit of this device.
*	events - A mask of events to report.
*
*   RESULT
*	None.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static VOID ReportEvents(struct DevUnit *unit,ULONG events,
   struct DevBase *base)
{
   struct IOSana2Req *request,*tail,*next_request;
   struct List *list;

   list=&unit->request_ports[EVENT_QUEUE]->mp_MsgList;
   next_request=(APTR)list->lh_Head;
   tail=(APTR)&list->lh_Tail;

   Disable();
   while(next_request!=tail)
   {
      request=next_request;
      next_request=(APTR)request->ios2_Req.io_Message.mn_Node.ln_Succ;

      if((request->ios2_WireError&events)!=0)
      {
         request->ios2_WireError=events;
         Remove((APTR)request);
         ReplyMsg((APTR)request);
      }
   }
   Enable();

   return;
}



/****i* 3c589.device/UnitTask **********************************************
*
*   NAME
*	UnitTask -- .
*
*   SYNOPSIS
*	UnitTask()
*
*	VOID UnitTask();
*
*   FUNCTION
*
*   INPUTS
*
*   RESULT
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static VOID UnitTask()
{
   struct Task *task;
   struct IORequest *request;
   struct DevUnit *unit;
   struct DevBase *base;
   struct MsgPort *general_port;
   ULONG signals,wait_signals,card_removed_signal,card_inserted_signal,
      general_port_signal;

   /* Get parameters */

   task=AbsExecBase->ThisTask;
   unit=task->tc_UserData;
   base=unit->device;

   /* Activate general request port */

   general_port=unit->request_ports[GENERAL_QUEUE];
   general_port->mp_SigTask=task;
   general_port->mp_SigBit=AllocSignal(-1);
   general_port_signal=1<<general_port->mp_SigBit;
   general_port->mp_Flags=PA_SIGNAL;

   /* Allocate a signal for notification of card removal */

   card_removed_signal=unit->card_removed_signal=1<<AllocSignal(-1);
   card_inserted_signal=unit->card_inserted_signal=1<<AllocSignal(-1);
   wait_signals=(1<<general_port->mp_SigBit)|card_removed_signal
      |card_inserted_signal;

   /* Tell ourselves to check port for old messages */

   Signal(task,general_port_signal);

   /* Infinite loop to service requests and signals */

   while(TRUE)
   {
      signals=Wait(wait_signals);

      if((signals&card_inserted_signal)!=0)
      {
         if(InitialiseCard(unit,base))
         {
            unit->flags|=UNITF_HAVECARD;
            if((unit->flags&UNITF_CONFIGURED)!=0)
               ConfigureAdapter(unit,base);
         }
         else
            ReleaseCard(unit->card_handle,0);
      }

      if((signals&card_removed_signal)!=0)
      {
         ReleaseCard(unit->card_handle,0);
         GoOffline(unit,base);
      }

      if((signals&general_port_signal)!=0)
      {
         while((request=(APTR)GetMsg(general_port))!=NULL)
         {
            /* Service the request as soon as the unit is free */

            ObtainSemaphore(&unit->access_lock);
            ServiceRequest((APTR)request,base);
         }
      }
   }
}



/****i* 3c589.device/ReadEEPROM ********************************************
*
*   NAME
*	ReadEEPROM -- .
*
*   SYNOPSIS
*	value = ReadEEPROM(io_base,index)
*
*	UWORD ReadEEPROM(UBYTE *,UWORD);
*
*   FUNCTION
*
*   INPUTS
*	io_base - Base of adapter's register window.
*	index - Offset within EEPROM.
*
*   RESULT
*	value - Contents of specified EEPROM location.
*
*   EXAMPLE
*
*   NOTES
*
*   BUGS
*
*   SEE ALSO
*
****************************************************************************
*
*/

static UWORD ReadEEPROM(volatile UBYTE *io_base,UWORD index)
{
   LEWordOut(io_base+EL3REG_EEPROMCMD,EL3ECMD_READ|index);
   while((LEWordIn(io_base+EL3REG_EEPROMCMD)&EL3REG_EEPROMCMDF_BUSY)
      !=0);
   return LEWordIn(io_base+EL3REG_EEPROMDATA);
}



