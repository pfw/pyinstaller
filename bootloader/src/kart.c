#include "kart.h"

#define _XOPEN_SOURCE 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sem.h>
#include <errno.h>

#include "cJSON.h"

int semid;

void exit_on_alarm(int sig)
{
    int semval = semctl(semid, 0, GETVAL);
    int exit_code = semval - 1000;
    semctl(semid, 0, IPC_RMID);
    exit(exit_code);
}

int kart_main(int argc, char **argv, char **environ)
{
    int res;
    if (getenv("KART_USE_HELPER"))
    {
        // start or use an existing helper process
        char **env_ptr;
        char *ptr = 0;

        int listSZ;
        for (listSZ = 0; environ[listSZ] != NULL; listSZ++)
            ;
        char **helper_environ = malloc((listSZ - 1) * sizeof(*helper_environ));

        cJSON *env = NULL;
        cJSON *args = NULL;
        size_t index = 0;
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddNumberToObject(payload, "pid", getpid());
        env = cJSON_AddObjectToObject(payload, "environ");
        int found = 0;
        /* while the current string pointed to by *env_variable is not empty, increment it. */
        for (env_ptr = environ; *env_ptr != NULL; env_ptr++)
        {
            int i = 0;
            char temp[4096] = "";
            int envlen;
            if ((envlen = strlen(*env_ptr)) > 4096)
            {
                printf("env var too long\n"); // TODO - need to handle this better
            }
            strcpy(temp, *env_ptr);
            ptr = strtok(temp, "=");

            char key[4096];
            char val[4096];

            while (ptr != NULL)
            {
                if (i == 0) /* in the first iteration we get the left part so we store it */
                    strcpy(key, ptr);

                if (i == 1) /* in the second iteration we get the right part so we store it */
                    strcpy(val, ptr);

                ptr = strtok(NULL, "="); /* set 'ptr' to point to the right part,
                                            if this is already the second iteration,
                                            it will point to NULL */
                i++;
            }
            if (strcmp(key, "KART_USE_HELPER"))
            {
                helper_environ[found++] = *env_ptr;
                cJSON_AddStringToObject(env, key, val);
            }
        }
        char **arg_ptr;
        args = cJSON_AddArrayToObject(payload, "argv");
        for (arg_ptr = argv; *arg_ptr != NULL; arg_ptr++)
        {
            cJSON_AddItemToArray(args, cJSON_CreateString(*arg_ptr));
        }

        int fp = open(getcwd(NULL, 0), O_RDONLY);
        int NUM_FD = 4;
        int fds[4] = {fileno(stdin), fileno(stdout), fileno(stderr), fp};

        char *socket_filename = malloc(strlen(getenv("HOME")) + strlen(".kart.socket") + 2);
        sprintf(socket_filename, "%s/%s", getenv("HOME"), ".kart.socket");
        int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);

        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, socket_filename);

        if (connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            // start helper in background and wait
            char *cmd = argv[0];

            char **helper_argv = (char **)malloc((4 + 1) * sizeof(char *));
            helper_argv[0] = cmd;
            helper_argv[1] = "helper";
            helper_argv[2] = "--socket";
            helper_argv[3] = socket_filename;
            helper_argv[4] = 0;

            pid_t pid;
            int status;

            status = posix_spawnp(&pid, cmd, NULL, NULL, helper_argv, helper_environ);
            if (status < 0)
            {
                printf("Error running kart helper: %s", strerror(status));
                exit(1);
            }

            int rtc, max_retry = 10;
            while ((rtc = connect(socket_fd, (struct sockaddr *)&addr, sizeof addr)) != 0 && --max_retry >= 0)
            {
                usleep(250000);
            }
            if (rtc < 0)
            {
                printf("Timeout connecting to kart helper\n");
                return 2;
            }
        }

        // set up exit code semaphore
        if ((semid = semget(IPC_PRIVATE, 1, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR)) < 0)
        {
            printf("Error setting up result communication with helper %s\n", strerror(errno));
            return 5;
        };

        // set the 'unused' semval
        // union semun semopts;
        // semopts.val = -1;
        // semctl(semid, 0, SETVAL, semopts);

        cJSON_AddNumberToObject(payload, "semid", semid);
        char *payload_string = cJSON_Print(payload);

        struct iovec iov = {
            .iov_base = payload_string,
            .iov_len = strlen(payload_string)};

        union
        {
            char buf[CMSG_SPACE(sizeof(fds))];
            struct cmsghdr align;
        } u;

        struct msghdr msg = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = u.buf,
            .msg_controllen = sizeof(u.buf)};

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

        *cmsg = (struct cmsghdr){
            .cmsg_level = SOL_SOCKET,
            .cmsg_type = SCM_RIGHTS,
            .cmsg_len = CMSG_LEN(sizeof(fds))};

        memcpy((int *)CMSG_DATA(cmsg), fds, sizeof(fds));
        msg.msg_controllen = cmsg->cmsg_len;

        signal(SIGALRM, exit_on_alarm);

        if (sendmsg(socket_fd, &msg, 0) < 0)
        {
            printf("Error sending command to kart helper %s\n", strerror(errno));
            return 3;
        };

        // loop until the semaphore value has changed.
        // this seems more sensible but I think it means this
        // process will close before the helper has flushed the file descriptors
        // which does funky things, eg. output after the shell shows
        // this process as finished
        // int semval;
        // for (int i = 0; i < 1000; i += 10) {
        //     semval = semctl(semid, 0, GETVAL);
        //     if (semval != -1) {
        //         int exit_code = semval - 1000;
        //         semctl(semid, 0, IPC_RMID);
        //         exit(exit_code);
        //     } else {
        //         usleep(i * 1000);
        //         printf("sleep for %d useconds\n", i*1000);
        //     }
        // }

        sleep(3600); // this should be as long as the longest command, clone etc, how long?
        printf("Timed out, no response from kart helper\n");
        return 4;
    }
    else
    {
        // run the full application as normal
        res = -9999;
    }
    return res;
}