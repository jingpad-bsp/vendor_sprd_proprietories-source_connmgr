#ifndef WCND_SM_H__
#define WCND_SM_H__

#include "wcnd.h"

// WCND STATE
#define WCND_STATE_CP2_STARTED 0
#define WCND_STATE_CP2_STOPPED 2
#define WCND_STATE_CP2_ASSERT 4

// WCND EVENT
#define WCND_EVENT_CP2_ASSERT 0x0020
#define WCND_EVENT_CP2_DOWN 0x0040
#define WCND_EVENT_PENGING_EVENT 0x0080
#define WCND_EVENT_CP2POWERON_REQ 0x0100
#define WCND_EVENT_CP2POWEROFF_REQ 0x0200
#define WCND_EVENT_MARLIN2_CLOSED 0x1000
#define WCND_EVENT_MARLIN2_OPENED 0x2000


/**
* NOTE: the commnad send to wcn, will prefix with "wcn ", such as "wcn BT-CLOSE"
*/


#define WCND_CMD_CP2_POWER_ON "poweron"
#define WCND_CMD_CP2_POWER_OFF "poweroff"

#define WCND_CMD_CP2_DUMP_ON "dump_enable"
#define WCND_CMD_CP2_DUMP_OFF "dump_disable"
#define WCND_CMD_CP2_DUMPMEM "dumpmem"
#define WCND_CMD_CP2_DUMPQUERY "dump?"

// Below cmd used internal
#define WCND_SELF_CMD_CP2_VERSION "getcp2version"

#define WCND_SELF_EVENT_CP2_ASSERT "cp2assert"
#define WCND_SELF_EVENT_MARLIN2_CLOSED "marlin2closed"
#define WCND_SELF_EVENT_MARLIN2_OPENED "marlin2opened"

#define WCND_CMD_RESPONSE_STRING "WCNBTWIFI-CMD"
#define WCND_CMD_EVENT_GE2_ERROR  "gnsserror"


int wcnd_sm_init(WcndManager *pWcndManger);
int wcnd_sm_step(WcndManager *pWcndManger, WcndMessage *pMessage);

#endif  // WCND_SM_H_
