#ifndef WCND_H__
#define WCND_H__

#include "wcnd_config.h"

#define WCND_DEBUG

#ifdef WCND_DEBUG
#define WCND_LOGD(x...) ALOGD(x)
#define WCND_LOGE(x...) ALOGE(x)
#else
#define WCND_LOGD(x...) \
  do {                  \
  } while (0)
#define WCND_LOGE(x...) \
  do {                  \
  } while (0)
#endif

#ifndef TEMP_FAILURE_RETRY
/* Used to retry syscalls that can return EINTR. */
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    typeof(exp) _rc;                       \
    do {                                   \
      _rc = (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#endif

#define USER_DEBUG_VERSION_STR "userdebug"

// the key used by property_get() to get partition path
#define PARTITION_PATH_PROP_KEY "ro.product.partitionpath"

// the key used to get the build type
#define BUILD_TYPE_PROP_KEY "ro.build.type"

#define WCND_SOCKET_NAME "wcnd"
#define WCND_ENG_SOCKET_NAME "wcnd_eng"


#define WCND_ENG_AUTOTEST_STRING "engtest autotest=1"
#define WCND_ENG_BBAT_STRING "androidboot.mode=autotest"


// WCND CLIENT TYPE
#define WCND_CLIENT_TYPE_SLEEP 0x30
#define WCND_CLIENT_TYPE_NOTIFY 0x00
#define WCND_CLIENT_TYPE_CMD 0x10
#define WCND_CLIENT_TYPE_CMD_PENDING 0x20
#define WCND_CLIENT_TYPE_CMD_SUBTYPE_CLOSE 0x11
#define WCND_CLIENT_TYPE_CMD_SUBTYPE_OPEN 0x12

#define WCND_CLIENT_TYPE_CMD_MASK 0xF0
#define RDWR_FD_FAIL (-2)
#define GENERIC_FAIL (-1)

typedef struct structWcndWorker {
  int (*handler)(void *ctx);
  void *ctx;
  void *data;
  int replyto_fd;  // fd to replay message
  struct structWcndWorker *next;
} WcndWorker;

typedef struct structWcndMessage {
  int event;
  int replyto_fd;  // fd to replay message
} WcndMessage;

typedef struct structWcndClient {
  int sockfd;
  int type;  // to identify if it is a socket for sending cmds or just for
             // listening event
} WcndClient;

typedef int (*cmd_handler)(int client_fd, int argc, char *argv[]);

typedef struct structWcnCmdExecuter {
  char *name;
  int (*runcommand)(int client_fd, int argc, char *argv[]);
} WcnCmdExecuter;

#define WCND_MAX_CLIENT_NUM (10)

#define WCND_MAX_IFACE_NAME_SIZE (32)

#define WCND_MAX_CMD_EXECUTER_NUM (10)

typedef struct structWcndManager {
  // identify if the struct is initialized or not.
  int inited;
  int is_engineer_poweron;
  pthread_mutex_t clients_lock;

  // to store the sockets that connect from the clients
  WcndClient clients[WCND_MAX_CLIENT_NUM];

  // the server socket to listen for client to connect
  int listen_fd;

  char wcn_assert_iface_name[WCND_MAX_IFACE_NAME_SIZE];
  char wcn_loop_iface_name[WCND_MAX_IFACE_NAME_SIZE];

  char wcn_stop_iface_name[WCND_MAX_IFACE_NAME_SIZE];
  char wcn_start_iface_name[WCND_MAX_IFACE_NAME_SIZE];
  char wcn_download_iface_name[WCND_MAX_IFACE_NAME_SIZE];
  char wcn_image_file_name[WCND_MAX_IFACE_NAME_SIZE];
  char wcn_atcmd_iface_name[WCND_MAX_IFACE_NAME_SIZE];

  int wcn_image_file_size;

  /*
  * count the notify information send, at the same time use as
  * a notify_id of the notify information send every time.
  * In order to identy the reponse of the notify information from
  * client, that means that the client must send back the notity_id in
  * the reponse of the notify information.
  */
  int notify_count;

  pthread_mutex_t cmdexecuter_list_lock;

  /**
  * Cmd executer to execute the cmd send from clients
  */
  const WcnCmdExecuter *cmdexecuter_list[WCND_MAX_CMD_EXECUTER_NUM];

  // to identify if a reset process is going on
  int doing_reset;
  // to identify if CP2 exception happened
  int is_cp2_error;
  // to identify if in the userdebug version
  int is_in_userdebug;

  int is_ge2_error;

  int is_ge2_version;

  // wcnd state
  int state;

  // bt/wifi state
  int btwifi_state;

  // saved bt/wifi state when assert happens
  int saved_btwifi_state;

  // pending events that will be handled in next state
  int pending_events;

  // self cmd socket pair to send cmd to self
  int selfcmd_sockets[2];

  // identify if enable to send notify
  int notify_enabled;

  // save the cp2 version info
  char cp2_version_info[256];
  int store_cp2_versin_done;

  pthread_mutex_t worker_lock;
  pthread_cond_t worker_cond;

  // engineer mode cmds queue
  WcndWorker *eng_cmd_queue;

  // to identify if the wcn modem (CP2) is enabled or not.
  // to check property "ro.modem.wcn.enable"
  int is_wcn_modem_enabled;

  // to identify if only for engineer mode, if yes, then the CP2 listen/loop
  // check will be disabled
  int is_eng_mode_only;

  // to saved the cp2 log state
  int is_cp2log_opened;
  int cp2_log_level;

  // to wait wifi driver unloaded before reset.
  int wait_wifi_driver_unloaded;

  // to identify if in the engpc autotest mode
  int eng_autotest;

  // dump mem in user mode??
  int dumpmem_on;

  // to identity marlin chip
  int marlin_type;

  // to check if it has been checked
  int loopcheck_checked;
} WcndManager;


// export API
WcndManager *wcnd_get_default_manager(void);
int wcnd_register_cmdexecuter(WcndManager *pWcndManger,
                              const WcnCmdExecuter *pCmdExecuter);
int wcnd_worker_init(WcndManager *pWcndManger);
int wcnd_worker_dispatch(WcndManager *pWcndManger, int (*handler)(void *),
                         char *data, int fd, int type);

#endif  // WCND_H_
