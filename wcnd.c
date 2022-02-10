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

#define LOG_TAG "WCND"
#define AID_SYSTEM 1000 /* system server */
#ifdef CONFIG_ENG_MODE
// external cmd executer declare here.
extern WcnCmdExecuter wcn_eng_cmdexecuter;
#endif

extern int wcnd_runcommand(int client_fd, int argc, char *argv[]);
extern void *cp2_listen_thread(void *arg);
extern void *cp2_loop_check_thread(void *arg);
extern void dispatch_command(WcndManager *pWcndManger, int client_fd,
                             char *data, int data_len);
extern int init(WcndManager *pWcndManger, int is_eng_only);
int store_cp2_version_info(WcndManager *pWcndManger);

/**
* static variables
*/
static WcndManager default_wcn_manager;

/*
* wcn cmd executer to executer cmd that relate to CP2 assert
* such as:
* (1) reset  //to reset the CP2
*/
static const WcnCmdExecuter wcn_cmdexecuter = {
    .name = "wcn", .runcommand = wcnd_runcommand,
};

WcndManager *wcnd_get_default_manager(void) {
    return &default_wcn_manager;
}

/*
 * To block the pipe signal to avoid the process exit.
 */
static void blockSigpipe(void) {
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
        WCND_LOGD("WARNING: SIGPIPE not blocked\n");
}

/**
* store the fd in the client_fds array.
* when the array is full, then something will go wrong, so please NOTE!!!
*/
static void store_client_fd(int client_fds[], int fd) {
    int i = 0;

    if (!client_fds) return;

    for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
        if (client_fds[i] == -1) {  // invalid fd
            client_fds[i] = fd;
            return;
        } else if (client_fds[i] == fd) {
            WCND_LOGD("%s: Somethine error happens. restore the same fd:%d",
                      __FUNCTION__, fd);
            return;
        }
    }

    // if full, ignore the last one, and save the new one
    if (i == WCND_MAX_CLIENT_NUM) {
        WCND_LOGD("ERRORR::%s: client_fds is FULL", __FUNCTION__);
        client_fds[i - 1] = fd;
        return;
    }
}


/**
* process the active client fd ( the client close the remote socket or send
* something)
*
* return < 0 for fail;
* TODO: can handle the cmd sent from the client here
*/
static int process_active_client_fd(WcndManager *pWcndManger, int fd) {
  char buffer[255];
  int len;

  memset(buffer, 0, sizeof(buffer));
  len = TEMP_FAILURE_RETRY(
      read(fd, buffer, sizeof(buffer) - 1));  // reserve last byte for null character.
  if (len < 0) {
    WCND_LOGD("read() failed len < 0, as the exception of (%s)", strerror(errno));
    return RDWR_FD_FAIL;
  } else if (0 == len) {
    WCND_LOGD("read() success len == 0, as the peer of client fd(%d) is being closed normally", fd);
    return RDWR_FD_FAIL;
  }

  dispatch_command(pWcndManger, fd, buffer,
                   len + 1);  // len+1 make sure string end with null character.

  return 0;
}

/**
* listen on the server socket for accept connection from client.
* and then also set the client socket to select read fds for:
* 1. to detect the exception from client, then to close the client socket.
* 2. to process the cmd from client in feature.
*/
static void *client_listen_thread(void *arg) {
  WcndManager *pWcndManger = (WcndManager *)arg;

  if (!pWcndManger) {
    WCND_LOGD("%s: UNEXCPET NULL WcndManager", __FUNCTION__);
    exit(-1);
  }

  int pending_fds[WCND_MAX_CLIENT_NUM];
  memset(pending_fds, -1, sizeof(pending_fds));

  while (1) {
    int i = 0;
    fd_set read_fds;
    int rc = 0;
    int max = -1;

    FD_ZERO(&read_fds);

    max = pWcndManger->listen_fd;
    FD_SET(pWcndManger->listen_fd, &read_fds);

    FD_SET(pWcndManger->selfcmd_sockets[1], &read_fds);
    if (pWcndManger->selfcmd_sockets[1] > max)
      max = pWcndManger->selfcmd_sockets[1];

    // if need to deal with the cmd sent from client, here add them to read_fds
    pthread_mutex_lock(&pWcndManger->clients_lock);
    for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
      int fd = pWcndManger->clients[i].sockfd;
      if (fd != -1) {  // valid fd
        FD_SET(fd, &read_fds);
        if (fd > max) max = fd;
      }
    }
    pthread_mutex_unlock(&pWcndManger->clients_lock);

    WCND_LOGD("listen_fd = %d, max=%d", pWcndManger->listen_fd, max);
    if ((rc = select(max + 1, &read_fds, NULL, NULL, NULL)) < 0) {
      if (errno == EINTR) continue;

      WCND_LOGD("select failed (%s) listen_fd = %d, max=%d", strerror(errno),
                pWcndManger->listen_fd, max);
      sleep(1);
      continue;
    } else if (!rc) {
      continue;
    }

    if (FD_ISSET(pWcndManger->listen_fd, &read_fds)) {
      struct sockaddr addr;
      socklen_t alen;
      int c;

      // accept the client connection
      do {
        alen = sizeof(addr);
        c = accept(pWcndManger->listen_fd, &addr, &alen);
        WCND_LOGD("%s got %d from accept", WCND_SOCKET_NAME, c);
      } while (c < 0 && errno == EINTR);

      if (c < 0) {
        WCND_LOGE("accept failed (%s)", strerror(errno));
        sleep(1);
        continue;
      }

      // save client
      pthread_mutex_lock(&pWcndManger->clients_lock);
      for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
        if (pWcndManger->clients[i].sockfd == -1) {  // invalid fd
          pWcndManger->clients[i].sockfd = c;
          pWcndManger->clients[i].type = WCND_CLIENT_TYPE_NOTIFY;
          break;

        } else if (pWcndManger->clients[i].sockfd == c) {
          WCND_LOGD("%s: Somethine error happens. restore the same fd:%d",
                    __FUNCTION__, c);
          break;
        }
      }

      // if full, ignore the last one, and save the new one
      if (i == WCND_MAX_CLIENT_NUM) {
        WCND_LOGD("ERRORR::%s: clients is FULL", __FUNCTION__);
        close(c);
      }
      pthread_mutex_unlock(&pWcndManger->clients_lock);
    }

    /* Add all active clients to the pending list first */
    memset(pending_fds, -1, sizeof(pending_fds));
    pthread_mutex_lock(&pWcndManger->clients_lock);
    for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
      int fd = pWcndManger->clients[i].sockfd;
      if ((fd != -1) && FD_ISSET(fd, &read_fds)) {
        store_client_fd(pending_fds, fd);
      }
    }
    pthread_mutex_unlock(&pWcndManger->clients_lock);

    if (FD_ISSET(pWcndManger->selfcmd_sockets[1], &read_fds))
      store_client_fd(pending_fds, pWcndManger->selfcmd_sockets[1]);

    /* Process the pending list, since it is owned by the thread, there is no
     * need to lock it */
    for (i = 0; i < WCND_MAX_CLIENT_NUM; i++) {
      int fd = pending_fds[i];

      /* remove from the pending list */
      pending_fds[i] = -1;

      /* Process it, if fail is returned and our sockets are connection-based,
       * remove and destroy it */
      if ((fd != -1) &&
          (process_active_client_fd(pWcndManger, fd) == RDWR_FD_FAIL)) {
        int j = 0;

        /* Remove the client from our array */
        WCND_LOGD("going to zap client fd of %d for %s", fd, WCND_SOCKET_NAME);
        pthread_mutex_lock(&pWcndManger->clients_lock);
        for (j = 0; j < WCND_MAX_CLIENT_NUM; j++) {
          if (pWcndManger->clients[j].sockfd == fd) {
            close(fd);  // close the socket
            pWcndManger->clients[j].sockfd = -1;
            pWcndManger->clients[j].type = WCND_CLIENT_TYPE_NOTIFY;
            break;
          }
        }
        pthread_mutex_unlock(&pWcndManger->clients_lock);
      }
    }
  }
}


/**
* Start thread to listen to client's connection.
*/
static int start_client_listener(WcndManager *pWcndManger) {
  if (!pWcndManger) return -1;

  pthread_t thread_id;

  if (pthread_create(&thread_id, NULL, client_listen_thread, pWcndManger)) {
    WCND_LOGE("start_client_listener: pthread_create (%s)", strerror(errno));
    return -1;
  }

  return 0;
}

/**
* Start thread to listen on the CP2 assert/watchdog interface to detect CP2
* exception
* return -1 fail;
*/
static int start_cp2_listener(WcndManager *pWcndManger) {
  if (!pWcndManger) return -1;

  if (pWcndManger->is_eng_mode_only) return 0;

  if (!pWcndManger->is_wcn_modem_enabled) return 0;

  pthread_t thread_id;

  if (pthread_create(&thread_id, NULL, cp2_listen_thread, pWcndManger)) {
    WCND_LOGE("start_cp2_listener: pthread_create (%s)", strerror(errno));
    return -1;
  }

  return 0;
}

/**
 * Check the kernel command line
 * if in eng autotest mode, when wcnd will start engpcclientwcn
 * return 0: not in the engtest autotest mode
 * return 1: in the engtest autotest mode
 */
static int check_kernel_cmdline(WcndManager *pWcndManger) {
  int fd = 0;
  char cmdline[1024] = {0};
  int eng_autotest = 0;

  WCND_LOGD("check_kernel_cmdline");

  fd = open("/proc/cmdline", O_RDONLY);
  if (fd >= 0) {
    if (read(fd, cmdline, sizeof(cmdline) - 1) > 0) {
      WCND_LOGD("kernel cmd line: %s", cmdline);

      if (strstr(cmdline, WCND_ENG_AUTOTEST_STRING)) {
        WCND_LOGD("in eng autotest mode!!!");
        eng_autotest = 1;
        if (pWcndManger) pWcndManger->eng_autotest = 1;
      }
    }
    close(fd);
  } else {
    WCND_LOGE("open /proc/cmdline failed, error: %s", strerror(errno));
  }

  return eng_autotest;
}

/**
* Start thread to loop check if CP2 is alive.
* return -1 fail;
*/
static int start_cp2_loop_check(WcndManager *pWcndManger) {
  if (!pWcndManger) return -1;

  if (pWcndManger->is_eng_mode_only) return 0;

  // if wcn modem is not enabled, just return
  if (!pWcndManger->is_wcn_modem_enabled) return 0;

  pthread_t thread_id;

  if (pthread_create(&thread_id, NULL, cp2_loop_check_thread, pWcndManger)) {
    WCND_LOGE("start_cp2_loop_check: pthread_create (%s)", strerror(errno));
    return -1;
  }

  return 0;
}

/**
* to switch to system user and set the cap
*/
static int os_process_init(void) {
  struct __user_cap_header_struct header;
  struct __user_cap_data_struct cap[2];

  prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);

  setuid(AID_SYSTEM);
  memset(&header, 0, sizeof(header));
  header.version = _LINUX_CAPABILITY_VERSION_3;
  header.pid = 0;

  memset(&cap, 0, sizeof(cap));
  cap[0].effective = cap[0].permitted =
      (1 << CAP_NET_ADMIN) | (1 << CAP_NET_RAW) | (1 << CAP_KILL);
  cap[0].inheritable = 0;
  cap[1].effective = cap[1].permitted = CAP_TO_MASK(CAP_WAKE_ALARM);
  cap[1].inheritable = 0;
  if (capset(&header, &cap[0]) < 0)
    WCND_LOGD("capset failed");

  /******* for debug******/
  gid_t list[64];
  int n, max;

  max = getgroups(64, list);
  if (max < 0) max = 0;

  WCND_LOGD("uid: %d,", getuid());

  WCND_LOGD("gid: %d,", getgid());
  if (max) {
    for (n = 0; n < max; n++) WCND_LOGD("group id %d: %d,", n, list[n]);
  }
  /******* for debug end******/

  memset(&cap, 0, sizeof(cap));
  capget(&header, &cap[0]);
  WCND_LOGD("capget version=%x, pid=%d", header.version, header.pid);
  WCND_LOGD("cap[0] = %x, cap[1] = %x", cap[0].permitted, cap[1].permitted);

  return 0;
}

int main(int argc, char *argv[]) {
  int c;
  int is_eng_only = 0;
  int is_recovery = 0;
  int autotest = 0;

  for (;;) {
    c = getopt(argc, argv, "GR");
    if (c < 0) break;
    switch (c) {
      case 'G':  // -G is engineer mode only
        is_eng_only = 1;
        break;
      case 'R':
        is_recovery = 1;
        break;
      default:
        break;
    }
  }

  if (!is_eng_only) {
    // check if in the engpc autotest mode
    autotest = check_kernel_cmdline(NULL);

    // if it is not an engineer mode only, then need to switch the user/group
    os_process_init();

   // generate_wifi_mac();
   // generate_bt_mac();
  }

  blockSigpipe();

  WcndManager *pWcndManger = wcnd_get_default_manager();
  if (!pWcndManger) {
    WCND_LOGE("wcnd_get_default_manager Fail!!!");
    return -1;
  }
  if (init(pWcndManger, is_eng_only) < 0) {
    WCND_LOGE("Init pWcnManager Fail!!!");
    return -1;
  }

  if (start_client_listener(pWcndManger) < 0) {
    WCND_LOGE("Start client listener Fail!!!");
    return -1;
  }
  if (start_cp2_listener(pWcndManger) < 0) {
    WCND_LOGE("Start CP2 listener Fail!!!");
    return -1;
  }
  // Start engineer service , such as for get CP2 log from PC.
  pWcndManger->eng_autotest = autotest;
  // register builin cmd executer
  wcnd_register_cmdexecuter(pWcndManger, &wcn_cmdexecuter);

#ifdef CONFIG_ENG_MODE
  // register external cmd executer such eng mode
  wcnd_register_cmdexecuter(pWcndManger, &wcn_eng_cmdexecuter);
#endif

  if (start_cp2_loop_check(pWcndManger) < 0) {
    WCND_LOGE("Start CP2loop_check Fail!!!");
  }

  // get CP2 version and save it
   store_cp2_version_info(pWcndManger);

  // do nothing, just sleep
  do {
    sleep(1000);
  } while (1);

  return 0;
}
