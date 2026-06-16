#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t parent_from_c1 = 0;
static volatile sig_atomic_t parent_from_c2 = 0;
static volatile sig_atomic_t child1_turn = 0;
static volatile sig_atomic_t child2_turn = 0;
static volatile sig_atomic_t stop_flag = 0;
static volatile sig_atomic_t child1_exited = 0;
static volatile sig_atomic_t child2_exited = 0;
static volatile sig_atomic_t child1_abnormal = 0;
static volatile sig_atomic_t child2_abnormal = 0;
static volatile sig_atomic_t game_failed = 0;
static volatile sig_atomic_t normal_shutdown = 0;

static pid_t child1_pid = 0;
static pid_t child2_pid = 0;
static int pipefd[2] = {-1, -1};

static int child1_turn_count = 0;
static int child2_turn_count = 0;
static int final_value = 0;

static void on_parent_from_c1(int signo) {
    (void)signo;
    parent_from_c1 = 1;
}

static void on_parent_from_c2(int signo) {
    (void)signo;
    parent_from_c2 = 1;
}

static void on_child1_turn(int signo) {
    (void)signo;
    child1_turn = 1;
}

static void on_child2_turn(int signo) {
    (void)signo;
    child2_turn = 1;
}

static void on_stop(int signo) {
    (void)signo;
    stop_flag = 1;
}

static void on_child_exit(int signo) {
    (void)signo;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int abnormal = 0;

        if (!normal_shutdown) {
            if (WIFEXITED(status)) {
                if (WEXITSTATUS(status) != EXIT_SUCCESS) {
                    abnormal = 1;
                }
            } else if (WIFSIGNALED(status)) {
                abnormal = 1;
            } else {
                abnormal = 1;
            }
        }

        if (pid == child1_pid) {
            child1_exited = 1;
            child1_abnormal = abnormal;
            if (abnormal) {
                game_failed = 1;
                stop_flag = 1;
            }
        } else if (pid == child2_pid) {
            child2_exited = 1;
            child2_abnormal = abnormal;
            if (abnormal) {
                game_failed = 1;
                stop_flag = 1;
            }
        }
    }
}

static void install_handler(int signo, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(signo, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

static void wait_for_flag(volatile sig_atomic_t *flag) {
    while (!*flag && !stop_flag) {
        pause();
    }
}

static void write_int(int fd, int value) {
    ssize_t n = write(fd, &value, sizeof(value));
    if (n != (ssize_t)sizeof(value)) {
        perror("write");
        exit(EXIT_FAILURE);
    }
}

static int read_int(int fd, int *value) {
    ssize_t n = read(fd, value, sizeof(*value));
    if (n == 0) {
        return 0;
    }
    if (n != (ssize_t)sizeof(*value)) {
        perror("read");
        exit(EXIT_FAILURE);
    }
    return 1;
}

static void child_loop(const char *name, int read_fd, int write_fd, int max_value,
                       volatile sig_atomic_t *turn_flag, pid_t parent_pid, int notify_sig) {
    (void)max_value;
    while (!stop_flag) {
        wait_for_flag(turn_flag);
        if (stop_flag) {
            break;
        }
        *turn_flag = 0;

        int value;
        if (!read_int(read_fd, &value)) {
            break;
        }

        value += 1;
        fprintf(stdout, "[%s] 接收并+1 -> %d\n", name, value);
        fflush(stdout);

        write_int(write_fd, value);
        kill(parent_pid, notify_sig);
    }
}

static void cleanup_resources(void) {
    if (pipefd[0] != -1) {
        close(pipefd[0]);
        pipefd[0] = -1;
    }
    if (pipefd[1] != -1) {
        close(pipefd[1]);
        pipefd[1] = -1;
    }
}

static void terminate_remaining_children(void) {
    sigset_t block_mask;
    sigset_t old_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block_mask, &old_mask);

    normal_shutdown = 1;

    if (child1_pid > 0 && !child1_exited) {
        kill(child1_pid, SIGTERM);
    }
    if (child2_pid > 0 && !child2_exited) {
        kill(child2_pid, SIGTERM);
    }

    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    int status;
    if (child1_pid > 0 && !child1_exited) {
        waitpid(child1_pid, &status, 0);
        child1_exited = 1;
    }
    if (child2_pid > 0 && !child2_exited) {
        waitpid(child2_pid, &status, 0);
        child2_exited = 1;
    }
}

static void print_exit_reason(const char *name, int abnormal) {
    if (abnormal) {
        fprintf(stdout, "[%s] 异常退出\n", name);
    } else {
        fprintf(stdout, "[%s] 正常结束\n", name);
    }
}

int main(void) {
    int max_value;
    fprintf(stdout, "请输入最大常量：");
    fflush(stdout);
    if (scanf("%d", &max_value) != 1) {
        fprintf(stderr, "输入无效，需要整数。\n");
        return EXIT_FAILURE;
    }

    if (pipe(pipefd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    install_handler(SIGUSR1, on_parent_from_c1);
    install_handler(SIGUSR2, on_parent_from_c2);
    install_handler(SIGTERM, on_stop);
    install_handler(SIGCHLD, on_child_exit);

    child1_pid = fork();
    if (child1_pid < 0) {
        perror("fork child1");
        cleanup_resources();
        return EXIT_FAILURE;
    }

    if (child1_pid == 0) {
        install_handler(SIGUSR1, on_child1_turn);
        install_handler(SIGTERM, on_stop);
        signal(SIGCHLD, SIG_DFL);
        kill(getppid(), SIGUSR1);
        child_loop("子进程 1", pipefd[0], pipefd[1], max_value, &child1_turn, getppid(), SIGUSR1);
        close(pipefd[0]);
        close(pipefd[1]);
        _exit(EXIT_SUCCESS);
    }

    child2_pid = fork();
    if (child2_pid < 0) {
        perror("fork child2");
        kill(child1_pid, SIGTERM);
        waitpid(child1_pid, NULL, 0);
        cleanup_resources();
        return EXIT_FAILURE;
    }

    if (child2_pid == 0) {
        install_handler(SIGUSR2, on_child2_turn);
        install_handler(SIGTERM, on_stop);
        signal(SIGCHLD, SIG_DFL);
        kill(getppid(), SIGUSR2);
        child_loop("子进程 2", pipefd[0], pipefd[1], max_value, &child2_turn, getppid(), SIGUSR2);
        close(pipefd[0]);
        close(pipefd[1]);
        _exit(EXIT_SUCCESS);
    }

    wait_for_flag(&parent_from_c1);
    parent_from_c1 = 0;
    wait_for_flag(&parent_from_c2);
    parent_from_c2 = 0;

    int value = 0;
    fprintf(stdout, "[父进程] 初始值 0，向[子进程 1] 开球\n");
    fflush(stdout);
    write_int(pipefd[1], value);
    kill(child1_pid, SIGUSR1);

    int expect_child = 1;
    int running = 1;
    while (running && !stop_flag) {
        if (game_failed) {
            break;
        }

        if (expect_child == 1) {
            wait_for_flag(&parent_from_c1);
            parent_from_c1 = 0;
            child1_turn_count++;
        } else {
            wait_for_flag(&parent_from_c2);
            parent_from_c2 = 0;
            child2_turn_count++;
        }

        if (stop_flag || game_failed) {
            break;
        }

        if (!read_int(pipefd[0], &value)) {
            break;
        }

        if (value > max_value) {
            running = 0;
            break;
        }

        value += 1;
        fprintf(stdout, "[父进程] 接收并+1 -> %d\n", value);
        fflush(stdout);

        if (value > max_value) {
            running = 0;
            break;
        }

        write_int(pipefd[1], value);
        if (expect_child == 1) {
            kill(child2_pid, SIGUSR2);
            expect_child = 2;
        } else {
            kill(child1_pid, SIGUSR1);
            expect_child = 1;
        }
    }

    final_value = value;

    terminate_remaining_children();
    cleanup_resources();

    fprintf(stdout, "\n========== 游戏统计 ==========\n");
    print_exit_reason("子进程 1", child1_abnormal);
    print_exit_reason("子进程 2", child2_abnormal);
    fprintf(stdout, "子进程 1 回合计数：%d\n", child1_turn_count);
    fprintf(stdout, "子进程 2 回合计数：%d\n", child2_turn_count);
    fprintf(stdout, "最终数值：%d\n", final_value);

    if (game_failed) {
        fprintf(stdout, "\n本局失败：子进程异常退出\n");
        fflush(stdout);
        return EXIT_FAILURE;
    }

    fprintf(stdout, "\n数值已超过最大常量，游戏正常结束。\n");
    fflush(stdout);

    return EXIT_SUCCESS;
}
