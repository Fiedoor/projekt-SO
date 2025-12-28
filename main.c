#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <errno.h>
#include <signal.h>

// === KONFIGURACJA ===
#define NAZWA_FIFO "kolejka_fifo"
#define ROZMIAR_BUFORA 256
// Unikalne klucze dla zasobów systemowych
#define KLUCZ_SHM 12345
#define KLUCZ_SEM 67890

// === ZMIENNE GLOBALNE (dla obsługi sygnałów) ===
int g_shm_id = -1;
int g_sem_id = -1;
int g_fifo_fd = -1;

// Struktura przekazywana przez Pamięć Współdzieloną
struct WspolnaPamiec {
    char tekst[ROZMIAR_BUFORA];
    int ilosc_bajtow; // P2 to oblicza, P1 wyświetla
    int czy_koniec;   // Flaga zakończenia transmisji
};

// === OBSŁUGA SYGNAŁÓW (CLEANUP) ===
// Ta funkcja uruchomi się, gdy wciśniesz Ctrl+C
void obsluga_sygnalu(int sig) {
    printf("\n\n[SYSTEM]: Otrzymano Ctrl+C (Sygnał %d). Sprzątam bałagan...\n", sig);

    // 1. Usuwamy Pamięć Współdzieloną
    if (g_shm_id != -1) {
        shmctl(g_shm_id, IPC_RMID, NULL);
        printf("[SYSTEM]: Pamięć SHM usunięta.\n");
    }

    // 2. Usuwamy Semafory
    if (g_sem_id != -1) {
        semctl(g_sem_id, 0, IPC_RMID);
        printf("[SYSTEM]: Semafory usunięte.\n");
    }

    // 3. Usuwamy plik FIFO
    if (g_fifo_fd != -1) close(g_fifo_fd);
    unlink(NAZWA_FIFO);
    printf("[SYSTEM]: Plik FIFO usunięty.\n");

    // 4. Zabijamy całą grupę procesów (P1, P2, P3), żeby nic nie wisiało
    kill(0, SIGKILL);
    exit(0);
}

// === POMOCNICZE FUNKCJE SEMAFORÓW ===
// P (Opuszczenie/Czekanie)
void sem_p(int sem_id, int sem_num) {
    struct sembuf operacja = {sem_num, -1, 0};
    if (semop(sem_id, &operacja, 1) == -1) {
        if (errno != EINTR) perror("Błąd sem_p"); // Ignorujemy przerwania sygnałem
    }
}

// V (Podniesienie/Sygnalizowanie)
void sem_v(int sem_id, int sem_num) {
    struct sembuf operacja = {sem_num, 1, 0};
    if (semop(sem_id, &operacja, 1) == -1) {
        if (errno != EINTR) perror("Błąd sem_v");
    }
}

// ================= MAIN =================
int main(int argc, char *argv[]) {
    // Rejestrujemy funkcję, która posprząta po Ctrl+C
    signal(SIGINT, obsluga_sygnalu);

    // 1. TWORZENIE PAMIĘCI WSPÓŁDZIELONEJ
    g_shm_id = shmget(KLUCZ_SHM, sizeof(struct WspolnaPamiec), 0666 | IPC_CREAT);
    if (g_shm_id == -1) { perror("Błąd shmget"); return 1; }

    // 2. TWORZENIE SEMAFORÓW (2 sztuki)
    // Sem 0: Kontroluje miejsce wolne (dla P2)
    // Sem 1: Kontroluje dane gotowe (dla P1)
    g_sem_id = semget(KLUCZ_SEM, 2, 0666 | IPC_CREAT);
    if (g_sem_id == -1) { perror("Błąd semget"); return 1; }

    // Inicjalizacja:
    semctl(g_sem_id, 0, SETVAL, 1); // Sem 0 = 1 (Jest wolne miejsce na start)
    semctl(g_sem_id, 1, SETVAL, 0); // Sem 1 = 0 (Brak danych na start)

    // 3. TWORZENIE FIFO
    if (mkfifo(NAZWA_FIFO, 0666) == -1) {
        if (errno != EEXIST) perror("Błąd mkfifo");
    }

    pid_t pid = fork();

    // ================= PROCES P3 (NADAWCA) =================
    if (pid == 0) {
        FILE *zrodlo_danych;
        int czy_zamykac_plik = 0;

        if (argc > 1) {
            printf("[P3]: Tryb PLIKOWY: %s\n", argv[1]); fflush(stdout);
            zrodlo_danych = fopen(argv[1], "r");
            if (!zrodlo_danych) { perror("Błąd pliku"); exit(1); }
            czy_zamykac_plik = 1;
        } else {
            printf("[P3]: Tryb INTERAKTYWNY. Pisz (Ctrl+D = koniec).\n"); fflush(stdout);
            zrodlo_danych = stdin;
        }

        // P3 czeka aż P2 otworzy rurę
        int fifo = open(NAZWA_FIFO, O_WRONLY);
        if (fifo == -1) { perror("Błąd open FIFO P3"); exit(1); }

        char bufor[ROZMIAR_BUFORA];
        // Czytamy i wysyłamy
        while (fgets(bufor, sizeof(bufor), zrodlo_danych) != NULL) {
            // Wysyłamy ZAWSZE pełną ramkę 256 bajtów
            // To zapobiega sklejaniu się napisów w FIFO
            write(fifo, bufor, sizeof(bufor));
        }

        if (czy_zamykac_plik) fclose(zrodlo_danych);
        close(fifo);
        printf("[P3]: Koniec danych. Zamykam się.\n");
        exit(0);
    }

    // ================= BLOK RODZICA =================
    else {
        pid_t pid2 = fork();

        if (pid2 == 0) {
            // ================= PROCES P2 (POŚREDNIK) =================
            // Logika: Czyta FIFO -> Liczy bajty -> Pisze do SHM

            struct WspolnaPamiec *wspolna = (struct WspolnaPamiec*) shmat(g_shm_id, NULL, 0);

            printf("[P2]: Czekam na połączenie FIFO...\n"); fflush(stdout);
            g_fifo_fd = open(NAZWA_FIFO, O_RDONLY);

            char bufor_odbiorczy[ROZMIAR_BUFORA];
            int bajty;

            // Pętla odczytu z FIFO
            while ((bajty = read(g_fifo_fd, bufor_odbiorczy, sizeof(bufor_odbiorczy))) > 0) {

                // CZEKAJ NA WOLNE MIEJSCE W PAMIĘCI (Sem 0 w dół)
                sem_p(g_sem_id, 0);

                // --- SEKCJA KRYTYCZNA ---
                // 1. Obliczamy długość (wymóg zadania)
                int dlugosc = strlen(bufor_odbiorczy);
                // (Opcjonalnie usuwamy enter z końca dla ładnego wyglądu)
                if (dlugosc > 0 && bufor_odbiorczy[dlugosc-1] == '\n') dlugosc--;

                // 2. Kopiujemy do pamięci współdzielonej
                strcpy(wspolna->tekst, bufor_odbiorczy);
                wspolna->ilosc_bajtow = dlugosc;
                wspolna->czy_koniec = 0;

                printf("   [P2] Przeliczyłem: %d znaków. Wstawiam do SHM.\n", dlugosc);
                fflush(stdout);

                // SYGNALIZUJ DANE GOTOWE DLA P1 (Sem 1 w górę)
                sem_v(g_sem_id, 1);
            }

            // Obsługa końca transmisji (gdy P3 zamknął rurę)
            sem_p(g_sem_id, 0);       // Czekaj na dostęp
            wspolna->czy_koniec = 1;  // Ustaw flagę końca
            sem_v(g_sem_id, 1);       // Powiadom P1

            printf("[P2]: Koniec pracy. Odłączam SHM i znikam.\n");
            shmdt(wspolna);
            close(g_fifo_fd);
            exit(0);
        }
        else {
            // ================= PROCES P1 (KONSUMENT / GLÓWNY) =================
            // Logika: Czeka na SHM -> Wyświetla wynik

            struct WspolnaPamiec *wspolna = (struct WspolnaPamiec*) shmat(g_shm_id, NULL, 0);

            printf("[P1]: Gotowy. Czekam na dane z Pamięci Współdzielonej...\n");
            fflush(stdout);

            while (1) {
                // CZEKAJ NA DANE (Sem 1 w dół)
                sem_p(g_sem_id, 1);

                // Sprawdzamy, czy P2 zgłosił koniec
                if (wspolna->czy_koniec == 1) {
                    sem_v(g_sem_id, 0); // Zwalniamy semafor (dobra praktyka)
                    break; // Wychodzimy z pętli
                }

                // Wyświetlamy dane
                printf("      ---> \033[32m[P1] Wynik z SHM: %s (Dlugosc: %d)\033[0m",
                       wspolna->tekst, wspolna->ilosc_bajtow);

                // Dokładamy enter jeśli go brakuje
                if (wspolna->tekst[strlen(wspolna->tekst)-1] != '\n') printf("\n");
                fflush(stdout);

                // ZWALNIAMY MIEJSCE DLA P2 (Sem 0 w górę)
                sem_v(g_sem_id, 0);
            }

            printf("\n[P1]: P2 zakończył pracę. Usuwam zasoby IPC.\n");

            // Sprzątanie po normalnym zakończeniu
            shmdt(wspolna);
            shmctl(g_shm_id, IPC_RMID, NULL); // Usuń pamięć
            semctl(g_sem_id, 0, IPC_RMID);    // Usuń semafory
            unlink(NAZWA_FIFO);               // Usuń plik kolejki

            // Czekamy, aż dzieci formalnie zakończą procesy
            wait(NULL);
            wait(NULL);
            printf("[SYSTEM]: Koniec programu. Wszystko czyste.\n");
        }
    }

    return 0;
}