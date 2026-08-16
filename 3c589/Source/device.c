/*

File: device.c
Author: Neil Cafferkey
Copyright (C) 2000-2010 Neil Cafferkey

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


#include <exec/types.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <utility/utility.h>
#include "initializers.h"

#include <proto/exec.h>
#include <proto/alib.h>
#include <proto/utility.h>

#include "device.h"

#include "unit_protos.h"
#include "request_protos.h"

#define VERSION 1
#define REVISION 5
#define UNIT_COUNT 1
#define UTILITY_VERSION 36
#define PCCARD_VERSION 1


/* Private prototypes */

static struct DevBase *DevInit(struct DevBase *dev_base REG("d0"),
   APTR seg_list REG("a0"),struct DevBase *base REG(BASE_REG));
static BYTE DevOpen(ULONG unit_no REG("d0"),
   struct IOSana2Req *request REG("a1"),ULONG flags REG("d1"),
   struct DevBase *base REG(BASE_REG));
static APTR DevClose(struct IOSana2Req *request REG("a1"),
   struct DevBase *base REG(BASE_REG));
static APTR DevExpunge(struct DevBase *base REG(BASE_REG));
static APTR DevReserved();
static VOID DevBeginIO(struct IOSana2Req *request REG("a1"),
   struct DevBase *base REG(BASE_REG));
static VOID DevAbortIO(struct IOSana2Req *request REG("a1"),
   struct DevBase *base REG(BASE_REG));
VOID DeleteDevice(struct DevBase *base);

extern const APTR vectors[];
extern const APTR init_table[];


/* Return an error immediately if someone tries to run the device */

LONG Main()
{
   return -1;
}

#define DEVICE_NAME "3c589.device"

const TEXT device_name[]=DEVICE_NAME;
static const TEXT version_string[]=DEVICE_NAME " 1.5 (3.2.2010)\n";
static const TEXT card_name[]=CARDRESNAME;
static const TEXT utility_name[]=UTILITYNAME;
static const TEXT pccard_name[]=PCCARDNAME;
static const TEXT timer_name[]=TIMERNAME;


const struct Resident device_rom_tag=
{
   RTC_MATCHWORD,
   (struct Resident *)&device_rom_tag,
   (APTR)init_table,
   RTF_AUTOINIT,
   VERSION,
   NT_DEVICE,
   0,
   (STRPTR)device_name,
   (STRPTR)version_string,
   (APTR)init_table
};


static const APTR vectors[]=
{
   (APTR)DevOpen,
   (APTR)DevClose,
   (APTR)DevExpunge,
   (APTR)DevReserved,
   (APTR)DevBeginIO,
   (APTR)DevAbortIO,
   (APTR)-1
};


static const struct
{
   SMALLINITBYTEDEF(type);
   SMALLINITPINTDEF(name);
   SMALLINITBYTEDEF(flags);
   SMALLINITWORDDEF(version);
   SMALLINITWORDDEF(revision);
   SMALLINITPINTDEF(id_string);
   INITENDDEF;
}
init_data=
{
   SMALLINITBYTE(OFFSET(Node,ln_Type),NT_DEVICE),
   SMALLINITPINT(OFFSET(Node,ln_Name),device_name),
   SMALLINITBYTE(OFFSET(Library,lib_Flags),LIBF_SUMUSED|LIBF_CHANGED),
   SMALLINITWORD(OFFSET(Library,lib_Version),VERSION),
   SMALLINITWORD(OFFSET(Library,lib_Revision),REVISION),
   SMALLINITPINT(OFFSET(Library,lib_IdString),version_string),
   INITEND
};


static const APTR init_table[]=
{
   (APTR)sizeof(struct DevBase),
   (APTR)vectors,
   (APTR)&init_data,
   (APTR)DevInit
};


static const ULONG rx_tags[]=
{
   S2_CopyToBuff,
   S2_CopyToBuff16
};


static const ULONG tx_tags[]=
{
   S2_CopyFromBuff,
   S2_CopyFromBuff16,
   S2_CopyFromBuff32
};



/****i* 3c589.device/DevInit ***********************************************
*
*   NAME
*	DevInit -- .
*
*   SYNOPSIS
*	dev_base = DevInit(dev_base,seg_list)
*
*	struct DevBase *DevInit(struct DevBase *,APTR);
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

static struct DevBase *DevInit(struct DevBase *dev_base REG("d0"),
   APTR seg_list REG("a0"),struct DevBase *base REG(BASE_REG))
{
   BOOL success=TRUE;

   dev_base->sys_base=(APTR)base;
   base=dev_base;
   base->seg_list=seg_list;

   base->card_base=OpenResource(card_name);
   base->utility_base=(APTR)OpenLibrary(utility_name,UTILITY_VERSION);
   base->pccard_base=OpenLibrary(pccard_name,PCCARD_VERSION);
   if((base->card_base==NULL)||(base->utility_base==NULL)||
      (base->pccard_base==NULL))
      success=FALSE;

   if(OpenDevice(timer_name,UNIT_VBLANK,(APTR)&base->timer_request,0)!=0)
      success=FALSE;

   NewList((APTR)(&dev_base->units));

   if(!success)
   {
      DeleteDevice(base);
      base=NULL;
   }

   return base;
}



/****i* 3c589.device/DevOpen ***********************************************
*
*   NAME
*	DevOpen -- .
*
*   SYNOPSIS
*	error = DevOpen(unit_num,request,flags)
*
*	BYTE DevOpen(ULONG,struct IOSana2Req *,ULONG);
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

static BYTE DevOpen(ULONG unit_num REG("d0"),
   struct IOSana2Req *request REG("a1"),ULONG flags REG("d1"),
   struct DevBase *base REG(BASE_REG))
{
   struct DevUnit *unit;
   BYTE error=0;
   struct Opener *opener;
   struct TagItem *tag_list;
   UWORD i;

   base->device.dd_Library.lib_OpenCnt++;
   base->device.dd_Library.lib_Flags&=~LIBF_DELEXP;

   request->ios2_Req.io_Unit=NULL;
   tag_list=request->ios2_BufferManagement;
   request->ios2_BufferManagement=NULL;

   /* Check request size and unit number */

   if((request->ios2_Req.io_Message.mn_Length<sizeof(struct IOSana2Req))
      ||(unit_num>=UNIT_COUNT))
      error=IOERR_OPENFAIL;

   /* Get the requested unit */

   if(error==0)
   {
      request->ios2_Req.io_Unit=(APTR)unit=GetUnit(unit_num,base);
      if(unit==NULL)
         error=IOERR_OPENFAIL;
   }

   /* Handle device sharing */

   if(error==0)
   {
      if((unit->open_count!=0)&&(((unit->flags&UNITF_SHARED)==0)||
         ((flags&SANA2OPF_MINE)!=0)))
         error=IOERR_UNITBUSY;
      unit->open_count++;
   }

   if(error==0)
   {
      if((flags&SANA2OPF_MINE)==0)
         unit->flags|=UNITF_SHARED;
      else if((flags&SANA2OPF_PROM)!=0)
         unit->flags|=UNITF_PROM;

      /* Set up buffer-management structure and get hooks */

      request->ios2_BufferManagement=opener=
         AllocVec(sizeof(struct Opener),MEMF_PUBLIC);
      if(opener==NULL)
         error=IOERR_OPENFAIL;
   }

   if(error==0)
   {
      NewList(&opener->read_port.mp_MsgList);
      opener->read_port.mp_Flags=PA_IGNORE;
      NewList((APTR)&opener->initial_stats);

      for(i=0;i<2;i++)
         opener->rx_function=(APTR)GetTagData(rx_tags[i],
            (UPINT)opener->rx_function,tag_list);
      for(i=0;i<3;i++)
         opener->tx_function=(APTR)GetTagData(tx_tags[i],
            (UPINT)opener->tx_function,tag_list);

      opener->filter_hook=(APTR)GetTagData(S2_PacketFilter,NULL,tag_list);
      opener->dma_tx_function=
         (APTR)GetTagData(S2_DMACopyFromBuff32,NULL,tag_list);

      Disable();
      AddTail((APTR)&unit->openers,(APTR)opener);
      Enable();
   }

   /* Back out if anything went wrong */

   if(error!=0)
      DevClose(request,base);

   /* Return */

   request->ios2_Req.io_Error=error;
   return error;
}



/****i* 3c589.device/DevClose **********************************************
*
*   NAME
*	DevClose -- .
*
*   SYNOPSIS
*	seg_list = DevClose(request)
*
*	APTR DevClose(struct IOSana2Req *);
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

static APTR DevClose(struct IOSana2Req *request REG("a1"),
   struct DevBase *base REG(BASE_REG))
{
   struct DevUnit *unit;
   APTR seg_list;
   struct Opener *opener;

   /* Free buffer-management resources */

   opener=(APTR)request->ios2_BufferManagement;
   if(opener!=NULL)
   {
      Disable();
      Remove((APTR)opener);
      Enable();
      FreeVec(opener);
   }

   /* Delete the unit if it's no longer in use */

   unit=(APTR)request->ios2_Req.io_Unit;
   if(unit!=NULL)
   {
      if((--unit->open_count)==0)
      {
         Remove((APTR)unit);
         DeleteUnit(unit,base);
      }
   }

   /* Expunge the device if a delayed expunge is pending */

   seg_list=NULL;

   if((--base->device.dd_Library.lib_OpenCnt)==0)
   {
      if((base->device.dd_Library.lib_Flags&LIBF_DELEXP)!=0)
         seg_list=DevExpunge(base);
   }

   return seg_list;
}



/****i* 3c589.device/DevExpunge ********************************************
*
*   NAME
*	DevExpunge -- .
*
*   SYNOPSIS
*	seg_list = DevExpunge()
*
*	APTR DevExpunge(VOID);
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

static APTR DevExpunge(struct DevBase *base REG(BASE_REG))
{
   APTR seg_list;

   if(base->device.dd_Library.lib_OpenCnt==0)
   {
      seg_list=base->seg_list;
      Remove((APTR)base);
      DeleteDevice(base);
   }
   else
   {
      base->device.dd_Library.lib_Flags|=LIBF_DELEXP;
      seg_list=NULL;
   }

   return seg_list;
}



/****i* 3c589.device/DevReserved *******************************************
*
*   NAME
*	DevReserved -- .
*
*   SYNOPSIS
*	result = DevReserved()
*
*	APTR DevReserved(VOID);
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

static APTR DevReserved()
{
   return NULL;
}



/****i* 3c589.device/DevBeginIO ********************************************
*
*   NAME
*	DevBeginIO -- .
*
*   SYNOPSIS
*	DevBeginIO(request)
*
*	VOID DevBeginIO(struct IORequest *);
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

static VOID DevBeginIO(struct IOSana2Req *request REG("a1"),
   struct DevBase *base REG(BASE_REG))
{
   struct DevUnit *unit;

   request->ios2_Req.io_Error=0;
   unit=(APTR)request->ios2_Req.io_Unit;

   if(AttemptSemaphore(&unit->access_lock))
      ServiceRequest(request,base);
   else
   {
      PutRequest(unit->request_ports[GENERAL_QUEUE],(APTR)request,base);
   }

   return;
}



/****i* 3c589.device/DevAbortIO ********************************************
*
*   NAME
*	DevAbortIO -- Try to stop a request.
*
*   SYNOPSIS
*	DevAbortIO(request)
*
*	VOID DevAbortIO(struct IOSana2Req *);
*
*   FUNCTION
*	Do our best to halt the progress of a request.
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
* Disable() used instead of a semaphore because device uses interrupts.
*
*/

static VOID DevAbortIO(struct IOSana2Req *request REG("a1"),
   struct DevBase *base REG(BASE_REG))
{
   Disable();
   if((request->ios2_Req.io_Message.mn_Node.ln_Type==NT_MESSAGE)
      &&((request->ios2_Req.io_Flags&IOF_QUICK)==0))
   {
      Remove((APTR)request);
      request->ios2_Req.io_Error=IOERR_ABORTED;
      request->ios2_WireError=S2WERR_GENERIC_ERROR;
      ReplyMsg((APTR)request);
   }
   Enable();

   return;
}



/****i* 3c589.device/DeleteDevice ******************************************
*
*   NAME
*	DeleteDevice -- .
*
*   SYNOPSIS
*	DeleteDevice()
*
*	VOID DeleteDevice(VOID);
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

VOID DeleteDevice(struct DevBase *base)
{
   UWORD neg_size,pos_size;

   /* Close devices */

   CloseDevice((APTR)&base->timer_request);

   /* Close libraries */

   if(base->pccard_base!=NULL)
      CloseLibrary(base->pccard_base);
   if(base->utility_base!=NULL)
      CloseLibrary((APTR)base->utility_base);

   /* Free device's memory */

   neg_size=base->device.dd_Library.lib_NegSize;
   pos_size=base->device.dd_Library.lib_PosSize;
   FreeMem((UBYTE *)base-neg_size,pos_size+neg_size);

   return;
}



