#ifndef WCND_MARLIN2_
#define WCND_MARLIN2_


#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cutils/sockets.h>
#include <ctype.h>
#include <pthread.h>
#include <errno.h>
#include <cutils/properties.h>
#include <cutils/log.h>
#include <signal.h>

#include <sys/capability.h>
#include <linux/prctl.h>
#include "wcnd.h"
#include "wcnd_util.h"
#include "wcnd_sm.h"

#define LOG_TAG "WCND"

// the CP2(WIFI/BT/FM) device node
#define WCN_DEV "NOT-USED"

// when what to read is that has been to write, then it indicate the
// CP2(WIFI/BT/FM) is OK
#define WCN_LOOP_IFACE "/proc/mdbg/loopcheck"

// when assert happens, related information will write to this interface
#define WCN_ASSERT_IFACE "/proc/mdbg/assert"

// The interface to communicate with slog
#define WCN_SLOG_IFACE "NOT-USED"

// write '1' to this interface, the CP2(WIFI/BT/FM) will stop
#define WCN_STOP_IFACE "NOT-USED"

// write '1' to this interface, the CP2(WIFI/BT/FM) will start to boot up
#define WCN_START_IFACE "NOT-USED"

// write image file to this interface, then the image will be download to
// CP2(WIFI/BT/FM)
#define WCN_DOWNLOAD_IFACE "NOT-USED"

// read from this interface, the memory infor will be read from CP2(WIFI/BT/FM)
#define WCN_DUMP_IFACE "NOT-USED"


// can read the status infor from this interface
#define WCN_STATUS_IFACE "NOT-USED"

// send at cmd to this interface
#define WCN_ATCMD_IFACE "/proc/mdbg/at_cmd"

#define LOOP_TEST_STR "at+loopcheck\r"
#define LOOP_TEST_ACK_STR "loopcheck_ack"



#define WCND_CP2_EXCEPTION_STRING "WCN-CP2-EXCEPTION"
#define WCND_CP2_RESET_START_STRING "WCN-CP2-RESET-START"
#define WCND_CP2_RESET_END_STRING "WCN-CP2-RESET-END"
#define WCND_CP2_ALIVE_STRING "WCN-CP2-ALIVE"
#define WCND_CP2_CLOSED_STRING "WCN-CP2-CLOSED"
#define WCND_GE2_EXCEPTION_STRING "WCN-GE2-EXCEPTION"

// CP2 AT Cmds
#define WCND_ATCMD_CP2_SLEEP "at+cp2sleep\r"
#define WCND_ATCMD_CP2_DISABLE_LOG "AT+ARMLOG=0\r"
#define WCND_ATCMD_CP2_ENABLE_LOG "AT+ARMLOG=1\r"
#define WCND_ATCMD_CP2_GET_VERSION "at+spatgetcp2info\r"
#define WCND_ATCMD_CP2_ENTER_USER "at+cp2_enter_user=1\r"
#define WCND_ATCMD_CP2_EXIT_USER "at+cp2_enter_user=0\r"
#define WCND_ATCMD_CP2_GET_CHIP  "at+getchipversion"
#define WCND_ATCMD_REBOOT_MARLIN "rebootmarlin"
#define WCND_ATCMD_REBOOT_WCN "rebootwcn"
#define WCND_ATCMD_CP2_LOGLEVEL "at+loglevel="
#define WCND_ATCMD_RESPONSE_ARMLOG "+ARMLOG:"
#define WCND_ATCMD_RESPONSE_LOGLEVEL "+LOGLEVEL:"
#define WCND_ATCMD_CP2_GET_LOG "at+armlog?"
#define WCND_ATCMD_CP2_GET_LOGLEVEL "at+loglevel?"


// cmd id to identify the cmds that the wcnd care about
#define WCND_ATCMD_CMDID_OTHERS (0)
#define WCND_ATCMD_CMDID_GET_VERSION (1)
#define WCND_ATCMD_CMDID_SLEEP (2)
#define WCND_ATCMD_CMDID_GET_CHIP (3)
#define WCND_ATCMD_CMDID_GET_LOG (4)
#define WCND_ATCMD_CMDID_GET_LOGLEVEL (5)
#define WCND_ATCMD_CMDID_SET_LOGLEVEL (6)

#define MARLIN2_POWEROFF_IDNENTITY "poweroff"

#define WCND_CP2_LOG_ENABLE  1
#define WCND_CP2_LOG_DISABLE 0
#define WCND_CP2_LOG_DEFAULE  2

#define WCND_TO_NONE_ACTION_CP2 -1
#define WCND_TO_RESET_CP2 0
#define WCND_TO_DUMP_CP2 1

#define WCND_AVOID_RESP_WITH_INVALID_FD -1

#define WCND_KILL_FM_PROCESS_TIMEOUT  5 //The unit is Second

#define WCND_RESET_PROP_KEY "persist.vendor.sys.wcnreset"
#define WCND_SLOG_ENABLE_PROP_KEY "persist.vendor.sys.wcnlog"
#define WCND_LOGLEVEL_PROP_KEY "persist.vendor.sys.loglevel"
#define WCND_PROPERTY_CP2_LOG  "persist.vendor.sys.cp2log"

#define WCND_SLOG_RESULT_PROP_KEY "persist.vendor.sys.wcnlog.result"
//#define WCND_ENGCTRL_PROP_KEY "persist.sys.engpc.disable"

#define WCND_CP2_STATE_PROP_KEY "persist.vendor.sys.wcn.status"


#define WCND_CP2_DEFAULT_CP2_VERSION_INFO "Fail: UNKNOW VERSION"
#define WCND_DEBUG_RESET_KEY "persist.vendor.sys.debug.reset"

#define WCND_WIFI_SUPPLICANT_PROCESS  "/vendor/bin/hw/wpa_supplicant"
#define WCND_BT_BLUETOOTH_PROCESS     "com.android.bluetooth"
#define WCND_FM_FMRADIO_PROCESS       "com.android.fmradio"


int init(WcndManager *pWcndManger, int is_eng_only);
void *cp2_listen_thread(void *arg);
int is_cp2_alive_ok(WcndManager *pWcndManger, int is_loopcheck);
void *cp2_loop_check_thread(void *arg);
int store_cp2_version_info(WcndManager *pWcndManger);
void handle_ge2_error(WcndManager *pWcndManger,char *info_str);
int wcnd_config_cp2_bootup(WcndManager *pWcndManger);

int check_if_reset_enable(WcndManager *pWcndManger);

int wcnd_reboot_cp2(WcndManager *pWcndManger);
int prepare_cp2_dump(WcndManager *pWcndManger);
int wcnd_dump_cp2_mem(WcndManager *pWcndManger);
int wcnd_reset_cp2(WcndManager *pWcndManger) ;

int wcnd_dump_cp2(WcndManager *pWcndManger);
int wcnd_do_wcn_reset_process(WcndManager *pWcndManger);
int wcnd_process_atcmd(int client_fd, char *atcmd_str,
                       WcndManager *pWcndManger);
int wcnd_send_notify_to_client(WcndManager *pWcndManger, char *info_str,
                               int notify_type);
int wcnd_set_marlin2_poweron(WcndManager *pWcndManger, int poweron);
int notify_cp2_exception(WcndManager *pWcndManger, char *info_str);
int wcnd_runcommand(int client_fd, int argc, char *argv[]);

int send_atcmd(WcndManager *pWcndManger, char *atcmd_str);
void wait_for_dump_logs(void);
int store_cp2_version_info(WcndManager *pWcndManger);

#endif

