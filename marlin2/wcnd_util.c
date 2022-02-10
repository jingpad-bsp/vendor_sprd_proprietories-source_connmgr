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
#include <dirent.h>

#include "wcnd_marlin2.h"

#include <cutils/properties.h>

#include <fcntl.h>
#include <sys/socket.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/limits.h>

#include <netinet/in.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <errno.h>
#include <netutils/ifc.h>
/**
* may do some clear action that will used during reset
* such as wcnlog.result property.
*/
void pre_send_cp2_exception_notify(void) {
  /* Erase any previous setting of the slogresult property */
  property_set(WCND_SLOG_RESULT_PROP_KEY, "0");
}

/**
* check "ro.vendor.modem.wcn.enable" property to see if cp2 enabled or not.
* default is enabled
* return non-zero for true.
*/
int check_if_wcnmodem_enable(void) {
  char value[PROPERTY_VALUE_MAX] = {'\0'};

  property_get(WCND_MODEM_ENABLE_PROP_KEY, value, "0");
  int is_enabled = atoi(value);

  return is_enabled;
}

int wcnd_register_cmdexecuter(WcndManager *pWcndManger,
                              const WcnCmdExecuter *pCmdExecuter) {
  if (!pWcndManger || !pCmdExecuter) return -1;

  if (!pWcndManger->inited) {
    WCND_LOGE("WcndManager IS NOT INITIALIZED ! ");
    return -1;
  }

  int i = 0;
  int ret = 0;
  pthread_mutex_lock(&pWcndManger->cmdexecuter_list_lock);

  for (i = 0; i < WCND_MAX_CMD_EXECUTER_NUM; i++) {
    if (!pWcndManger->cmdexecuter_list[i]) {  // empty
      pWcndManger->cmdexecuter_list[i] = pCmdExecuter;
      break;
    } else if (pWcndManger->cmdexecuter_list[i] == pCmdExecuter) {
      WCND_LOGD("cmd executer:%p has been register before!", pCmdExecuter);
      break;
    }
  }

  pthread_mutex_unlock(&pWcndManger->cmdexecuter_list_lock);

  if (WCND_MAX_CMD_EXECUTER_NUM == i) {
    WCND_LOGE("ERRORR::%s: cmdexecuter_list is FULL", __FUNCTION__);
    ret = -1;
  }

  return ret;
}


int wcnd_send_selfcmd(WcndManager *pWcndManger, char *cmd) {
  return TEMP_FAILURE_RETRY(write(pWcndManger->selfcmd_sockets[0], cmd,
                                  strlen(cmd) + 1));  // including the 'null'
}

/**
* static API
*/
#define OK_STR "OK"
#define FAIL_STR "FAIL"

int send_back_cmd_result(int client_fd, char *str, int isOK) {
  char buffer[255];

  if (client_fd < 0) return -1;

  memset(buffer, 0, sizeof(buffer));

  if (!str) {
    snprintf(buffer, sizeof(buffer), "%s", (isOK ? OK_STR : FAIL_STR));
  } else {
    snprintf(buffer, sizeof(buffer), "%s %s", (isOK ? OK_STR : FAIL_STR), str);
  }

  int ret = write(client_fd, buffer, strlen(buffer) + 1);
  if (ret < 0) {
    WCND_LOGE("write %s to client_fd:%d fail (error:%s)", buffer, client_fd,
              strerror(errno));
    return -1;
  }

  return 0;
}

int wcnd_send_back_cmd_result(int client_fd, char *str, int isOK) {
  return send_back_cmd_result(client_fd, str, isOK);
}

/**
* cmd format:
* module_name [submodule_name cmd | cmd]  arg1 arg2 ...
* eg:
* wcn reset
* eng iwnapi getmaxpower
* Note: data must end with null character.
*/
static void dispatch_command2(WcndManager *pWcndManger, int client_fd,
                              char *data) {
  if (!pWcndManger || !data) {
    send_back_cmd_result(client_fd, "Null pointer!!", 0);
    return;
  }

  int i, j = 0;
  int argc = 0;

#define WCND_CMD_ARGS_MAX (16)

  char *argv[WCND_CMD_ARGS_MAX];
  char tmp[255];
  char *p = data;
  char *q = tmp;
  char *qlimit = tmp + sizeof(tmp) - 1;
  int esc = 0;
  int quote = 0;
  int k;
  int haveCmdNum = 0;

  memset(argv, 0, sizeof(argv));
  memset(tmp, 0, sizeof(tmp));

  while (*p) {
    if (*p == '\\') {
      if (esc) {
        if (q >= qlimit) goto overflow;
        *q++ = '\\';
        esc = 0;
      } else {
        esc = 1;
      }
      p++;
      continue;
    } else if (esc) {
      if (*p == '"') {
        if (q >= qlimit) goto overflow;
        *q++ = '"';
      } else if (*p == '\\') {
        if (q >= qlimit) goto overflow;
        *q++ = '\\';
      } else {
        send_back_cmd_result(client_fd, "Unsupported escape sequence", 0);
        goto out;
      }
      p++;
      esc = 0;
      continue;
    }

    if (*p == '"') {
      if (quote)
        quote = 0;
      else
        quote = 1;
      p++;
      continue;
    }

    if (q >= qlimit) goto overflow;
    *q = *p++;
    if (!quote && *q == ' ') {
      *q = '\0';
      if (haveCmdNum) {
        char *endptr;
        int cmdNum = (int)strtol(tmp, &endptr, 0);
        if (endptr == NULL || *endptr != '\0') {
          send_back_cmd_result(client_fd, "Invalid sequence number", 0);
          goto out;
        }
        // TDO Save Cmd Num
        haveCmdNum = 0;
      } else {
        if (argc >= WCND_CMD_ARGS_MAX) goto overflow;
        argv[argc++] = strdup(tmp);
      }
      memset(tmp, 0, sizeof(tmp));
      q = tmp;
      continue;
    }
    q++;
  }

  *q = '\0';
  if (argc >= WCND_CMD_ARGS_MAX) goto overflow;

  argv[argc++] = strdup(tmp);

  for (k = 0; k < argc; k++) {
    WCND_LOGD("%s: arg[%d] = '%s'", __FUNCTION__, k, argv[k]);
  }

  if (quote) {
    send_back_cmd_result(client_fd, "Unclosed quotes error", 0);
    goto out;
  }

  cmd_handler handler = NULL;

  pthread_mutex_lock(&pWcndManger->cmdexecuter_list_lock);
  for (i = 0; i < WCND_MAX_CMD_EXECUTER_NUM; ++i) {
    if (pWcndManger->cmdexecuter_list[i] &&
        pWcndManger->cmdexecuter_list[i]->name &&
        (!strcmp(argv[0], pWcndManger->cmdexecuter_list[i]->name))) {
      handler = pWcndManger->cmdexecuter_list[i]->runcommand;
      break;
    }
  }
  pthread_mutex_unlock(&pWcndManger->cmdexecuter_list_lock);

  if (!handler) {
    send_back_cmd_result(client_fd, "Command not recognized", 0);
    goto out;
  }

  handler(client_fd, argc - 1, &argv[1]);

out:
  for (j = 0; j < argc; j++) free(argv[j]);
  return;

overflow:
  send_back_cmd_result(client_fd, "Command too long", 0);
  goto out;
}
/**
* To handle the worker. That is to handle the command.
*/
int wcnd_woker_handle(void *worker) {
  if (!worker) return -1;
  WcndWorker *pWorker = (WcndWorker *)worker;
  WcndManager *pWcndManger = (WcndManager *)(pWorker->ctx);
  int client_fd = pWorker->replyto_fd;
  char *data = pWorker->data;

  if (!pWcndManger || !data) {
    send_back_cmd_result(client_fd, "Null pointer!!", 0);
    return -1;
  }

  dispatch_command2(pWcndManger, client_fd, data);

  return 0;
}

/**
* if it is a engineer mode command, dispatch it to engineer worker thread
*/
static int worker_dispatch(WcndManager *pWcndManger, int client_fd,
                           char *data) {
  if (!pWcndManger || !data) {
    send_back_cmd_result(client_fd, "Null pointer!!", 0);
    return -1;
  }

  // for engineer mode command, dispatch it to engineer worker thread
  if (strstr(data, "eng")) {
#ifdef CONFIG_ENG_MODE
    if (wcnd_worker_dispatch(pWcndManger, wcnd_woker_handle, data, client_fd,
                             0) < 0) {
      send_back_cmd_result(client_fd, "eng cmd dispatch error!!", 0);
      return -1;
    }
#else
    send_back_cmd_result(client_fd, "eng cmd dispatch error!!", 0);
    return -1;
#endif
  } else {
    dispatch_command2(pWcndManger, client_fd, data);
  }

  return 0;
}

/**
* the data string may contain multi cmds, each cmds end with null character.
*/
void dispatch_command(WcndManager *pWcndManger, int client_fd,
                             char *data, int data_len) {
  char buffer[255];
  int count = 0, start = 0, end = data_len;

  if (!data || data_len >= 255) return;
  memset(buffer, 0, sizeof(buffer));
  memcpy(buffer, data, data_len);
  while (buffer[count] == ' ') count++;  // remove blankspace

  start = count;

  while (count < end) {
    if (buffer[count] == '\0') {
      if (start < count) {
        worker_dispatch(pWcndManger, client_fd, buffer + start);
      }
      start = count + 1;
    }

    count++;
  }

  if (buffer[end] != '\0') {
    WCND_LOGE("%s: cmd should end with null character!", __FUNCTION__);
  } else if (start < count) {
    worker_dispatch(pWcndManger, client_fd, buffer + start);
  }

  return;
}

/**
* Note: message send from wcnd must end with null character
*             use null character to identify a completed message.
*/
int send_msg(WcndManager *pWcndManger, int client_fd, char *msg_str) {
  if (!pWcndManger || !msg_str) return GENERIC_FAIL;

  char *buf;

  pWcndManger->notify_count++;
  buf = msg_str;

  /* Send the zero-terminated message */

  // Note: message send from wcnd must end with null character
  // use null character to identify a completed message.
  int len = strlen(buf) + 1;  // including null character

  WCND_LOGD("send %s to client_fd:%d", buf, client_fd);

  int ret = TEMP_FAILURE_RETRY(write(client_fd, buf, len));
  if (ret < 0) {
    WCND_LOGE("write %d bytes to client_fd:%d fail (error:%s)", len, client_fd,
              strerror(errno));
    ret = RDWR_FD_FAIL;
  }

  return ret;
}

int wcnd_split_int(char* string, char *ch)
{
    char *ptr,*retptr;
    int i=0;
    int ret;
    ptr = string;
    while ((retptr=strtok(ptr, ch)) != NULL) {
        WCND_LOGD("substr[%d]:%s\n", i, retptr);
        if(i == 1){
            ret = atoi(retptr);
            return ret;
        }
        i++;
        ptr = NULL;
    }
    return -1;
}


static int wcnd_read_to_buf(const char *filename, char *buf, int buf_size) {
  int fd;

  if (buf_size <= 1) return -1;

  ssize_t ret = -1;
  fd = open(filename, O_RDONLY);
  if (fd >= 0) {
    ret = read(fd, buf, buf_size - 1);
    close(fd);
  }
  ((char *)buf)[ret > 0 ? ret : 0] = '\0';
  return ret;
}

static unsigned wcnd_strtou(const char *string) {
  unsigned long v;
  char *endptr;
  char **endp = &endptr;

  if (!string) return UINT_MAX;

  *endp = (char *)string;

  if (!isalnum(string[0])) return UINT_MAX;
  errno = 0;
  v = strtoul(string, endp, 10);
  if (v > UINT_MAX) return UINT_MAX;

  char next_ch = **endp;
  if (next_ch) {
    /* "1234abcg" or out-of-range */
    if (isalnum(next_ch) || errno) return UINT_MAX;

    /* good number, just suspicious terminator */
    errno = EINVAL;
  }

  return v;
}

static pid_t wcnd_find_pid_by_name(const char *proc_name) {
  pid_t target_pid = 0;

  DIR *proc_dir = NULL;
  struct dirent *entry = NULL;

  if (!proc_name) return 0;

  proc_dir = opendir("/proc");

  if (!proc_dir) {
    WCND_LOGE("open /proc fail: %s", strerror(errno));
    return 0;
  }

  while ((entry = readdir(proc_dir))) {
    char buf[1024];
    unsigned pid;
    int n;
    char filename[sizeof("/proc/%u/task/%u/cmdline") + sizeof(int) * 3 * 2];

    pid = wcnd_strtou(entry->d_name);
    if (errno) continue;

    sprintf(filename, "/proc/%u/cmdline", pid);

    n = wcnd_read_to_buf(filename, buf, 1024);

    if (n < 0) continue;

    //WCND_LOGD("pid: %d, command name: %s", pid, buf);

    if (strcmp(buf, proc_name) == 0) {
      WCND_LOGD("find pid: %d for target process name: %s", pid, proc_name);

      target_pid = pid;

      break;
    }
  } /* for (;;) */

  if (proc_dir) closedir(proc_dir);

  return target_pid;
}

/**
* to check if the process with the process name or pid exist
* return 0: the process does not exist
* return > 0: the process still exist
* return < 0: other error happens
*/
int wcnd_check_process_exist(const char *proc_name, int proc_pid) {
  pid_t target_pid = 0;

  DIR *proc_dir = NULL;
  struct dirent *entry = NULL;

  if (!proc_name && proc_pid <= 0) return 0;

  proc_dir = opendir("/proc");

  if (!proc_dir) {
    WCND_LOGE("open /proc fail: %s", strerror(errno));
    return -1;
  }

  while ((entry = readdir(proc_dir))) {
    char buf[1024];
    unsigned pid;
    int n;
    char filename[sizeof("/proc/%u/task/%u/cmdline") + sizeof(int) * 3 * 2];

    pid = wcnd_strtou(entry->d_name);
    if (errno) continue;

    sprintf(filename, "/proc/%u/cmdline", pid);

    n = wcnd_read_to_buf(filename, buf, 1024);

    if (n < 0) continue;

    //WCND_LOGD("pid: %d, command name: %s", pid, buf);

    if (proc_name && (strcmp(buf, proc_name) == 0)) {
      WCND_LOGD("find pid: %d for target process name: %s", pid, proc_name);

      target_pid = pid;

      break;
    }

    if (proc_pid > 0 && pid == (unsigned)proc_pid) {
      WCND_LOGD("find process by pid: %d for target process name: %s", pid,
                (proc_name ? proc_name : ""));

      target_pid = pid;

      break;
    }
  } /* for (;;) */

  if (proc_dir) closedir(proc_dir);

  if (target_pid == 0)
    WCND_LOGD("The Process for (%d)%s does not exist!!", proc_pid,
              (proc_name ? proc_name : ""));

  return (int)target_pid;
}

int wcnd_kill_process(pid_t pid, int signal) {
  // signal such as SIGKILL
  return kill(pid, signal);
}

/**
* return -2: for the target process is not exist
* return -1 for fail
* return > 0 the target pid
*/
int wcnd_kill_process_by_name(const char *proc_name, int signal) {
  if (!proc_name) return WCND_KILL_PROCESS_NO_EXIT;

  pid_t target_pid = wcnd_find_pid_by_name(proc_name);

  if (target_pid == 0) {
    WCND_LOGD("Cannot find the target pid!!");
    return WCND_KILL_PROCESS_NO_EXIT;
  }

  WCND_LOGD("kill %s by signal: %d\n", proc_name, signal);

  if (wcnd_kill_process(target_pid, signal) < 0) {
    WCND_LOGE("Error kill process: %s", strerror(errno));

    return WCND_KILL_PROCESS_FAILURE;
  }

  return (int)target_pid;
}

/**
 * Return 0: for the process is not exist.
 * Otherwise return the pid of the process.
 */
int wcnd_find_process_by_name(const char *proc_name) {
  pid_t target_pid = wcnd_find_pid_by_name(proc_name);

  if (target_pid == 0) {
    WCND_LOGD("Cannot find the target process!!");
    return 0;
  }

  return (int)target_pid;
}

int wcnd_down_network_interface(const char *ifname) {
  ifc_init();

  if (ifc_down(ifname)) {
    WCND_LOGE("Error downing interface: %s", strerror(errno));
  }
  ifc_close();
  return 0;
}

int wcnd_up_network_interface(const char *ifname) {
  ifc_init();

  if (ifc_up(ifname)) {
    WCND_LOGE("Error upping interface: %s", strerror(errno));
  }
  ifc_close();
  return 0;
}


static const char WIFI_DRIVER_PROP_NAME[] = "wlan.driver.status";

void wcnd_wait_for_driver_unloaded(void) {
  char driver_status[PROPERTY_VALUE_MAX];

  int count = 100; /* wait at most 20 seconds for completion */
  while (count-- > 0) {
    if (!property_get(WIFI_DRIVER_PROP_NAME, driver_status, NULL) ||
        strcmp(driver_status, "ok") != 0) /* driver not loaded */
      break;
    usleep(200000);
    // WCND_LOGE("status: %s", driver_status);
  }

  if (count <= 0) {
    WCND_LOGE("Error Wifi driver cannot unloaded in 20 seconds");
  } else {
    WCND_LOGE("Wifi driver has been unloaded");
  }
}

/**
* To notify the wifi driver with the cp2 state.
* state_ok: 0, then cp2 is assert.
* return  0 for sucess.
*  1 means driver is removing, wcnd need to wait driver to be unloaded
*/
#define WIFI_DRIVER_PRIV_CMD_SET_CP2_ASSERT (0x10)

#define WIFI_DRIVER_STATE_REMOVEING (0x11)

int wcnd_notify_wifi_driver_cp2_state(int state_ok) {
  int fd, ret, index, len, cmd, data;
  char buf[32] = {0};

  fd = open("/proc/wlan", O_RDWR);
  if (fd < 0) {
    WCND_LOGE("[%s][open][ret:%d]\n", __func__, fd);
    return 0;
  }
  cmd = 0x10;  // WIFI_DRIVER_PRIV_CMD_SET_CP2_ASSERT
  data = 0x00;
  len = 0x04;
  index = 0x00;
  memcpy(&buf[index], (char *)(&len), 4);
  index += 4;
  memcpy(&buf[index], (char *)(&data), 4);
  index += 4;
  ret = ioctl(fd, cmd, &buf[0]);
  if (ret < 0) {
    WCND_LOGE("[%s][ioctl][ret:%d]\n", __func__, ret);
    close(fd);
    return 0;
  }
  memcpy((char *)(&ret), &buf[0], 4);
  if (0x11 == ret) {  // WIFI_DRIVER_STATE_REMOVEING
    WCND_LOGE("marlin wifi drv removing\n");
    return 1;
  }
  close(fd);

  WCND_LOGD("notify marlin wifi drv cp2 assert success\n");

  return 0;
}

/**
* To find the "target" file or dir in the specified path.
* special case: to find the file or dir, its name contains the "target" string.
*
* return  0 : not foud
*  1 : find it
* -1 : error
*/
int wcnd_find(const char *path, const char *target) {
  struct stat buf;
  int is_dir = 0;

  if (!path || !target) return -1;

  int r = stat(path, &buf);
  if (r == 0) {
    if (S_ISDIR(buf.st_mode)) is_dir = 1;
  } else {
    WCND_LOGE("error for _stat(%s):(error:%s)\n", path, strerror(errno));
    return -1;
  }

  if (!is_dir) {
    if (strstr(path, target))
      return 1;
    else
      return 0;
  }

  // path is dir
  DIR *find_dir = NULL;
  struct dirent *entry = NULL;
  int ret = 0;

  find_dir = opendir(path);

  if (!find_dir) {
    WCND_LOGE("open %s fail: %s", path, strerror(errno));
    return -1;
  }

  while ((entry = readdir(find_dir))) {

    if (strstr(entry->d_name, target)) {
      WCND_LOGD("found: %s for %s\n", entry->d_name, target);
      ret = 1;
      goto out;
    }

  } /* for (;;) */

out:
  if (find_dir) closedir(find_dir);

  return ret;
}

/**
 * To stop the process with the specified name.
 * in: const char *proc_name -->  the name of the process
 * in: int time_out --> time out value in seconds to wait process to be stopped
 * Return:
 *    1: success
 *    0: timeout
 *    -1: fail
 */
int wcnd_stop_process(const char *proc_name, int time_out) {

  if (!proc_name) return -1;

  int ret = wcnd_kill_process_by_name(proc_name, SIGINT);

  // have send sig to target process, wait it to exit
  if(-2 != ret) {
    //check if process is killed

    int count = 50;
    if (time_out > 0) count = time_out * 10;

    while(count > 0) {
      int check_ret = wcnd_check_process_exist(proc_name, ret);
	   // if the target process does not exist or the original exited,
	   // and a new process is detected
      if(!check_ret || (check_ret > 0 && ret > 0 && check_ret != ret))
        break;
      else
        count--;

      if (count == 0)
        return 0; // timeout

      usleep(100*1000); //100ms
    }
  }

  return 1;
}

