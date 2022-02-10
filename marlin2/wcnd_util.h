#ifndef WCND_UTIL_H__
#define WCND_UTIL_H__

#include <sys/types.h>
#include <signal.h>

#define WCND_MODEM_ENABLE_PROP_KEY "ro.vendor.modem.wcn.enable"

#define WCND_KILL_PROCESS_NO_EXIT -2
#define WCND_KILL_PROCESS_FAILURE -1


int wcnd_kill_process(pid_t pid, int signal);
int wcnd_kill_process_by_name(const char *proc_name, int signal);

int wcnd_find_process_by_name(const char *proc_name);

int wcnd_down_network_interface(const char *ifname);
int wcnd_up_network_interface(const char *ifname);

void wcnd_wait_for_supplicant_stopped();
void wcnd_wait_for_driver_unloaded(void);

int wcnd_notify_wifi_driver_cp2_state(int state_ok);

int wcnd_check_process_exist(const char *proc_name, int proc_pid);

int wcnd_find(const char *path, const char *target);

int wcnd_stop_process(const char *proc_name, int time_out);
int wcnd_send_back_cmd_result(int client_fd, char *str, int isOK);

int wcnd_send_selfcmd(WcndManager *pWcndManger, char *cmd);
int send_msg(WcndManager *pWcndManger, int client_fd, char *msg_str);
int wcnd_split_int(char* string, char *ch);
int check_if_wcnmodem_enable(void);

void pre_send_cp2_exception_notify(void);
void dispatch_command(WcndManager *pWcndManger, int client_fd,
                             char *data, int data_len);

#endif  // WCND_UTIL_H_
