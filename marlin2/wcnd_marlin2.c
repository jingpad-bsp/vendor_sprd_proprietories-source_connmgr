#include "wcnd_marlin2.h"


#define WATI_FOR_CP2_READY_TIME_MSECS (100)
#define MAX_LOOP_TEST_COUNT (50)
#define LOOP_TEST_INTERVAL_MSECS (100)
#define RESET_FAIL_RETRY_COUNT (3)
#define RESET_RETRY_INTERVAL_MSECS (2000)


/**
* send notify information to all the connected clients.
* return -1 for fail
* Note: message send from wcnd must end with null character
*             use null character to identify a completed message.
*/
int wcnd_send_notify_to_client(WcndManager *pWcndManger, char *info_str,
                               int notify_type) {
    int i, ret;

    if (!pWcndManger || !info_str) return -1;

    WCND_LOGD("send_notify_to_client (type:%d)", notify_type);


    pthread_mutex_lock(&pWcndManger->clients_lock);

    /* info socket clients that WCN with str info */
    for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
        int fd = pWcndManger->clients[i].sockfd;
        int type = pWcndManger->clients[i].type;
        WCND_LOGD("clients[%d].sockfd = %d, type = %d\n", i, fd, type);

        if (fd >= 0 && ((type == notify_type) ||
                        ((notify_type == WCND_CLIENT_TYPE_CMD) &&
                         ((type & WCND_CLIENT_TYPE_CMD_MASK) == notify_type)))) {
             ret = send_msg(pWcndManger, fd, info_str);
      if (RDWR_FD_FAIL == ret) {
        WCND_LOGD("reset clients[%d].sockfd = -1", i);
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

void handle_ge2_error(WcndManager *pWcndManger,char *info_str) {
  if (!pWcndManger) {
    WCND_LOGE("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    return;
  }

  WCND_LOGD("handle_ge2_error\n");
  if(pWcndManger->is_cp2_error) return;
  // set the cp2 error flag, it is cleared when reset successfully
  pWcndManger->is_cp2_error = 1;
  pWcndManger->is_ge2_error = 1;

  notify_cp2_exception(pWcndManger, info_str);

  WCND_LOGD("handle_ge2_error end!\n");

  return;
}

/**
* Reset CP2 if loop check fail
*/
static void handle_cp2_loop_check_fail(WcndManager *pWcndManger) {
  if (!pWcndManger) {
    WCND_LOGE("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    return;
  }

  WCND_LOGD("handle_cp2_loop_check_fail\n");
  if(pWcndManger->is_cp2_error) return;

  // set the cp2 error flag, it is cleared when reset successfully
  pWcndManger->is_cp2_error = 1;
  if (pWcndManger->is_in_userdebug)
	sleep(15);	// add for waiting marlin watchdog to do some clean work,
				// check Bug#440390

  char *rdbuffer = "CP2 LOOP CHECK FAIL";


  notify_cp2_exception(pWcndManger, rdbuffer);

  WCND_LOGD("handle_cp2_loop_check_fail end!\n");

  return;
}


/*
* polling /dev/spipe_wcn0, do write and read testing.
* is_loopcheck: if true use select.
*     if false use NONBLOCK mode, because after downloading image and starting
* CP2, the cp2 may not receive the string
*     that wrote to the /dev/spipe_wcn0, for it is not inited completely.
* Note:
*     return 0 for OK;
*     return -1 for fail
*     return 1 for marlin2 power off
*/
int is_cp2_alive_ok(WcndManager *pWcndManger, int is_loopcheck) {
  int len = 0;
  int loop_fd = -1;

  loop_fd = open(pWcndManger->wcn_loop_iface_name, O_RDWR | O_NONBLOCK);

  WCND_LOGD("%s: open polling interface: %s, fd = %d", __func__,
            pWcndManger->wcn_loop_iface_name, loop_fd);
  if (loop_fd < 0) {
    WCND_LOGE("open %s failed, error: %s", pWcndManger->wcn_loop_iface_name,
              strerror(errno));
    return -1;
  }

  len = write(loop_fd, LOOP_TEST_STR, strlen(LOOP_TEST_STR));
  if (len < 0) {
    WCND_LOGE("%s: write %s failed, error:%s", __func__,
              pWcndManger->wcn_loop_iface_name, strerror(errno));
    close(loop_fd);
    return -1;
  }

    // wait
    usleep(100 * 1000);

    char buffer[32];
    memset(buffer, 0, sizeof(buffer));
    do {
      len = read(loop_fd, buffer, sizeof(buffer));
    } while (len < 0 && errno == EINTR);

    // special handle for marlin2
    if (strstr(buffer, MARLIN2_POWEROFF_IDNENTITY)) {
       WCND_LOGE("%s: read %d return %d, buffer:%s!", __func__, loop_fd, len,
                  buffer);
        close(loop_fd);
        return 1;  // marlin2 is power off
    }

    if ((len <= 0) || !strstr(buffer, LOOP_TEST_ACK_STR)) {
      WCND_LOGE("%s: read %d return %d, buffer:%s,  errno = %s", __func__,
                loop_fd, len, buffer, strerror(errno));
      close(loop_fd);
      return -1;
    }

  WCND_LOGD("%s: loop: %s is OK", __func__, pWcndManger->wcn_loop_iface_name);

  close(loop_fd);

  return 0;
}
#define LOOP_CHECK_INTERVAL_MSECS (5000)

static int marlin2_listen_status(int loop_fd ,int time){

char buffer[32];
int len = 0;

  fd_set read_fds;
  int rc = 0;
  int max = -1;
  struct timeval timeout;

  timeout.tv_sec = time;
  timeout.tv_usec = 0;

  FD_ZERO(&read_fds);

  max = loop_fd;
  FD_SET(loop_fd, &read_fds);

pollmarlin2:
  if(time > 0) {
      rc = select(max + 1, &read_fds, NULL, NULL, &timeout);
  } else {
      rc = select(max + 1, &read_fds, NULL, NULL, NULL);
  }
  if (rc  < 0) {
      if (errno == EINTR) goto pollmarlin2;
      WCND_LOGD("%s: select loop_fd(%d) failed: %s", __func__, loop_fd,
                strerror(errno));
      close(loop_fd);
      return -1;
  } else if(rc > 0) {
      if (!(FD_ISSET(loop_fd, &read_fds))) {
          WCND_LOGD("%s: select loop_fd(%d) return > 0, but loop_fd is not set!",
                     __func__, loop_fd);
          close(loop_fd);
          return -1;
      }
  }
  return rc;
}


/**
*check cp2 loop dev interface in a interval of 5 seconds
* if fail, reset the cp2
*/
void *cp2_loop_check_thread(void *arg) {
  WcndManager *pWcndManger = (WcndManager *)arg;

  int count = 0;
  int is_loopcheck_fail = 0;
  int loop_fd = -1;
  char buffer[32];
  int len = 0;

  if (!pWcndManger) {
    WCND_LOGD("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    exit(-1);
  }

  // special handle for the case that an assert/watchdog assert happens when
  // system up
  // before WcnManagerService is ready. At this time need to notify assert again
  if (pWcndManger->is_cp2_error && !pWcndManger->doing_reset) {
    char value[PROPERTY_VALUE_MAX] = {'\0'};

    property_get(WCND_RESET_PROP_KEY, value, "0");
    int is_reset = atoi(value);
    if (is_reset) {
      WCND_LOGD(
          "%s: CP2 assert/watchdog assert, and reset is enabled, but does not "
          "doing reset."
          "So notify loop fail again!!",
          __FUNCTION__);

      handle_cp2_loop_check_fail(pWcndManger);
      // wait 20 seconds for reset
      sleep(20);
    }
  }
  loop_fd = open(pWcndManger->wcn_loop_iface_name, O_RDWR /*|O_NONBLOCK*/);
  if (loop_fd < 0) {
    WCND_LOGE("%s :open %s failed, error: %s", __func__,
              pWcndManger->wcn_loop_iface_name, strerror(errno));
    exit(-1);
  }

// check if marlin2 is power on
checkmarlin2:

  WCND_LOGE("%s :Check marlin2 Status!!", __func__);

  memset(buffer, 0, sizeof(buffer));
  do {
    len = read(loop_fd, buffer, sizeof(buffer));
  } while (len < 0 && errno == EINTR);

  WCND_LOGE("%s :After Read. (len = %d)!!", __func__, len);

  if ((len <= 0) || !strstr(buffer, LOOP_TEST_ACK_STR)) {
    WCND_LOGE("%s: read %d return %d, buffer:%s,  (errno = %s)", __func__,
              loop_fd, len, buffer, strerror(errno));
    WCND_LOGE("%s: marlin2 is power off!", __func__);
    wcnd_send_selfcmd(pWcndManger, "wcn " WCND_SELF_EVENT_MARLIN2_CLOSED);

    // wait until marlin is power on

    if(marlin2_listen_status(loop_fd,0) > 0){
        goto checkmarlin2;
    }else {
        WCND_LOGE("%s: marlin2_listen_status is error!", __func__);
       exit(-1);
    }
  }

  // marlin2 is power on
  WCND_LOGE("%s: marlin2 is power on! is_cp2_error = %d", __func__, pWcndManger->is_cp2_error);
  if (pWcndManger->is_cp2_error == 0) {
    wcnd_send_selfcmd(pWcndManger, "wcn " WCND_SELF_EVENT_MARLIN2_OPENED);
  }

  while (1) {
    // marlin2 is power on
    if(marlin2_listen_status(loop_fd,LOOP_CHECK_INTERVAL_MSECS/1000) < 0){
        WCND_LOGE("%s: marlin2_listen_status is error wait 5s fd = d%", __func__,loop_fd);
        usleep(LOOP_CHECK_INTERVAL_MSECS * 1000);
    }

    // cp2 exception happens just continue for next poll
    if (pWcndManger->state == WCND_STATE_CP2_ASSERT) {
      WCND_LOGD("%s: CP2 or ge2 exception happened and not reset success!!",
                __FUNCTION__);
	 while(pWcndManger->is_cp2_error){
         WCND_LOGD("%s: CP2 or ge2 exception happened and not reset success!!",
             __FUNCTION__);
		 sleep(5);
	 }
      goto checkmarlin2;;
    }

    count = 2;
    while (count-- > 0) {
      int ret = 0;
      ret = is_cp2_alive_ok(pWcndManger,0);  // marlin cannot support select, so use non-block mode
      if (ret < 0) {
        // during loop checking, cp2 exception happens just continue
        if (pWcndManger->is_cp2_error ||
            (pWcndManger->state != WCND_STATE_CP2_STARTED)) {
          is_loopcheck_fail = 0;
          break;
        }

        is_loopcheck_fail = 1;
        if (count > 0) sleep(3);//first time is fail, wait 3s for next check.

      } else if (ret == 1)  // marlin2 poweroff
      {
        WCND_LOGD("%s: marlin2 is power off!!", __FUNCTION__);
        goto checkmarlin2;
      } else {
        is_loopcheck_fail = 0;
        break;
      }
    }

    if (is_loopcheck_fail) {

      WCND_LOGD("%s: loop check fail, going to reset cp2!!", __FUNCTION__);
      handle_cp2_loop_check_fail(pWcndManger);
      // wait 20 seconds for reset
      sleep(20);
      goto checkmarlin2;
    }
  }
  WCND_LOGD("%s: marlin2_loop_check error return",
                __FUNCTION__);
}

  /**
  * Store the CP2 Version info, used when system startup
  */
int store_cp2_version_info(WcndManager *pWcndManger) {
  if (!pWcndManger) return -1;

  int count = 100;

  if (pWcndManger->is_eng_mode_only) return 0;

  // if not use our own wcn, just return
  if (!pWcndManger->is_wcn_modem_enabled) return 0;

  wcnd_send_selfcmd(pWcndManger, "wcn " WCND_CMD_CP2_POWER_ON);

  // wait CP2 started, wait 10s at most
  while (count-- > 0) {
      if (pWcndManger->state == WCND_STATE_CP2_STARTED) break;

      usleep(100 * 1000);
  }

  if (pWcndManger->state != WCND_STATE_CP2_STARTED) {
      WCND_LOGE("%s: CP2 does not start successs, just return!!", __func__);
      wcnd_send_selfcmd(pWcndManger, "wcn " WCND_CMD_CP2_POWER_OFF);
      return -1;
   }

  // wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_GET_VERSION, pWcndManger);
  wcnd_send_selfcmd(pWcndManger, "wcn " WCND_SELF_CMD_CP2_VERSION);

  count = 100;
  // wait get cp2 version complete, wait 10s at most
  while (count-- > 0) {
    if (pWcndManger->store_cp2_versin_done) break;

    usleep(100 * 1000);
  }
  wcnd_send_selfcmd(pWcndManger, "wcn " WCND_CMD_CP2_POWER_OFF);
  return 0;
}
  /**
* Do some CP2 config, if it is a user version
*/
static int config_cp2_for_user_version(WcndManager *pWcndManger) {
  if (!pWcndManger) return -1;

  if(pWcndManger->is_in_userdebug) {
    WCND_LOGD("in userdebug version, need to config CP2 to enter userdebug mode!!!");
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_EXIT_USER, pWcndManger);
  }else {
    WCND_LOGD("in user version, need to config CP2 to enter user mode!!!");
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, WCND_ATCMD_CP2_ENTER_USER, pWcndManger);
  }
  return 0;
}
  /**
  * Config CP2 one time after CP2 has just started. (such as system boot up, CP2
  * recovery after reset)
  * return 0 for success, -1 for fail.
  */
  static int config_cp2_bootup(WcndManager *pWcndManger) {
    if (!pWcndManger) return -1;

    if (pWcndManger->is_eng_mode_only) return 0;

    // if not use our own wcn, just return
    if (!pWcndManger->is_wcn_modem_enabled) return 0;

    // Do some CP2 config, if it is a user version
    config_cp2_for_user_version(pWcndManger);
    char cp2_log_level[WCND_MAX_IFACE_NAME_SIZE];
    snprintf(cp2_log_level,WCND_MAX_IFACE_NAME_SIZE ,WCND_ATCMD_CP2_LOGLEVEL"%d\r",
                       pWcndManger->cp2_log_level);
    WCND_LOGD("on_marlin2_poweron cp2_log_level %s",cp2_log_level);
    wcnd_process_atcmd(WCND_AVOID_RESP_WITH_INVALID_FD, cp2_log_level, pWcndManger);
    return 0;
  }

void wcnd_cp2_log_init(WcndManager *pWcndManger) {
    int cp2_log = 0;
    char value[PROPERTY_VALUE_MAX] = {'\0'};

    property_get(WCND_LOGLEVEL_PROP_KEY, value, "3");
    pWcndManger->cp2_log_level = atoi(value);
    WCND_LOGD("%s: CP2 log level: %d", __FUNCTION__,pWcndManger->cp2_log_level);

    property_get(WCND_PROPERTY_CP2_LOG, value, "2");
    cp2_log = atoi(value);
    if(cp2_log == WCND_CP2_LOG_DEFAULE) {
        if(pWcndManger->is_in_userdebug){
            pWcndManger->is_cp2log_opened = WCND_CP2_LOG_ENABLE;
            property_set(WCND_PROPERTY_CP2_LOG,"1");
        } else{
            pWcndManger->is_cp2log_opened = WCND_CP2_LOG_DISABLE;
            property_set(WCND_PROPERTY_CP2_LOG,"0");
        }
    }else {
        pWcndManger->is_cp2log_opened = cp2_log;
    }

    WCND_LOGD("%s: CP2 log : %d", __FUNCTION__,pWcndManger->is_cp2log_opened);

}
/**
* do some prepare work, before dump/reset cp2
*/
void prepare_cp2_recovery(WcndManager *pWcndManger) {
    if (!check_if_reset_enable(pWcndManger)) {
        WCND_LOGD("For Marlin2, to dump, then don't kill bt/wpa_supplicant!!"
                "But as require of Slog, wait for while before starting dump!\n");
        sleep(2);
        return;
    }
    wcnd_reset_cp2(pWcndManger);

    // kill supplicant
    int wlan_ret = wcnd_kill_process_by_name(WCND_WIFI_SUPPLICANT_PROCESS, SIGINT);
    // kill bluetooth
    int bluetooth_ret = wcnd_kill_process_by_name(WCND_BT_BLUETOOTH_PROCESS, SIGINT);
    /* down the wifi network interface */
    wcnd_down_network_interface("wlan0");
    // kill fm
    wcnd_stop_process(WCND_FM_FMRADIO_PROCESS, WCND_KILL_FM_PROCESS_TIMEOUT);
    pWcndManger->wait_wifi_driver_unloaded = 0;

    // have send sig to target process, wait it to exit
    if (WCND_KILL_PROCESS_NO_EXIT != wlan_ret) {
        // check if bt process is killed
        // Bug#450036 bt cost more than 2s to exit when receive SIGINT -->
        int count = 50;
        while (count > 0) {
            int check_wlan_ret = wcnd_check_process_exist(WCND_WIFI_SUPPLICANT_PROCESS, wlan_ret);
            // if the target bt process does not exist
            // or the original exited, and a new bt process is detected
            if (!check_wlan_ret || (wlan_ret > 0 && check_wlan_ret > 0 && check_wlan_ret != wlan_ret)){
                WCND_LOGD("%s, killed the process of %s \n", __FUNCTION__, WCND_WIFI_SUPPLICANT_PROCESS);
                break;
            }else{
                count--;
            }
            // 100ms
            usleep(100 * 1000);
        }
    }

    // have send sig to target process, wait it to exit
    if (WCND_KILL_PROCESS_NO_EXIT != bluetooth_ret) {
        // check if bt process is killed
        // Bug#450036 bt cost more than 2s to exit when receive SIGINT -->
        int count = 50;
        while (count > 0) {
            int check_bt_ret = wcnd_check_process_exist(WCND_BT_BLUETOOTH_PROCESS, bluetooth_ret);
            // if the target bt process does not exist
            // or the original exited, and a new bt process is detected
            if (!check_bt_ret || (bluetooth_ret > 0 && check_bt_ret > 0 && check_bt_ret != bluetooth_ret)){
                WCND_LOGD("%s, killed the process of %s \n", __FUNCTION__, WCND_BT_BLUETOOTH_PROCESS);
                break;
            }else{
                count--;
            }
            // 100ms
            usleep(100 * 1000);
        }
    }
}


/**
* do cp2 reset processtrue:
* 1. notify connected clients "cp2 reset start"
* 2. reset/dump cp2
* 3. notify connected clients "cp2 reset end"
*/
int wcnd_do_wcn_reset_process(WcndManager *pWcndManger) {
  WcndMessage message;
  int is_reset_dump = WCND_TO_NONE_ACTION_CP2;

  if (!pWcndManger) {
    WCND_LOGE("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    return -1;
  }

  prepare_cp2_recovery(pWcndManger);
  is_reset_dump = prepare_cp2_dump(pWcndManger);
  if (WCND_TO_RESET_CP2 == is_reset_dump){
      WCND_LOGD("%s: Have sent cmd of reset wcn cp2.", __FUNCTION__);
  } else if (WCND_TO_DUMP_CP2 == is_reset_dump){
      WCND_LOGD("%s: Have sent cmd of dump wcn cp2.", __FUNCTION__);
  }

  message.event = WCND_EVENT_CP2_DOWN;
  message.replyto_fd = -1;

  wcnd_sm_step(pWcndManger, &message);

  return 0;
}
/**
* power on/off marlin2
*/
int wcnd_set_marlin2_poweron(WcndManager *pWcndManger, int poweron) {
  if(!pWcndManger) {
    WCND_LOGD("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    return -1;
  }

  if(poweron) {
    send_atcmd(pWcndManger, "startwcn");
  }	else {
    send_atcmd(pWcndManger, "stopwcn");
  }

  return 0;
}

/**
* do some check, before dump.
* return 0  for "reset" can be going on
* return 1  for "dump"  can be going on
* return -1 for "NoneAction"
*/
int prepare_cp2_dump(WcndManager *pWcndManger) {
    if (!pWcndManger) return WCND_TO_NONE_ACTION_CP2;

    // special handle for marlin2
    if (!check_if_reset_enable(pWcndManger)) {
      // WCND_LOGD("Send 'dumpmem' cmd before notify slog!!");

      // send "dumpmem" cmd to cp2 first
      send_atcmd(pWcndManger, WCND_CMD_CP2_DUMPMEM);

      // notify slog
      //WCND_LOGD("Notify slog to dump");

      // char buffer[255];
      // snprintf(buffer, 255, "%s", WCND_CP2_EXCEPTION_STRING);

      // wcnd_send_notify_to_client(pWcndManger, buffer,
      // WCND_CLIENT_TYPE_NOTIFY);

      WCND_LOGD("Wait slog to dump");
      // wait slog dump complete
      wait_for_dump_logs();

      WCND_LOGD("Wait slog dump complete");
      //To do dump cp2
      return WCND_TO_DUMP_CP2;
    }
    //To do reset cp2
    return WCND_TO_RESET_CP2;
}

/**
* to dump cp2 mem
*/
int wcnd_dump_cp2_mem(WcndManager *pWcndManger) {
  // if(!pWcndManger) return -1;

  // dump mem
  // send "dumpmem" cmd to cp2
  send_atcmd(pWcndManger, WCND_CMD_CP2_DUMPMEM);
  return 0;
}
/**
* to reset cp2 mem
*/
int wcnd_reset_cp2(WcndManager *pWcndManger) {
   if(!pWcndManger) return -1;

    if(pWcndManger->is_ge2_error) {
        WCND_LOGD("GPS error, kill bt/wpa_supplicant!!\n");
        send_atcmd( pWcndManger,WCND_ATCMD_REBOOT_WCN);
    }else if(pWcndManger->is_cp2_error){
        WCND_LOGD("marlin2 error, kill bt/wpa_supplicant!!\n");
        send_atcmd(pWcndManger, WCND_ATCMD_REBOOT_MARLIN);
    }
    //if(pWcndManger->is_engineer_poweron) {
        wcnd_set_marlin2_poweron(pWcndManger, 0);
        sleep(1);
    //}
    return 0;
}

int wcnd_dump_cp2(WcndManager *pWcndManger) {
  if (!pWcndManger) return -1;

  // set to prevent loop check
  pWcndManger->is_cp2_error = 1;

  char buffer[255];
  memset(buffer, 0, sizeof(buffer));
  snprintf(buffer, sizeof(buffer)-1, "%s", WCND_CP2_EXCEPTION_STRING " for dump mem");

  wcnd_send_notify_to_client(pWcndManger, buffer, WCND_CLIENT_TYPE_NOTIFY);

  // dump mem
  wcnd_dump_cp2_mem(pWcndManger);

  return 0;
}

/**
* check "persist.vendor.sys.wcnreset" property to see if reset cp2 or not.
* return non-zero for true.
*/
int check_if_reset_enable(WcndManager *pWcndManger) {
  char value[PROPERTY_VALUE_MAX] = {'\0'};
  int is_reset = 0;
  int debug_reset = 0;
  property_get(WCND_DEBUG_RESET_KEY, value, "0");
  debug_reset = atoi(value);
  property_get(WCND_RESET_PROP_KEY, value, "0");
  is_reset = atoi(value);
  if((!pWcndManger->is_in_userdebug && !pWcndManger->dumpmem_on && is_reset) || debug_reset){
    return 1;
  }

  return 0;
}

int notify_cp2_exception(WcndManager *pWcndManger, char *info_str) {
  if (!pWcndManger) {
    WCND_LOGE("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    return -1;
  }

  // set CP2 status to be assert
  property_set(WCND_CP2_STATE_PROP_KEY, "assert");

  // notify wifi driver, cp2 is assert
  if (1 == wcnd_notify_wifi_driver_cp2_state(0)) {
    WCND_LOGD("need to wait wifi driver to be unloaded before resetting");
    pWcndManger->wait_wifi_driver_unloaded = 1;
  }

  char buffer[255];

  pre_send_cp2_exception_notify();

  wcnd_send_selfcmd(pWcndManger, "wcn " WCND_SELF_EVENT_CP2_ASSERT);

   if(pWcndManger->is_ge2_error) {
     if (info_str)
        snprintf(buffer, sizeof(buffer), "%s %s", WCND_GE2_EXCEPTION_STRING, info_str);
     else
        snprintf(buffer, sizeof(buffer), "%s", WCND_GE2_EXCEPTION_STRING);
   }else{
  // notify exception
  if (info_str)
    snprintf(buffer, sizeof(buffer), "%s %s", WCND_CP2_EXCEPTION_STRING,
             info_str);
  else
    snprintf(buffer, sizeof(buffer), "%s", WCND_CP2_EXCEPTION_STRING);
  }
  wcnd_send_notify_to_client(pWcndManger, buffer, WCND_CLIENT_TYPE_NOTIFY);

  return 0;
}

/**
* handle the cp2 assert
* 1. send notify to client the cp2 assert
* 2. reset cp2
* 3. send notify to client the cp2 reset completed
*/
static int handle_cp2_assert(WcndManager *pWcndManger, int assert_fd) {
  if (!pWcndManger) {
    WCND_LOGE("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    return -1;
  }
  WCND_LOGD("handle_cp2_assert\n");
  if(pWcndManger->is_cp2_error) return -1;

  // set the cp2 error flag, it is cleared when reset successfully
  pWcndManger->is_cp2_error = 1;

  char rdbuffer[200];
  int len;

  memset(rdbuffer, 0, sizeof(rdbuffer));

  len = read(assert_fd, rdbuffer, sizeof(rdbuffer));
  if (len <= 0) {
    WCND_LOGE("read %d return %d, errno = %s", assert_fd, len, strerror(errno));
    // return -1;
  }

  if (pWcndManger->is_in_userdebug)
    sleep(15);  // add for waiting marlin watchdog to do some clean work, check
                // Bug#446762

  WCND_LOGD("handle_cp2_assert start\n");

  notify_cp2_exception(pWcndManger, rdbuffer);

  WCND_LOGD("handle_cp2_assert end!\n");

  return 0;
}


void *cp2_listen_thread(void *arg) {
  WcndManager *pWcndManger = (WcndManager *)arg;

  if (!pWcndManger) {
    WCND_LOGD("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    exit(-1);
  }

  int assert_fd = -1;

  // The count to check if assert dev node or watchdog dev node is ok
  int count = 5;

get_assertfd:
  assert_fd = open(pWcndManger->wcn_assert_iface_name, O_RDWR);
  WCND_LOGD("%s: open assert dev: %s, fd = %d", __func__,
            pWcndManger->wcn_assert_iface_name, assert_fd);
  if (assert_fd < 0) {
    WCND_LOGD("open %s failed, error: %s", pWcndManger->wcn_assert_iface_name,
              strerror(errno));
    sleep(2);
    if (count-- > 0) goto get_assertfd;
  }

  if (assert_fd < 0) {
    WCND_LOGE("Fail to open Assert Dev Node");
    return NULL;
  }

  while (1) {
    int i = 0;
    fd_set read_fds;
    int rc = 0;
    int max = -1;

    FD_ZERO(&read_fds);

    if (assert_fd > 0) {
      max = assert_fd;
      FD_SET(assert_fd, &read_fds);
    }

    WCND_LOGD("assert_fd = %d, max=%d", assert_fd, max);

    if(max < 0) {
      WCND_LOGE(
          "ERROR: have not fd to Select On (assert_fd = %d, "
          "max=%d)",
          assert_fd,  max);
      return NULL;
    }

    if ((rc = select(max + 1, &read_fds, NULL, NULL, NULL)) < 0) {
      if (errno == EINTR) continue;

      WCND_LOGD("select failed assert_fd = %d,  max=%d",
                assert_fd, max);
      sleep(1);
      continue;
    } else if (!rc) {
      continue;
    }

    if ((assert_fd > 0) && FD_ISSET(assert_fd, &read_fds)) {
      // there is exception from assert.
      handle_cp2_assert(pWcndManger, assert_fd);
    }

    // sleep for a while before going to next polling
    sleep(1);

    /* TODO:   */
  }
}
int check_boot_mode() {
    int fd = 0;
    char cmdline[1024] = {0};
    int eng_autotest = 0;
    int ret = 0;
    char value[PROPERTY_VALUE_MAX] = {'\0'};
    WCND_LOGD("check_boot_mode");
    property_get("ro.bootmode", value, "");

    if(strstr(value, "cali")) {
        WCND_LOGD("check_boot_mode cali");
        return 1;
    }
    fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, cmdline, sizeof(cmdline) - 1) > 0) {
            WCND_LOGD("check_boot_mode cmdline: %s", cmdline);
            if (strstr(cmdline, WCND_ENG_BBAT_STRING)) {
                WCND_LOGD("in BBAT mode!!!");
                ret = 1;
            }
        }
        close(fd);
    } else {
        WCND_LOGE("open /proc/cmdline failed, error: %s", strerror(errno));
    }
    return ret;
}

/**
* Initial the wcnd manager struct.
* return -1 for fail;
*/
int init(WcndManager *pWcndManger, int is_eng_only) {
  if (!pWcndManger) return -1;

  memset(pWcndManger, 0, sizeof(WcndManager));

  int i = 0;

  pthread_mutex_init(&pWcndManger->clients_lock, NULL);
  pthread_mutex_init(&pWcndManger->cmdexecuter_list_lock, NULL);

  for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) pWcndManger->clients[i].sockfd = -1;

  pWcndManger->is_eng_mode_only = is_eng_only;
  if (pWcndManger->is_eng_mode_only) {
#ifdef CONFIG_ENG_MODE
    WCND_LOGE("%s: ONLY for engineer mode!!", __FUNCTION__);

    pWcndManger->listen_fd = socket_local_server(
        WCND_ENG_SOCKET_NAME, ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
    if (pWcndManger->listen_fd < 0) {
      WCND_LOGE("%s: cannot create local socket server", __FUNCTION__);
      return -1;
    }
#else
    WCND_LOGE("%s: Not support eng mode", __FUNCTION__);
    return -1;
#endif
  } else {
    pWcndManger->listen_fd = socket_local_server(
        WCND_SOCKET_NAME, ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
    if (pWcndManger->listen_fd < 0) {
      WCND_LOGE("%s: cannot create local socket server", __FUNCTION__);
      return -1;
    }
  }

  snprintf(pWcndManger->wcn_assert_iface_name, WCND_MAX_IFACE_NAME_SIZE, "%s",
           WCN_ASSERT_IFACE);
  snprintf(pWcndManger->wcn_loop_iface_name, WCND_MAX_IFACE_NAME_SIZE, "%s",
           WCN_LOOP_IFACE);
  snprintf(pWcndManger->wcn_start_iface_name, WCND_MAX_IFACE_NAME_SIZE, "%s",
           WCN_START_IFACE);
  snprintf(pWcndManger->wcn_stop_iface_name, WCND_MAX_IFACE_NAME_SIZE, "%s",
           WCN_STOP_IFACE);
  snprintf(pWcndManger->wcn_download_iface_name, WCND_MAX_IFACE_NAME_SIZE, "%s",
           WCN_DOWNLOAD_IFACE);
  snprintf(pWcndManger->wcn_atcmd_iface_name, WCND_MAX_IFACE_NAME_SIZE, "%s",
           WCN_ATCMD_IFACE);
  WCND_LOGD(
      " WCN ASSERT Interface: %s \n  WCN LOOP "
      "Interface: %s \n WCN START Interface: %s \n"
      "WCN STOP Interface: %s \n WCN DOWNLAOD Interface: %s \n WCN IMAGE File: "
      "%s \n WCN ATCMD Interface: %s",
      pWcndManger->wcn_assert_iface_name,
      pWcndManger->wcn_loop_iface_name, pWcndManger->wcn_start_iface_name,
      pWcndManger->wcn_stop_iface_name, pWcndManger->wcn_download_iface_name,
      pWcndManger->wcn_image_file_name, pWcndManger->wcn_atcmd_iface_name);

  // To check if "persist.vendor.sys.wcnreset" is set or not. if not set it to be
  // default "1"
  char value[PROPERTY_VALUE_MAX] = {'\0'};
  if (property_get(WCND_RESET_PROP_KEY, value, NULL) <= 0) {
    property_set(WCND_RESET_PROP_KEY, "1");
  }

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pWcndManger->selfcmd_sockets) == -1) {
    WCND_LOGE("%s: cannot create socketpair for self cmd socket", __FUNCTION__);
    return -1;
  }

  // to get the wcn modem state
  pWcndManger->is_wcn_modem_enabled = check_if_wcnmodem_enable();

  // to get build type
  property_get(BUILD_TYPE_PROP_KEY, value, USER_DEBUG_VERSION_STR);
  if (strstr(value, USER_DEBUG_VERSION_STR)||check_boot_mode()) {
    pWcndManger->is_in_userdebug = 1;
    WCND_LOGD("userdebug version: %s!!!", value);
  }
  wcnd_cp2_log_init(pWcndManger);
  // init the cp2 status property
  property_set(WCND_CP2_STATE_PROP_KEY, "ok");

  wcnd_sm_init(pWcndManger);

  memcpy(pWcndManger->cp2_version_info, WCND_CP2_DEFAULT_CP2_VERSION_INFO,
         sizeof(WCND_CP2_DEFAULT_CP2_VERSION_INFO));
  pWcndManger->is_ge2_version = 1;

#ifdef CONFIG_ENG_MODE
  // start engineer worker thread
  wcnd_worker_init(pWcndManger);
#endif
  pWcndManger->inited = 1;

  return 0;
}
