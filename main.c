#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>

/* --- KONFIGURACJA --- */
#define FIFO_PATH "kolejka_fifo"
#define MAX_LINE 1024

/* Klucze IPC */
#define QUEUE_KEY_CHAR 'Q'
#define SHM_KEY_CHAR   'S'
#define SEM_KEY_CHAR   'X'

/* Typy komunikatów */
#define KY1 1L
#define KY2 2L
#define KY3 3L
#define ACK_MTYPE 4L

/* Semafory */
#define SEM_EMPTY 0  
#define SEM_FULL  1  

/* Znaczniki */
#define BATCH_END_TOKEN "__BATCH_END__"
#define EXIT_TOKEN "__EXIT__"

/* --- STRUKTURY DANYCH --- */

typedef struct {
    char buffer[MAX_LINE];
} SharedData;

typedef struct {
    long mtype;
    int sig;
} ctrl_msg_t;

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* --- ZMIENNE GLOBALNE --- */

int msq_id = -1;
int shm_id = -1;
int sem_id = -1;
SharedData *shm_ptr = NULL;

pid_t pid_p1 = -1;
pid_t pid_p2 = -1;
pid_t pid_p3 = -1;

/* Flagi sterujące */
volatile sig_atomic_t parent_got_sig = 0;
volatile sig_atomic_t parent_last_sig = 0;
volatile sig_atomic_t parent_sender_pid = 0;

volatile sig_atomic_t p1_paused = 0, p1_term = 0;
volatile sig_atomic_t p2_paused = 0, p2_term = 0;
volatile sig_atomic_t p3_paused = 0, p3_term = 0;

/* --- DEKLARACJE --- */
void die(const char *msg);
void notify(pid_t pid);
void setup_signal_mask(void);
void wait_if_paused(volatile sig_atomic_t *paused, volatile sig_atomic_t *term, const char *who);
void sem_wait_safe(int sem_idx, volatile sig_atomic_t *paused, volatile sig_atomic_t *term, const char *who);
void sem_post_safe(int sem_idx);
int safe_msgsnd(int q, const void *msgp, size_t msgsz, int msgflg);
void drain_ack_queue(void);
void chomp(char *s);
ssize_t safe_write_interruptible(int fd, const void *buf, size_t count, volatile sig_atomic_t *paused, volatile sig_atomic_t *term, const char *who);
bool safe_fgets_interruptible(FILE *f, char *buf, size_t sz, volatile sig_atomic_t *paused, volatile sig_atomic_t *term);

/* --- IMPLEMENTACJA --- */

void die(const char *msg) {
    perror(msg);
    exit(1);
}

void notify(pid_t pid) {
    if (pid > 0) kill(pid, SIGUSR1);
}

void chomp(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') s[len - 1] = '\0';
}

void setup_signal_mask(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);  
    sigaddset(&mask, SIGQUIT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
}

void apply_control_sig(const char *who, int sig, volatile sig_atomic_t *paused, volatile sig_atomic_t *term) {
    if (sig == SIGTSTP) {
        *paused = 1;
    } else if (sig == SIGCONT) {
        *paused = 0;
    } else if (sig == SIGTERM) {
        *term = 1;
        *paused = 0; 
    }
}

/* --- HANDLERY --- */

void parent_sig_handler(int sig, siginfo_t *info, void *u) {
    (void)u;
    if (info) parent_sender_pid = info->si_pid;
    parent_last_sig = sig;
    parent_got_sig = 1;
}

void p1_notify_handler(int sig) {
    if (sig == SIGUSR1) {
        ctrl_msg_t msg;
        while (msgrcv(msq_id, &msg, sizeof(int), KY1, IPC_NOWAIT) != -1) {
            apply_control_sig("P1", msg.sig, &p1_paused, &p1_term);
        }
    }
}

void p2_notify_handler(int sig) {
    if (sig == SIGUSR1) {
        ctrl_msg_t msg;
        while (msgrcv(msq_id, &msg, sizeof(int), KY2, IPC_NOWAIT) != -1) {
            apply_control_sig("P2", msg.sig, &p2_paused, &p2_term);
            notify(pid_p1);
        }
    }
}

void p3_notify_handler(int sig) {
    if (sig == SIGUSR1) {
        ctrl_msg_t msg;
        while (msgrcv(msq_id, &msg, sizeof(int), KY3, IPC_NOWAIT) != -1) {
            apply_control_sig("P3", msg.sig, &p3_paused, &p3_term);
            notify(pid_p2);
        }
    }
}

void p2_external_sig_handler(int sig) { //jak p2 dostanie sygnał od procesu 
    pid_t pp = getppid();
    if (pp > 1) kill(pp, sig);
}

/* --- PAUZA (sigsuspend) --- */
void wait_if_paused(volatile sig_atomic_t *paused, volatile sig_atomic_t *term, const char *who) {
    if (*paused && !*term) {
        const char *msg = " [PAUZA]\n";
        write(STDERR_FILENO, msg, strlen(msg));

        sigset_t mask, oldmask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        sigprocmask(SIG_BLOCK, &mask, &oldmask);

        while (*paused && !*term) {
            sigsuspend(&oldmask);
        }
        sigprocmask(SIG_SETMASK, &oldmask, NULL);

        if (!*term) {
            const char *msg2 = " [WZNOWIONO]\n";
            write(STDERR_FILENO, msg2, strlen(msg2));
        }
    }
}

/* --- SEMAFORY --- */
void sem_wait_safe(int sem_idx, volatile sig_atomic_t *paused, volatile sig_atomic_t *term, const char *who) {
    struct sembuf sb;
    sb.sem_num = sem_idx;
    sb.sem_op = -1;
    sb.sem_flg = 0;

    while (1) {
        /* P1 i P2 ignorują 'term' jako sygnał natychmiastowego wyjścia */
        wait_if_paused(paused, term, who);
        
        if (semop(sem_id, &sb, 1) == -1) {
            if (errno == EINTR) continue;
            die("Błąd semop wait");
        }
        break;
    }
}

void sem_post_safe(int sem_idx) {
    struct sembuf sb;
    sb.sem_num = sem_idx;
    sb.sem_op = 1;
    sb.sem_flg = 0;
    while (semop(sem_id, &sb, 1) == -1) {
        if (errno == EINTR) continue;
        die("Błąd semop post");
    }
}

/* --- I/O SAFE --- */
int safe_msgsnd(int q, const void *msgp, size_t msgsz, int msgflg) {
    while (1) {
        if (msgsnd(q, msgp, msgsz, msgflg) == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

void drain_ack_queue(void) {
    ctrl_msg_t msg;
    while (msgrcv(msq_id, &msg, sizeof(int), ACK_MTYPE, IPC_NOWAIT) >= 0);
}

/* Funkcja zapisu - obsługuje flagę 'term' */
ssize_t safe_write_interruptible(int fd, const void *buf, size_t count, volatile sig_atomic_t *paused, volatile sig_atomic_t *term, const char *who) {
    size_t done = 0;
    const char *ptr = (const char *)buf;
    while (done < count) {
        if (*term) return -1;
        wait_if_paused(paused, term, who);
        
        ssize_t n = write(fd, ptr + done, count - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += n;
    }
    return done;
}

bool safe_fgets_interruptible(FILE *f, char *buf, size_t sz, volatile sig_atomic_t *paused, volatile sig_atomic_t *term) {
    while (1) {
        if (*term) return false;
        wait_if_paused(paused, term, "Fgets");
        if (*term) return false;
        
        errno = 0;
        char *res = fgets(buf, (int)sz, f);
        if (res) return true;
        
        if (errno == EINTR) {
            clearerr(f);
            continue;
        }
        if (feof(f)) return false;
        return false;
    }
}

void parent_send_control(int sig) {
    ctrl_msg_t m = { .sig = sig };
    m.mtype = KY3; safe_msgsnd(msq_id, &m, sizeof(int), 0);
    m.mtype = KY2; safe_msgsnd(msq_id, &m, sizeof(int), 0);
    m.mtype = KY1; safe_msgsnd(msq_id, &m, sizeof(int), 0);
    notify(pid_p3);
}

/* --- PROCESY ROBOCZE --- */

void process_p1(void) {
    setup_signal_mask();
    shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) die("shmat P1");

    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = p1_notify_handler;
    sigaction(SIGUSR1, &sa, NULL);

    fprintf(stderr, "[P1] Start.\n");

    /* P1 ignoruje SIGTERM jako powód wyjścia. Czeka na token. */
    volatile sig_atomic_t ignore_term = 0;

    while (1) {
        wait_if_paused(&p1_paused, &p1_term, "P1");
        
        sem_wait_safe(SEM_FULL, &p1_paused, &ignore_term, "P1");

        char local_buf[MAX_LINE];
        strncpy(local_buf, shm_ptr->buffer, MAX_LINE);
        local_buf[MAX_LINE-1] = '\0';

        sem_post_safe(SEM_EMPTY);

        if (strcmp(local_buf, BATCH_END_TOKEN) == 0) {
            ctrl_msg_t ack = { .mtype = ACK_MTYPE, .sig = 0 };
            safe_msgsnd(msq_id, &ack, sizeof(int), 0);
            fprintf(stderr, "[P1] Koniec partii -> ACK\n");
            continue;
        }

        if (strcmp(local_buf, EXIT_TOKEN) == 0) {
            ctrl_msg_t ack = { .mtype = ACK_MTYPE, .sig = 1 };
            safe_msgsnd(msq_id, &ack, sizeof(int), 0);
            fprintf(stderr, "[P1] Otrzymano EXIT token. Koniec.\n");
            break;
        }

        printf("Wynik (P1): %s\n", local_buf);
        fflush(stdout);
    }
    shmdt(shm_ptr);
    exit(0);
}

void process_p2(void) {
    setup_signal_mask();
    shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) die("shmat P2");

    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = p2_external_sig_handler;
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGCONT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_n;
    sigemptyset(&sa_n.sa_mask);
    sa_n.sa_flags = 0;
    sa_n.sa_handler = p2_notify_handler;
    sigaction(SIGUSR1, &sa_n, NULL);

    int fifo = open(FIFO_PATH, O_RDONLY);
    if (fifo == -1) die("open FIFO P2");
    FILE *fin = fdopen(fifo, "r");

    fprintf(stderr, "[P2] Start.\n");

    char buf[MAX_LINE];
    long line_len = 0;

    /* P2 ignoruje SIGTERM jako powód wyjścia. Czeka na token od P3. */
    volatile sig_atomic_t ignore_term = 0;

    while (1) {
        wait_if_paused(&p2_paused, &p2_term, "P2");

        if (!safe_fgets_interruptible(fin, buf, sizeof(buf), &p2_paused, &ignore_term)) {
            /* EOF na FIFO - zazwyczaj oznacza śmierć P3 */
            if (line_len > 0) {
                sem_wait_safe(SEM_EMPTY, &p2_paused, &ignore_term, "P2");
                snprintf(shm_ptr->buffer, MAX_LINE, "%ld", line_len);
                sem_post_safe(SEM_FULL);
            }
            break;
        }

        if (strcmp(buf, BATCH_END_TOKEN "\n") == 0) {
            sem_wait_safe(SEM_EMPTY, &p2_paused, &ignore_term, "P2");
            strcpy(shm_ptr->buffer, BATCH_END_TOKEN);
            sem_post_safe(SEM_FULL);
            line_len = 0;
            continue;
        }

        if (strcmp(buf, EXIT_TOKEN "\n") == 0) {
            sem_wait_safe(SEM_EMPTY, &p2_paused, &ignore_term, "P2");
            strcpy(shm_ptr->buffer, EXIT_TOKEN);
            sem_post_safe(SEM_FULL);
            fprintf(stderr, "[P2] Otrzymano EXIT token. Przekazuję i kończę.\n");
            break;
        }

        size_t part = strlen(buf);
        if (part > 0 && buf[part-1] == '\n') {
            long total = line_len + (part - 1);
            sem_wait_safe(SEM_EMPTY, &p2_paused, &ignore_term, "P2");
            snprintf(shm_ptr->buffer, MAX_LINE, "Dlugosc: %ld Tresc: %s", total,buf);
            sem_post_safe(SEM_FULL);
            line_len = 0;
        } else {
            line_len += part;
        }
    }
    shmdt(shm_ptr);
    fclose(fin);
    exit(0);
}

void process_p3(const char *path) {
    setup_signal_mask();
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = p3_notify_handler;
    sigaction(SIGUSR1, &sa, NULL);

    int fifo = open(FIFO_PATH, O_WRONLY);
    if (fifo == -1) die("open FIFO P3");

    fprintf(stderr, "[P3] Start.\n");

    char buf[MAX_LINE];
    char choice[16];

    while (1) {
        /* Jeśli SIGTERM przyszedł, gdy byliśmy w pętli (ale nie w fgets), wychodzimy */
        if (p3_term) {
            /* Musimy powiadomić P2 o wyjściu, zanim sami wyjdziemy! */
             volatile sig_atomic_t ignore_term_exit = 0;
             fprintf(stderr, "[P3] Wykryto SIGTERM na starcie pętli. Kończę.\n");
             safe_write_interruptible(fifo, EXIT_TOKEN "\n", strlen(EXIT_TOKEN)+1, &p3_paused, &ignore_term_exit, "P3");
             break;
        }

        wait_if_paused(&p3_paused, &p3_term, "P3");

        printf("PID p2 (do wysyłania sygnałów): %d \n\n=== MENU P3 === \n1. Klawiatura\n2. Plik\nWybierz > ",getpid()-1);
        fflush(stdout);

        if (!safe_fgets_interruptible(stdin, choice, sizeof(choice), &p3_paused, &p3_term)) {
            /* Jeśli safe_fgets zwróciło false i jest p3_term, to znaczy, 
               że użytkownik wysłał SIGTERM będąc w menu. */
            if (p3_term) {
                fprintf(stderr, "[P3] SIGTERM w menu -> Wysyłam EXIT token i kończę.\n");
                volatile sig_atomic_t ignore_term_exit = 0;
                
                /* 1. Wysyłam token do P2 */
                safe_write_interruptible(fifo, EXIT_TOKEN "\n", strlen(EXIT_TOKEN)+1, &p3_paused, &ignore_term_exit, "P3");
                
                /* 2. Czekam aż reszta się zamknie (odbieram ACK od P1) */
                ctrl_msg_t ack;
                while (msgrcv(msq_id, &ack, sizeof(int), ACK_MTYPE, 0) < 0);

                break; /* Wyjście z programu */
            }
            continue;
        }
        int op = atoi(choice);

        if (op == 1) {
            printf("Wpisuj (kropka . kończy):\n");
            while (1) {
                if (!safe_fgets_interruptible(stdin, buf, sizeof(buf), &p3_paused, &p3_term)) break;
                if (strcmp(buf, ".\n") == 0) break;
                safe_write_interruptible(fifo, buf, strlen(buf), &p3_paused, &p3_term, "P3");
            }
        } else if (op == 2) {
            printf("Sciezka: "); fflush(stdout);
            if (!safe_fgets_interruptible(stdin, buf, sizeof(buf), &p3_paused, &p3_term)) {
                /* SIGTERM przy podawaniu ścieżki */
                 if (p3_term) {
                    fprintf(stderr, "[P3] SIGTERM przy ścieżce -> Wysyłam EXIT.\n");
                    volatile sig_atomic_t ignore_term_exit = 0;
                    safe_write_interruptible(fifo, EXIT_TOKEN "\n", strlen(EXIT_TOKEN)+1, &p3_paused, &ignore_term_exit, "P3");
                    break;
                 }
                 continue;
            }
            chomp(buf);
            FILE *f = fopen(buf, "r");
            if (f) {
                char fbuf[MAX_LINE];
                /* Maskowanie SIGTERM podczas wysyłania pliku */
                volatile sig_atomic_t ignore_term = 0;

                fprintf(stderr, "[P3] Wysyłam plik... (SIGTERM będzie obsłużony PO zakończeniu)\n");

                while (safe_fgets_interruptible(f, fbuf, sizeof(fbuf), &p3_paused, &ignore_term)) {
                    safe_write_interruptible(fifo, fbuf, strlen(fbuf), &p3_paused, &ignore_term, "P3");
                    if (strlen(fbuf) > 0 && fbuf[strlen(fbuf)-1] != '\n' && feof(f))
                        safe_write_interruptible(fifo, "\n", 1, &p3_paused, &ignore_term, "P3");
                }
                fclose(f);
                if (p3_term) fprintf(stderr, "[P3] Plik wysłany. Wykryto SIGTERM.\n");

            } else perror("Błąd pliku");
        }

        /* Koniec partii - wysyłamy BATCH_END */
        volatile sig_atomic_t ignore_term_ack = 0;
        drain_ack_queue();
        safe_write_interruptible(fifo, BATCH_END_TOKEN "\n", strlen(BATCH_END_TOKEN)+1, &p3_paused, &ignore_term_ack, "P3");

        printf("[P3] Czekam na ACK...\n");
        ctrl_msg_t ack;
        while (msgrcv(msq_id, &ack, sizeof(int), ACK_MTYPE, 0) < 0) {
            if (errno == EINTR) {
                wait_if_paused(&p3_paused, &ignore_term_ack, "P3");
            } else die("msgrcv ACK");
        }
        printf("[P3] Otrzymano ACK.\n");

        /* Sprawdzamy czy był SIGTERM */
        if (p3_term) {
            fprintf(stderr, "[P3] Kończę pracę zgodnie z żądaniem SIGTERM (po partii).\n");
            
            /* Wysyłamy EXIT_TOKEN, żeby zamknąć P2 i P1 */
            safe_write_interruptible(fifo, EXIT_TOKEN "\n", strlen(EXIT_TOKEN)+1, &p3_paused, &ignore_term_ack, "P3");
            
            /* Czekamy na ACK wyjścia od P1 */
            while (msgrcv(msq_id, &ack, sizeof(int), ACK_MTYPE, 0) < 0);
            
            break;
        }
    }
    close(fifo);
    exit(0);
}

/* --- MAIN --- */
int main(int argc, char **argv) {
    const char *input_path = (argc >= 2) ? argv[1] : NULL;

    int logfd = open("logs.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (logfd != -1) {
        dup2(logfd, STDERR_FILENO);
        close(logfd);
    }

    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0666) == -1) die("mkfifo");

    msq_id = msgget(ftok(".", QUEUE_KEY_CHAR), IPC_CREAT | 0666);
    shm_id = shmget(ftok(".", SHM_KEY_CHAR), sizeof(SharedData), IPC_CREAT | 0666);
    sem_id = semget(ftok(".", SEM_KEY_CHAR), 2, IPC_CREAT | 0666);
    
    if (msq_id == -1 || shm_id == -1 || sem_id == -1) die("IPC init");

    union semun arg;
    arg.val = 1; semctl(sem_id, SEM_EMPTY, SETVAL, arg);
    arg.val = 0; semctl(sem_id, SEM_FULL, SETVAL, arg);

    setup_signal_mask();

    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = parent_sig_handler;
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGCONT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    struct sigaction sa_int;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_SIGINFO;
    sa_int.sa_sigaction = parent_sig_handler;
    sigaction(SIGINT, &sa_int, NULL);

    if ((pid_p1 = fork()) == 0) process_p1();
    if ((pid_p2 = fork()) == 0) process_p2();
    if ((pid_p3 = fork()) == 0) process_p3(input_path);

    fprintf(stderr, "[MAIN] Start. P3=%d P2=%d P1=%d.\n", pid_p3, pid_p2, pid_p1);
    fprintf(stderr, "STEROWANIE: kill -SIGTSTP %d (pauza), kill -SIGTERM %d (koniec)\n", pid_p2, pid_p2);

    int active = 3;
    while (active > 0) {
        int st;
        pid_t w = waitpid(-1, &st, WUNTRACED | WCONTINUED);
        
        if (w < 0) {
            if (errno == EINTR) {
                if (parent_got_sig) {
                    int sig = parent_last_sig;
                    parent_got_sig = 0;
                    if (sig == SIGINT) sig = SIGTERM;

                    fprintf(stderr, "[MAIN] Sygnał %d -> wysyłam do dzieci\n", sig);
                    parent_send_control(sig);
                    
                    /* Rodzic tylko wysyła sygnał. 
                       P3 podejmuje decyzję kiedy wysłać EXIT token. */
                }
                continue;
            }
            perror("waitpid");
            break;
        }

        if (WIFSTOPPED(st)) continue;
        if (WIFCONTINUED(st)) continue;

        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            active--;
            fprintf(stderr, "[MAIN] Potomek %d zakończony.\n", w);
        }
    }

    msgctl(msq_id, IPC_RMID, NULL);
    shmctl(shm_id, IPC_RMID, NULL);
    semctl(sem_id, 0, IPC_RMID);
    unlink(FIFO_PATH);
    fprintf(stderr, "[MAIN] Bye.\n");
    return 0;
}
