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

int wcnd_runcommand(int client_fd, int argc, char *argv[]) {
  WcndManager *pWcndManger = wcnd_get_default_manager();

  if (argc < 1) {
    wcnd_send_back_cmd_result(client_fd, "Missing argument", 0);
    return 0;
  }

  int k = 0;
  for (k = 0; k < argc; k++) {
    WCND_LOGD("%s: arg[%d] = '%s'", __FUNCTION__, k, argv[k]);
  }

  if (!strcmp(argv[0], "reset")) {
    if (client_fd != pWcndManger->selfcmd_sockets[1]) {
      // tell the client the reset cmd is executed
      wcnd_send_back_cmd_result(client_fd, NULL, 1);
    }

    if (pWcndManger->is_cp2_error) wcnd_do_wcn_reset_process(pWcndManger);
  } else if (!strcmp(argv[0], "test")) {
    WCND_LOGD("%s: do nothing for test cmd", __FUNCTION__);
    wcnd_send_back_cmd_result(client_fd, NULL, 1);
  } else if (strstr(argv[0], "at+")) {  // at cmd
    WCND_LOGD("%s: AT cmd(%s)(len=%d)", __FUNCTION__, argv[0], strlen(argv[0]));
    wcnd_process_atcmd(client_fd, argv[0], pWcndManger);
  } else if (!strcmp(argv[0], WCND_SELF_EVENT_CP2_ASSERT)) {

    WcndMessage message;

    message.event = WCND_EVENT_CP2_ASSERT;
    message.replyto_fd = -1;
    wcnd_sm_step(pWcndManger, &message);
  } else if (!strcmp(argv[0], WCND_CMD_CP2_POWER_ON)) {
    int i = 0;

    // to set the type to be cmd
    pthread_mutex_lock(&pWcndManger->clients_lock);
    for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
      if (pWcndManger->clients[i].sockfd == client_fd) {
        pWcndManger->clients[i].type = WCND_CLIENT_TYPE_CMD;
        break;
      }
    }
    pthread_mutex_unlock(&pWcndManger->clients_lock);

    // if not use our own wcn, just return
    if (!pWcndManger->is_wcn_modem_enabled) {
      wcnd_send_notify_to_client(pWcndManger, WCND_CMD_RESPONSE_STRING " OK",
                                 WCND_CLIENT_TYPE_CMD);

      return 0;
    }

    //special handle for marlin2
    wcnd_set_marlin2_poweron(pWcndManger, 1);
    if(pWcndManger->state != WCND_STATE_CP2_STOPPED) {
	   wcnd_send_notify_to_client(pWcndManger, WCND_CMD_RESPONSE_STRING " OK",
										 WCND_CLIENT_TYPE_CMD);
	}
    //need to check timeout ........
  } else if (!strcmp(argv[0], WCND_CMD_CP2_POWER_OFF)) {
    int i = 0;

    // to set the type to be cmd
    pthread_mutex_lock(&pWcndManger->clients_lock);
    for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
      if (pWcndManger->clients[i].sockfd == client_fd) {
        pWcndManger->clients[i].type = WCND_CLIENT_TYPE_CMD;
        break;
      }
    }
    pthread_mutex_unlock(&pWcndManger->clients_lock);

    // if not use our own wcn, just return
    if (!pWcndManger->is_wcn_modem_enabled) {
      wcnd_send_notify_to_client(pWcndManger, WCND_CMD_RESPONSE_STRING " OK",
                                 WCND_CLIENT_TYPE_CMD);
      return 0;
    }
    wcnd_set_marlin2_poweron(pWcndManger, 0);
    wcnd_send_notify_to_client(pWcndManger, WCND_CMD_RESPONSE_STRING " OK",
                               WCND_CLIENT_TYPE_CMD);
    return 0;
  } else if (!strcmp(argv[0], WCND_SELF_CMD_CP2_VERSION)) {
    WCND_LOGD("%s: store the cp2 version info", __FUNCTION__);
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_GET_VERSION, pWcndManger);
    pWcndManger->store_cp2_versin_done = 1;
  } else if (!strcmp(argv[0], WCND_CMD_CP2_DUMP_ON)) {
    wcnd_send_back_cmd_result(client_fd, NULL, 1);
    if (pWcndManger) pWcndManger->dumpmem_on = 1;
  } else if (!strcmp(argv[0], WCND_CMD_CP2_DUMP_OFF)) {
    wcnd_send_back_cmd_result(client_fd, NULL, 1);
    if (pWcndManger) pWcndManger->dumpmem_on = 0;
  } else if (!strcmp(argv[0], WCND_CMD_CP2_DUMPMEM)) {
    wcnd_dump_cp2(pWcndManger);
    wcnd_send_back_cmd_result(client_fd, NULL, 1);
  } else if (!strcmp(argv[0], WCND_CMD_CP2_DUMPQUERY)) {
    if (pWcndManger->dumpmem_on)
      wcnd_send_back_cmd_result(client_fd, "dump=1", 1);
    else
      wcnd_send_back_cmd_result(client_fd, "dump=0", 1);

  } else if (!strcmp(argv[0], WCND_SELF_EVENT_MARLIN2_CLOSED)) {

    WcndMessage message;

    message.event = WCND_EVENT_MARLIN2_CLOSED;
    message.replyto_fd = -1;
    wcnd_sm_step(pWcndManger, &message);
  } else if (!strcmp(argv[0], WCND_SELF_EVENT_MARLIN2_OPENED)) {
    WcndMessage message;
    message.event = WCND_EVENT_MARLIN2_OPENED;
    message.replyto_fd = -1;
    wcnd_sm_step(pWcndManger, &message);
  }else if(!strcmp(argv[0], WCND_CMD_EVENT_GE2_ERROR))
   {
      WCND_LOGD("%s",WCND_CMD_EVENT_GE2_ERROR);
        if(!pWcndManger->is_wcn_modem_enabled ||(pWcndManger->is_ge2_error == 1) || (pWcndManger->is_cp2_error == 1)) return  -1;
        WCND_LOGD("%s: is_ge2_version %d", __FUNCTION__,pWcndManger->is_ge2_version);
        if(pWcndManger->is_ge2_version) {
#ifndef CP2_GE2_COEXT
           if(pWcndManger->is_in_userdebug) {
               char buffer[255];
               memset(buffer, 0, sizeof(buffer));
               snprintf(buffer, sizeof(buffer)-1, "%s %s", WCND_GE2_EXCEPTION_STRING, argv[1]);
               if (argc >= 2) {
                 for (k = 2; k < argc; k++) {
                   snprintf(buffer + strlen(buffer), sizeof(buffer) - 1 - strlen(buffer), " %s", argv[k]);
                 }
               }
               // Notify client reset start
               pWcndManger->notify_enabled = 1;
               wcnd_send_notify_to_client(pWcndManger, buffer,
                       WCND_CLIENT_TYPE_NOTIFY);
			   return 0;
            }
#endif
            char info[255];
            memset(info, 0, sizeof(info));
            snprintf(info, sizeof(info) - 1, "%s", argv[1]);
            if (argc >= 2) {
              for (k = 2 ; k < argc; k++) {
                snprintf(info + strlen(info), sizeof(info) - 1 - strlen(info), " %s", argv[k]);
              }
            }
            handle_ge2_error(pWcndManger, info);

        }
  } else {
    wcnd_send_back_cmd_result(client_fd, "Not support cmd", 0);
  }

  return 0;
}
