// run either as service (daemon) or send a message to switch a power regime
#define _POSIX_C_SOURCE 200809L

#define PID_PATH "/run/user/1000/dynppswitch.pid"

#define SOCK_PATH "/run/user/1000/dynppswitch.sock" 

#define NUM_TRIES 3

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>



typedef enum : int {
    LP = 0,
    BL = 1,
    HP = 2,
} P_STATES;

static int current_state;

static volatile sig_atomic_t running = 1;

void sighandler(int sig);

int set_p_state(void);

void j_message(char j_msg);

void j_service(void);

int lock_or_exit(void);

void setservice(int lock_fd);

void write_pid(int lock_fd, pid_t process_id);

int main(int argc, char* argv[]) {

    int status = 1;

    if (argc != 2) {
        
        fprintf(stderr, "Wrong arguments, use --switch or --service or --quit as args!");
        return 1;

    }

    if (strcmp(argv[1], "--switch") == 0) {
        
        j_message('s');

    } else if (strcmp(argv[1], "--service") == 0) {

        j_service();

    } else if (strcmp(argv[1], "--quit") == 0) {
        
        j_message('q');

    } else {
        fprintf(stderr, "Wrong arguments, use --switch or --service or --quit as args!");
        return 1;
    }

    return 0;
}


void j_service(void) {

    current_state = BL;
    int result = set_p_state();

    if (result == -1) {
        fprintf(stderr, "could not set an initial state");
        exit(3);
    }
    
    int lock = lock_or_exit();

    setservice(lock);
}



int lock_or_exit(void) {

    int fd;

    unsigned int num_attempts = NUM_TRIES;

    do {

        fd = open(PID_PATH, O_CREAT | O_RDWR, 0644);
        
        if (errno == EINTR) continue;
        
        num_attempts--;
        usleep(1000);

    } while(fd < 0 && num_attempts > 0);

    if (fd < 0) {
        
        perror("could not open a lock file");
        exit(1);

    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {

        if (errno == EWOULDBLOCK) {

            fprintf(stderr, "Service already running");
            close(fd);
            exit(2);
        
        }

        perror("flock");
        close(fd);
        exit(1);
    
    }

    return fd;
}

void setservice(int lock_fd) {

    // --- subprocess setup

    pid_t pid = fork();
    
    if (pid < 0) {
        
        perror("failed to fork");
        exit(1);
    
    }

    if (pid > 0) {
        
        close(lock_fd);
        exit(0);

    }

    setsid();
    umask(0);

    if (chdir("/") < 0) {

        perror("service chdir error");
        exit(1);
    
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int devnull = open("/dev/null", O_RDWR);

    if (devnull >= 0) {
        
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        
        if (devnull > STDERR_FILENO) close(devnull);

    }

    write_pid(lock_fd, getpid());

    // --- socket setup

    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);

    int sock_fd;

    unsigned int sock_tries = NUM_TRIES;

    do {

        sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (errno == EINTR) continue;
        sock_tries--;

    } while (sock_fd < 0 && sock_tries > 0);
    if (sock_fd < 0) {

        perror("socket err");
        exit(1);
    
    }

    unlink(SOCK_PATH);
    umask(0117);

    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind err");
        close(sock_fd);
        exit(1);
    }
    umask(0);

    if (listen(sock_fd, 5) < 0) {
        perror("listen err");
        close(sock_fd);
        unlink(SOCK_PATH);
        exit(1);
    }

    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = sighandler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    while (running) {
        
        int client = accept(sock_fd, NULL, NULL);
        
        if (client < 0) {
            if (errno == EINTR && !running) break;
            continue;
        }

        char cmd; // client command to change
        
        ssize_t n = read(client, &cmd, 1);

        if (cmd == 'q') {
        
            close(client);
            break;
        
        }
        if (n > 0) {

            current_state = (current_state + 1) % 3; // restricts state in a loop from 0 to 2
            set_p_state();

            const char* names[] = {"LP", "BL", "HP"};
            const char* reply = names[current_state];

            write(client, reply, (strlen(reply)));
        
        }

        close(client);

    }

    // --- cleanup

    close(sock_fd);
    unlink(SOCK_PATH);
    close(lock_fd);
    unlink(PID_PATH);


    exit(0);
}

void write_pid(int fd, pid_t process_id) {

    if (ftruncate(fd, 0) < 0) {

        perror("ftruncate error");
        exit(1);

    }

    lseek(fd, 0, SEEK_SET);

    char insbuf[32];

    unsigned int wrote = 0;

    unsigned int tries = NUM_TRIES;

    int len = snprintf(insbuf, sizeof(insbuf), "%ld\n", (long)process_id);

    do {

        wrote += write(fd, insbuf, len);
        tries--;
    
    } while (wrote < len && tries > 0);

    if (wrote < len) {
        perror("write pid err");
        exit(1);
    }

}

void sighandler(int sig) {
    (void)signal;
    running = 0;
}

void j_message(char j_msg) {
    
    // --- preparing the socket

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);

    unsigned int sock_tries = NUM_TRIES;

    int sock;

    do {

        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (errno == EINTR) continue;
        sock_tries--;

    } while (sock < 0 && sock_tries > 0);

    if (sock < 0) {

        perror("message socket err");
        exit(1);
    
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        
        perror("message connect error");
        close(sock);
        exit(1);

    }

    if (write(sock, &j_msg, 1) != 1) {

        perror("message write error");
        close(sock);
        exit(1);

    }

    char resp[16];

    ssize_t n = read(sock, resp, sizeof(resp) - 1);

    if (n > 0) {

        resp[n] = '\0';
        fprintf(stdout, "Power regime changed to: %s\n", resp);
    
    }

    close(sock);

    exit(0);

}



int set_p_state(void) {

    if (current_state < 0 || current_state > 2) return -1;

    pid_t pid = fork();

    if (pid == 0) {

        int devnull = open("/dev/null", O_WRONLY);

        close(STDIN_FILENO);
        close(STDERR_FILENO);
        close(STDOUT_FILENO);

        if (devnull < 0) {
            _exit(1);
        } 

        dup2(devnull, STDERR_FILENO);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);

        close(devnull);
        switch (current_state) {
            case LP:
                execlp("/usr/bin/powerprofilesctl", "powerprofilesctl", "set", "power-saver");
            case BL:
                execlp("/usr/bin/powerprofilesctl", "powerprofilesctl", "set", "balanced");
            case HP:
                execlp("/usr/bin/powerprofilesctl", "powerprofilesctl", "set", "performance");
            default:
                _exit(1);    
        }
        _exit(1);

    } else if (pid > 0) {

        wait(NULL);

    }

    return 0;
}