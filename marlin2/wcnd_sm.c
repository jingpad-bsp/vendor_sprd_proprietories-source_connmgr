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

/**
* send notify information to the specified connected client.
* return -1 for fail
* Note: message send from wcnd must end with null character
*             use null character to identify a completed message.
*/
static int wcnd_notify_client(WcndManager *pWcndManger, char *info_str,
                              int client_fd, int notify_type) {
  int i;

  if (!pWcndManger || !info_str) return -1;

  WCND_LOGD("notify_client (fd:%d, type: %d)", client_fd, notify_type);

  pthread_mutex_lock(&pWcndManger->clients_lock);

  /* info socket clients that WCN with str info */
  for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
    int fd = pWcndManger->clients[i].sockfd;
    int type = pWcndManger->clients[i].type;
    WCND_LOGD("clients[%d].sockfd = %d, type = %d\n", i, fd, type);

    if (fd >= 0 && (fd == client_fd) &&
        ((type == notify_type) ||
         ((notify_type == WCND_CLIENT_TYPE_CMD) &&
          ((type & WCND_CLIENT_TYPE_CMD_MASK) == notify_type)))) {
      /* Send the zero-terminated message */

      // Note: message send from wcnd must end with null character
      // use null character to identify a completed message.
      int len = strlen(info_str) + 1;  // including null character

      WCND_LOGD("send %s to client_fd:%d", info_str, client_fd);

      int ret = TEMP_FAILURE_RETRY(write(client_fd, info_str, len));
      if (ret < 0) {
        WCND_LOGE("write %d bytes to client_fd:%d fail (error:%s)", len,
                  client_fd, strerror(errno));

        close(fd);
        pWcndManger->clients[i].sockfd = -1;
        pWcndManger->clients[i].type = WCND_CLIENT_TYPE_NOTIFY;
      } else if (notify_type == WCND_CLIENT_TYPE_CMD ||
                 notify_type == WCND_CLIENT_TYPE_CMD_PENDING ||
                 (notify_type & WCND_CLIENT_TYPE_CMD_MASK) ==
                     WCND_CLIENT_TYPE_CMD) {
        pWcndManger->clients[i].type = WCND_CLIENT_TYPE_SLEEP;
      }
    }
  }

  pthread_mutex_unlock(&pWcndManger->clients_lock);

  return 0;
}

/**
* Config marlin2 one time after marlin2 is power on
* return 0 for success, -1 for fail.
*/
static int on_marlin2_poweron(WcndManager *pWcndManger) {
  if (!pWcndManger) return -1;

  if (pWcndManger->is_eng_mode_only) return 0;

  // if not use our own wcn, just return
  if (!pWcndManger->is_wcn_modem_enabled) return 0;

  // get marlin2 version
  if (!pWcndManger->store_cp2_versin_done) {
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_GET_VERSION, pWcndManger);
    pWcndManger->store_cp2_versin_done = 1;
  }

  // in user version, config CP2 to enter user mode
  if(pWcndManger->is_in_userdebug) {
    WCND_LOGD("in user version, need to config CP2 to enter user mode!!!");
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_EXIT_USER, pWcndManger);
  }else {
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_ENTER_USER, pWcndManger);
  }

  // Reset the cp2 log, since after power on cp2, it will reset to default
  if (pWcndManger->is_cp2log_opened) {
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_ENABLE_LOG, pWcndManger);
  } else {
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_DISABLE_LOG, pWcndManger);
  }
  char cp2_log_level[WCND_MAX_IFACE_NAME_SIZE];
  snprintf(cp2_log_level,WCND_MAX_IFACE_NAME_SIZE ,WCND_ATCMD_CP2_LOGLEVEL"%d\r",
           pWcndManger->cp2_log_level);
  WCND_LOGD("on_marlin2_poweron cp2_log_level %s",cp2_log_level);
  wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, cp2_log_level, pWcndManger);

  return 0;
}

static void wcn_state_handle_default(WcndManager *pWcndManger,
                                     WcndMessage *pMessage) {
  if (!pWcndManger || !pMessage) return;

  switch (pMessage->event) {
    case WCND_EVENT_CP2_ASSERT:
      // transact to next state
      pWcndManger->state = WCND_STATE_CP2_ASSERT;
      WCND_LOGD("Enter WCND_STATE_CP2_ASSERT from WCND_STATE_CP2_STARTED");
      // to do reset
      wcnd_send_selfcmd(pWcndManger, "wcn reset");
      break;

    default:
      break;
  }
}

static void wcn_state_cp2_stopped(WcndManager *pWcndManger,
                                  WcndMessage *pMessage) {
  if (!pWcndManger || !pMessage) return;

  pWcndManger->notify_enabled = 0;

  switch (pMessage->event) {
    case WCND_EVENT_CP2_DOWN:
      wcnd_send_notify_to_client(pWcndManger, WCND_CMD_RESPONSE_STRING " OK",WCND_CLIENT_TYPE_CMD);
      break;
    case WCND_EVENT_MARLIN2_OPENED:
      WCND_LOGD("Receive MARLIN2 OPENED EVENT!");

      wcnd_send_notify_to_client(pWcndManger, WCND_CMD_RESPONSE_STRING " OK",WCND_CLIENT_TYPE_CMD);
      pWcndManger->state = WCND_STATE_CP2_STARTED;
      pWcndManger->is_cp2_error = 0;
      pWcndManger->is_ge2_error = 0;
      pWcndManger->notify_enabled = 1;
      on_marlin2_poweron(pWcndManger);
      break;

    default:
      wcn_state_handle_default(pWcndManger, pMessage);
      break;
  }
}


static void wcn_state_cp2_started(WcndManager *pWcndManger,
                                  WcndMessage *pMessage) {
  if (!pWcndManger || !pMessage) return;

  pWcndManger->notify_enabled = 1;

  switch (pMessage->event) {
    case WCND_EVENT_CP2_DOWN:
      // transact to next state
      pWcndManger->state = WCND_STATE_CP2_STOPPED;

      WCND_LOGD(
          "Warning: Enter WCND_STATE_CP2_STOPPED from wcn_state_cp2_started");

      break;
    case WCND_EVENT_MARLIN2_CLOSED:
      WCND_LOGD("Receive MARLIN2 CLOSED EVENT!");

      wcnd_send_notify_to_client(pWcndManger, WCND_CMD_RESPONSE_STRING " OK",
                                       WCND_CLIENT_TYPE_CMD);
      pWcndManger->state = WCND_STATE_CP2_STOPPED;
      pWcndManger->notify_enabled = 0;
      break;
    case WCND_EVENT_MARLIN2_OPENED:
      WCND_LOGD("Receive MARLIN2 OPENED EVENT!");

      wcnd_send_notify_to_client(pWcndManger, WCND_CMD_RESPONSE_STRING " OK",WCND_CLIENT_TYPE_CMD);
      pWcndManger->is_cp2_error = 0;
      pWcndManger->is_ge2_error = 0;
      pWcndManger->notify_enabled = 1;
      on_marlin2_poweron(pWcndManger);
      break;
    default:
      wcn_state_handle_default(pWcndManger, pMessage);
      break;
  }
}

static void wcn_state_cp2_assert(WcndManager *pWcndManger,
                                 WcndMessage *pMessage) {

  if (!pWcndManger || !pMessage) return;

  pWcndManger->notify_enabled = 1;

  switch (pMessage->event) {

    case WCND_EVENT_CP2_DOWN:
          if(check_if_reset_enable(pWcndManger)) {
              WCND_LOGE(" wcn_state_cp2_starting send WCN-CP2-ALIVE!!");
              wcnd_send_notify_to_client(pWcndManger, WCND_CP2_ALIVE_STRING,WCND_CLIENT_TYPE_NOTIFY);
              pWcndManger->is_cp2_error = 0;
              pWcndManger->is_ge2_error = 0;
              pWcndManger->state = WCND_STATE_CP2_STOPPED;
              break;
          } else {
              pWcndManger->state = WCND_STATE_CP2_ASSERT;
              break;
          }
      break;

    default:
      wcn_state_handle_default(pWcndManger, pMessage);
      break;
  }
}


/**
* This Function will be called only in client listen thread, so there is not
* need to do some sync
*/
// state machine handle message
int wcnd_sm_step(WcndManager *pWcndManger, WcndMessage *pMessage) {
  if (!pWcndManger || !pMessage) return -1;

  // if not use our own wcn, just return
  if (!pWcndManger->is_wcn_modem_enabled) {
    return 0;
  }

  WCND_LOGD("Current State: %d, receive event: 0x%x!!", pWcndManger->state,
            pMessage->event);

  switch (pWcndManger->state) {
    case WCND_STATE_CP2_STOPPED:
      wcn_state_cp2_stopped(pWcndManger, pMessage);
      break;
    case WCND_STATE_CP2_STARTED:
      wcn_state_cp2_started(pWcndManger, pMessage);
      break;
    case WCND_STATE_CP2_ASSERT:
      wcn_state_cp2_assert(pWcndManger, pMessage);
      break;
    default:
      WCND_LOGE("ERROR:Unknown CP2 STATE (%d)",
                pWcndManger->state);
      break;
  }


  return 0;
}

int wcnd_sm_init(WcndManager *pWcndManger) {
  if (!pWcndManger) return -1;

  // if not use our own wcn, just return
  if (!pWcndManger->is_wcn_modem_enabled) {
    pWcndManger->state = WCND_STATE_CP2_STARTED;
    pWcndManger->notify_enabled = 1;
  } else {
    pWcndManger->state = WCND_STATE_CP2_STOPPED;
    pWcndManger->notify_enabled = 0;
  }

  return 0;
}
