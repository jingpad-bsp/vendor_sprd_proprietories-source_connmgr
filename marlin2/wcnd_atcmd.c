#define LOG_TAG "WCND"
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

#include "wcnd_marlin2.h"
#include "wcnd_sm.h"

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
/**
* Related Download Function for the wcn
*/

/**
* check "persist.sys.sprd.wcnlog" and "persist.sys.sprd.wcnlog.result"
* 1. if wcnlog == 0, just return.
* 2. if wcnlog == 1, wait until wcnlog.result == 1.
*/
#define WAIT_ONE_TIME (200) /* wait for 200ms at a time */
                            /* when polling for property values */
void wait_for_dump_logs(void) {
  const char *desired_status = "1";
  char value[PROPERTY_VALUE_MAX] = {'\0'};

  property_get(WCND_SLOG_ENABLE_PROP_KEY, value, "0");
  int slog_enable = atoi(value);
  if (!slog_enable) return;

  int maxwait = 300;  // wait max 300 s for slog dump cp2 log
  int maxnaps = (maxwait * 1000) / WAIT_ONE_TIME;

  if (maxnaps < 1) {
    maxnaps = 1;
  }

  memset(value, 0, sizeof(value));

  while (maxnaps-- > 0) {
    usleep(WAIT_ONE_TIME * 1000);
    if (property_get(WCND_SLOG_RESULT_PROP_KEY, value, NULL)) {
      if (strcmp(value, desired_status) == 0) {
        return;
      }
    }
  }
}
/**
* to check the CP2 state, and decide whether to real send cmd to CP2 or not.
* return <0, do not send cmd to CP2
* return others, need to send cmd to CP2
* out: atcmd_id, to specify the at cmd for getting cp2 version or set cp2 sleep
* or others
*/
static int before_send_atcmd(int client_fd, char *atcmd_str,
                             WcndManager *pWcndManger, int *atcmd_id) {
  char buffer[255];
  int to_get_cp2_version = 0;
  int send_back_is_done = 0;
  int to_tell_cp2_sleep = 0;
  int ret = -1;
  char loglevel[10];
  //char substr[10][30];
  memset(buffer, 0, sizeof(buffer));

  // check if it is going to get cp2 version
  if (strcasestr(atcmd_str, "spatgetcp2info")) {
    WCND_LOGD("%s: To get cp2 version", __func__);
    to_get_cp2_version = 1;
    if (atcmd_id) *atcmd_id = WCND_ATCMD_CMDID_GET_VERSION;
  } else if (!strcmp(atcmd_str, WCND_ATCMD_CP2_SLEEP)) {
    WCND_LOGD("%s: To tell cp2 to sleep ", __func__);
    to_tell_cp2_sleep = 1;
    if (atcmd_id) *atcmd_id = WCND_ATCMD_CMDID_SLEEP;
  }else if(!strcmp(atcmd_str, WCND_ATCMD_CP2_GET_CHIP)) {
     if (atcmd_id) *atcmd_id = WCND_ATCMD_CMDID_GET_CHIP;
  }else if(!strcmp(atcmd_str, WCND_ATCMD_CP2_GET_LOG)) {
     if (atcmd_id) *atcmd_id = WCND_ATCMD_CMDID_GET_LOG;
  }else if(!strcmp(atcmd_str, WCND_ATCMD_CP2_GET_LOGLEVEL)) {
     if (atcmd_id) *atcmd_id = WCND_ATCMD_CMDID_GET_LOGLEVEL;
  } else if (strstr(atcmd_str, WCND_ATCMD_CP2_LOGLEVEL)) {
     if (atcmd_id) *atcmd_id = WCND_ATCMD_CMDID_SET_LOGLEVEL;
  }

  // check if it is cp2 log setting
  if (strcasestr(atcmd_str, "at+armlog=0")) {
    pWcndManger->is_cp2log_opened = 0;
	property_set(WCND_PROPERTY_CP2_LOG,"0");
  } else if (strcasestr(atcmd_str, "at+armlog=1")) {
    pWcndManger->is_cp2log_opened = 1;
	property_set(WCND_PROPERTY_CP2_LOG,"1");
  }else if(strcasestr(atcmd_str, "at+loglevel=")){
    snprintf(buffer, sizeof(buffer) - 1, "%s", atcmd_str);
    ret = wcnd_split_int(buffer,"=");
    if(ret >= 0) {
        pWcndManger->cp2_log_level = ret;
        sprintf(loglevel,"%d",ret);
        property_set(WCND_LOGLEVEL_PROP_KEY,loglevel);
    }
    WCND_LOGD("%s: at+loglevel: %d", __func__, pWcndManger->cp2_log_level);
  }
  memset(buffer, 0, sizeof(buffer));

  // if cp2 is not in normal state
  if (pWcndManger->state != WCND_STATE_CP2_STARTED) {
    if (to_get_cp2_version) {
      // get cp2 version, use saved instead, if cp2 not in normal state
      snprintf(buffer, sizeof(buffer) - 1, "%s", pWcndManger->cp2_version_info);

      WCND_LOGD("%s: Save version info: '%s'", __func__, buffer);

      // send back the response
      if (client_fd > 0) {
        int ret = write(client_fd, buffer, strlen(buffer) + 1);
        if (ret < 0) {
          WCND_LOGE("write %s to client_fd:%d fail (error:%s)", buffer,
                    client_fd, strerror(errno));
        }
      }

      send_back_is_done = 1;
    } else if (!to_tell_cp2_sleep) {  // other cmds except sleep cmd
      WCND_LOGD("%s: CP2 is not in normal state (%d), at cmd FAIL, atcmd_id (%d)", __func__,
                pWcndManger->state, *atcmd_id);
      if (WCND_ATCMD_CMDID_GET_LOGLEVEL == *atcmd_id) {
        char value[10];
        sprintf(value, " %d", pWcndManger->cp2_log_level);
        strcpy(buffer, WCND_ATCMD_RESPONSE_LOGLEVEL);
        strcat(buffer, value);
        int ret = write(client_fd, buffer, strlen(buffer) + 1);
        if (ret < 0) {
          WCND_LOGE("write %s to client_fd:%d fail (error:%s)", buffer, client_fd,
           strerror(errno));
        }
      } else if (WCND_ATCMD_CMDID_SET_LOGLEVEL == *atcmd_id) {
        wcnd_send_back_cmd_result(client_fd, buffer, 1);
      } else {
        snprintf(buffer, sizeof(buffer) - 1, "%s", WCND_CP2_CLOSED_STRING);
        wcnd_send_back_cmd_result(client_fd, buffer, 0);
      }
      send_back_is_done = 1;
    }
  } else {
    if (pWcndManger->is_cp2_error) {
      WCND_LOGD("%s: CP2 is assert, at cmd FAIL", __func__);

      snprintf(buffer, sizeof(buffer) - 1, "%s", WCND_CP2_EXCEPTION_STRING);

      wcnd_send_back_cmd_result(client_fd, buffer, 0);

      send_back_is_done = 1;
    }
  }

  if (send_back_is_done)
    return -1;  // do not real send at cmd to cp2
  else
    return 0;
}

/**
* client_fd: the fd to send back the comand response
* return < 0 for cp2 return fail or do not real send cmd to cp2 because cp2 is
* not in normal state
*/
int wcnd_process_atcmd(int client_fd, char *atcmd_str,
                       WcndManager *pWcndManger) {
  int len = 0;
  int atcmd_fd = -1;
  char buffer[255];
  char buffer_temp[255];
  int atcmd_id = 0;
  int ret_value = -1;
 // int to_tell_cp2_sleep = 0;

  // First check if wcn modem is enabled or not, if not, the at cmd is not
  // supported
  if (pWcndManger && !pWcndManger->is_wcn_modem_enabled) {
    wcnd_send_back_cmd_result(client_fd, "WCN Disabled", 0);
    return -1;
  }

  if (!atcmd_str || !pWcndManger) return -1;

  int atcmd_len = strlen(atcmd_str);

  WCND_LOGD("%s: Receive AT CMD: %s, len = %d", __func__, atcmd_str, atcmd_len);

  // do some check, before real send to cp2
  if (before_send_atcmd(client_fd, atcmd_str, pWcndManger, &atcmd_id) < 0) {
    WCND_LOGD("%s: CP2 not in normal state, do not real send atcmd to it",
              __func__);
    return -1;
  }

  // Below send at command to CP2

  memset(buffer, 0, sizeof(buffer));

  snprintf(buffer, sizeof(buffer), "%s", atcmd_str);

  // at cmd shoud end with '\r'
  if ((atcmd_len < (sizeof(buffer) - 1)) && (buffer[atcmd_len - 1] != '\r')) {
    buffer[atcmd_len] = '\r';
    atcmd_len++;
  }

  atcmd_fd = open(pWcndManger->wcn_atcmd_iface_name, O_RDWR | O_NONBLOCK);
  WCND_LOGD("%s: open at cmd interface: %s, fd = %d", __func__,
            pWcndManger->wcn_atcmd_iface_name, atcmd_fd);
  if (atcmd_fd < 0) {
    WCND_LOGE("open %s failed, error: %s", pWcndManger->wcn_atcmd_iface_name,
              strerror(errno));
    wcnd_send_back_cmd_result(client_fd, "Send atcmd fail", 0);
    return -1;
  }

  len = write(atcmd_fd, buffer, atcmd_len);
  if (len < 0) {
    WCND_LOGE("%s: write %s failed, error:%s", __func__,
              pWcndManger->wcn_atcmd_iface_name, strerror(errno));
    close(atcmd_fd);
    wcnd_send_back_cmd_result(client_fd, "Send atcmd fail", 0);
    return -1;
  }

  // wait
  usleep(100 * 1000);

  WCND_LOGD("%s: Wait ATcmd to return", __func__);

  // Get AT Cmd Response
  int try_counts = 0;

try_again:
  if (try_counts++ > 5) {
    WCND_LOGE("%s: wait for response fail!!!!!", __func__);
    if (WCND_ATCMD_CMDID_GET_VERSION ==
        atcmd_id) {  // use saved version info instead
      snprintf(buffer, sizeof(buffer) - 1, "%s", pWcndManger->cp2_version_info);
      ret_value = 0;
    } else {
      snprintf(buffer, sizeof(buffer) - 1, "Fail: No data available");
    }
  } else {
    memset(buffer, 0, sizeof(buffer));

    do {
      len = read(atcmd_fd, buffer, sizeof(buffer) - 1);
    } while (len < 0 && errno == EINTR);

    if ((len <= 0)) {
      WCND_LOGE("%s: read fd(%d) return len(%d), errno = %s", __func__,
                atcmd_fd, len, strerror(errno));
      usleep(300 * 1000);
      goto try_again;
    } else {
      if (strstr(buffer, "fail") || strstr(buffer, "FAIL")) {
        // cp2 return fail
        ret_value = -1;
      } else {
        // save the CP2 version info
        if (WCND_ATCMD_CMDID_GET_VERSION == atcmd_id) {
          memcpy(pWcndManger->cp2_version_info, buffer, sizeof(buffer));
        }else if (WCND_ATCMD_CMDID_GET_CHIP == atcmd_id) {
            if(strstr(buffer,"2342B")) {
                pWcndManger->is_ge2_version = 1;
            }
        } else {
            int split_ret = -1;
            memcpy(buffer_temp,buffer,sizeof(buffer_temp)-1);
            if(WCND_ATCMD_CMDID_GET_LOG == atcmd_id){
               if(strstr(buffer,WCND_ATCMD_RESPONSE_ARMLOG)) {
                    split_ret = wcnd_split_int(buffer_temp," ");
                    if(split_ret >= 0) {
                        pWcndManger->is_cp2log_opened = split_ret;
                    }
                    WCND_LOGE("%s: ARM LOG = %d", __func__,pWcndManger->is_cp2log_opened);
               }
            }
            if(WCND_ATCMD_CMDID_GET_LOGLEVEL == atcmd_id){
                if(strstr(buffer,WCND_ATCMD_RESPONSE_LOGLEVEL)) {
                    split_ret = wcnd_split_int(buffer_temp," ");
                    if(split_ret >= 0){
                        pWcndManger->cp2_log_level= split_ret;
                    }
                    WCND_LOGE("%s: LOG LEVEL = %d", __func__,pWcndManger->cp2_log_level);
                }
            }
        }
        ret_value = 0;
      }
    }
  }

  WCND_LOGD("%s: ATcmd to %s return: '%s'", __func__,
            pWcndManger->wcn_atcmd_iface_name, buffer);

  close(atcmd_fd);

  /* The caller of "client_listen_thread" makes sure external client_fd >=0
   * There is the scenario that wcnd sent it-self ATCommand with client_fd == -1
   */
  if (WCND_AVOID_RESP_WITH_INVALID_FD == client_fd) {
    WCND_LOGD("Avoid sending response of '%s' to WCND it-self, ret_value: %d",
            buffer, ret_value);
    return ret_value;
  } else if(client_fd <= 0) {
    WCND_LOGE("Fail to send the response of '%s' as Invalid client_fd, ret_value: %d",
            buffer, ret_value);
    return ret_value;
  }

  // send back the response
  int ret = write(client_fd, buffer, strlen(buffer) + 1);
  if (ret < 0) {
    WCND_LOGE("write %s to client_fd:%d fail (error:%s)", buffer, client_fd,
              strerror(errno));
    return -1;
  }

  return ret_value;
}

/**
* send AT comd
*/
int send_atcmd(WcndManager *pWcndManger, char *atcmd_str) {
  int len = 0;
  int atcmd_fd = -1;
  char buffer[255];

  // First check if wcn modem is enabled or not, if not, the at cmd is not
  // supported
  if (pWcndManger && !pWcndManger->is_wcn_modem_enabled) {
    return -1;
  }

  if (!atcmd_str || !pWcndManger) return -1;

  int atcmd_len = strlen(atcmd_str);

  WCND_LOGD("%s: Receive AT CMD: %s, len = %d", __func__, atcmd_str, atcmd_len);

  // Below send at command to CP2

  memset(buffer, 0, sizeof(buffer));

  snprintf(buffer, sizeof(buffer)-1, "%s", atcmd_str);

  // at cmd shoud end with '\r'
  if ((atcmd_len < 254) && (buffer[atcmd_len - 1] != '\r')) {
    buffer[atcmd_len] = '\r';
    atcmd_len++;
  }

  atcmd_fd = open(pWcndManger->wcn_atcmd_iface_name, O_RDWR | O_NONBLOCK);
  WCND_LOGD("%s: open at cmd interface: %s, fd = %d", __func__,
            pWcndManger->wcn_atcmd_iface_name, atcmd_fd);
  if (atcmd_fd < 0) {
    WCND_LOGE("open %s failed, error: %s", pWcndManger->wcn_atcmd_iface_name,
              strerror(errno));
    return -1;
  }

  len = write(atcmd_fd, buffer, atcmd_len);
  if (len < 0) {
    WCND_LOGE("%s: write %s failed, error:%s", __func__,
              pWcndManger->wcn_atcmd_iface_name, strerror(errno));
    close(atcmd_fd);
    return -1;
  }
  WCND_LOGE("%s: write %s success", __func__,
              pWcndManger->wcn_atcmd_iface_name);
  close(atcmd_fd);
  return 0;
}

