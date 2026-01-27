#include <stdio.h> //dpowiada za standardowe wejście i wyjście
#include <stdlib.h> //Zawiera funkcje ogólne, takie jak exit() (kończenie procesu) czy malloc (zarządzanie pamięcią).
#include <unistd.h> //zawiera kluczowe funkcje systemowe: fork() (tworzenie P1, P2, P3 ), pipe() (łącza), read/write (obsługa FIFO i PIPE) oraz close
#include <sys/types.h> //(((definiują typy danych używane przez system (np. pid_t) oraz funkcje do sprawdzania statusu plików i tworzenia ich, jak mkfifo.
#include <sys/stat.h> //)))
#include <fcntl.h> //(File Control) Służy do kontrolowania deskryptorów plików. Dzięki niej możesz otwierać pliki i rury w konkretnych trybach, np. O_RDONLY (tylko odczyt) lub O_WRONLY (tylko zapis).
#include <string.h>
#include <sys/wait.h> //Pozwala Procesowi Macierzystemu (PM) czekać na zakończenie pracy swoich dzieci (P1, P2, P3), aby uniknąć "procesów zombie"
//#include <sys/ipc.h> //Zawiera ogólne definicje dla mechanizmów komunikacji międzyprocesowej (Inter-Process Communication). Pozwala generować unikalne klucze dla zasobów.
#include <sys/shm.h> //obsługa Pamięci Współdzielonej (Shared Memory). Umożliwia P2 i P1 dostęp do tego samego obszaru RAM
#include <sys/sem.h> //Obsługa Semaforów. Pozwalają one synchronizować dostęp do pamięci, by P1 nie czytał danych, których P2 jeszcze nie zdążył zapisać.
#include <sys/msg.h> //Obsługa Kolejek Komunikatów. To w nich Proces Macierzysty zapisuje wartości sygnałów, które potem odczytują P3, P2 i P1.
#include <errno.h> //systemowa biblioteka do obsługi błędów. Jeśli np. mkfifo się nie uda, errno powie Ci dokładnie, czy problemem był brak uprawnień, czy to, że plik już istnieje.
#include <signal.h> //Odpowiada za obsługę Sygnałów (np. SIGUSR1, SIGTERM). Pozwala procesom "pukać do siebie nawzajem" i informować o stanie.
//DYREKTYWY
#define NAZWA_FIFO "kolejka_fifo"
#define ROZMIAR_BUFORA 256
#define KLUCZ_SHM 12345 //WSPÓLNA PAMIEC DLA KILKU ROCESÓW
#define KLUCZ_SEM 67890 //SEMAFORY - STEROWANIE PROCESAMI
#define KLUCZ_MSG 54321 //SYGNAŁY - KONTROLA CO SIE DZIEJE PRZEKAZ INFORMACJI

struct WspolnaPamiec { //obszar P2 i P1
    char tekst[ROZMIAR_BUFORA]; //P2 wpisuje tu to, co odebrał z FIFO.
    int ilosc_bajtow; //To miejsce na wynik pracy procesu P2. P2 liczy znaki (strlen) i wpisuje liczbę tutaj. P1 stąd ją odczytuje.
    int czy_koniec; //oznaczenie czy plik został cały odczytan
};

struct Komunikat {//przestrzeń dla komunikatow
    long mtype;
    int wartosc_sygnalu; //Twoje dane
};

int shm_id; //ID Pamięci Współdzielonej
int sem_id; // ID Semaforów
int msg_id; // ID Kolejki Komunikatów
pid_t p1, p2, p3; // Numery procesów (Process ID)

// --- SEMAFORY ---
void sem_p(int s_id, int s_num) {
    // Tworzymy strukturę operacji
    // s_num: numer semafora (0 lub 1)
    // -1:    operacja odejmowania (chcę wziąć zasób)
    // 0:     flagi (standardowe zachowanie)
    struct sembuf op = {s_num, -1, 0};
    while (semop(s_id, &op, 1) == -1) if (errno != EINTR) break;
    // Jeśli semop zwrócił błąd (-1), sprawdzamy dlaczego.
    // EINTR oznacza "Interrupted System Call" - przerwanie przez sygnał.
    // W Twoim projekcie przechodzą sygnały SIGUSR1. One przerywają czekanie na semaforze.
    // Jeśli to nie był sygnał (tylko np. błąd pamięci), to przerywamy pętlę (break).
}

void sem_v(int s_id, int s_num) {
    // 1: operacja dodawania     (oddaję zasób, zwiększam licznik)
    struct sembuf op = {s_num, 1, 0};
    //zabezpieczenie przed przerwaniem przez sygnał
    while (semop(s_id, &op, 1) == -1) if (errno != EINTR) break;
}

// --- OBSŁUGA SYGNAŁÓW ---
void send_status(int target_pid, int signal_val) {
    struct Komunikat m = {1, signal_val};
    msgsnd(msg_id, &m, sizeof(int), 0); //"Weź dane spod adresu &m, zmierz tylko treść (int), i wrzuć to do kolejki o numerze msg_id, a jak nie ma miejsca, to czekaj (0)."
    kill(target_pid, SIGUSR1); //wysłanie sygnału przez kill
}

void universal_handler(int sig, siginfo_t *info, void *context) {
    struct Komunikat msg;
    if (sig == SIGUSR1) {
        // Każdy proces odbiera komunikat, przetwarza i podaje dalej
        if (msgrcv(msg_id, &msg, sizeof(int), 1, IPC_NOWAIT) != -1) {

            if (getpid() == p3) send_status(p2, msg.wartosc_sygnalu); // program sprawdza do którego procesu ma wysłać wiadomosc
            else if (getpid() == p2) send_status(p1, msg.wartosc_sygnalu);
        }
    }
}

void setup_signals() {
    struct sigaction sa;
    sa.sa_sigaction = universal_handler;
    // Dodajemy SA_RESTART. Dzięki temu, gdy przyjdzie sygnał podczas pisania na klawiaturze (fgets),
    // funkcja nie wyrzuci błędu i nie przerwie programu, tylko wznowi czekanie na tekst.
    sa.sa_flags = SA_SIGINFO | SA_RESTART; // Weryfikacja nadawcy
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

// --- PROCESY ---
void wykonaj_p3(const char* zrodlo) {
    // 1. WŁĄCZENIE NASŁUCHU
    // Bez tego P3 byłby głuchy na sygnały od Procesu Macierzystego (PM).
    // Musi to zrobić na samym początku, żeby być gotowym na instrukcje "z góry".
    setup_signals();

    // P3 informuje PM: "Jestem gotowy!". Podnosi semafor nr 2.
    sem_v(sem_id, 2);

    // 2. WYBÓR ŹRÓDŁA DANYCH
    // Jeśli 'zrodlo' (nazwa pliku) istnieje -> otwórz ten plik (fopen).
    // Jeśli 'zrodlo' to NULL -> użyj klawiatury (stdin).
    // Realizuje wymóg: "czyta dane ... ze standardowego strumienia wejściowego lub pliku"
    FILE *f;
    if (zrodlo != NULL) {
        f = fopen(zrodlo, "r");
        if (!f) { perror("Blad otwarcia pliku"); exit(1); }
    } else {
        f = stdin;
        printf("\n--- TRYB INTERAKTYWNY ---\n");
        printf("Jestes w procesie P3. Wpisz tekst i wcisnij ENTER.\n");
        printf("Aby zakonczyc, wcisnij Ctrl+D (EOF).\n");
        printf("-------------------------\n> ");
        fflush(stdout);
    }

    // 3. PODŁĄCZENIE DO FIFO
    // Otwieramy plik kolejki o nazwie "kolejka_fifo".
    // O_WRONLY = Open Write Only
    // Jeśli open() dostanie sygnał, zwróci -1 i errno=EINTR. Musimy próbować do skutku.
    int fd;
    do {
        fd = open(NAZWA_FIFO, O_WRONLY);
    } while (fd == -1 && errno == EINTR);

    if (fd == -1) { perror("Błąd otwarcia FIFO w P3"); exit(1); }

    // 4. PRZYGOTOWANIE BUFORA
    // Rezerwujemy pamięć (256 znaków) na pojedynczą linię tekstu.
    char bufor[ROZMIAR_BUFORA];

    // 5. PĘTLA PRZETWARZANIA
    // fgets czyta plik linijka po linijce, aż dojedzie do końca (NULL).
    // Realizuje wymóg: "czyta dane (pojedyncze wiersze)"
    while (1) {
        if (fgets(bufor, sizeof(bufor), f) == NULL) {
            if (errno == EINTR) {
                clearerr(f); // Sygnał przerwał czytanie, czyścimy błąd i próbujemy znowu
                continue;
            }
            break; // Prawdziwy koniec pliku lub Ctrl+D
        }

        // 6. WYSYŁKA DO RURY
        // P3 wrzuca to, co przeczytał, prosto do pliku fifo (do zmiennej fd).
        // Wysyła cały bufor (sizeof), żeby P2 zawsze wiedział ile odczytać.
        // Realizuje wymóg: "przekazuje je w niezmienionej formie do procesu 2"
        write(fd, bufor, sizeof(bufor));

        // Jeśli tryb interaktywny, wyświetl zachętę ponownie
        if (zrodlo == NULL) {
            printf("> ");
            fflush(stdout);
        }
    }

    // 7. SPRZĄTANIE I KONIEC
    // Zamykamy wejście do rury. To sygnał dla P2: "Koniec transmisji, nic więcej nie będzie".
    // Gdybyś tego nie zamknął, P2 wisiałby i czekał w nieskończoność.
    close(fd);
    if (zrodlo) fclose(f);

    //Kończymy proces P3
    exit(0);
}

void wykonaj_p2() {
    setup_signals();
    struct WspolnaPamiec *shm = (struct WspolnaPamiec*)shmat(shm_id, NULL, 0);

    int fd;
    do {
        fd = open(NAZWA_FIFO, O_RDONLY);
    } while (fd == -1 && errno == EINTR);

    char bufor[ROZMIAR_BUFORA];
    while (read(fd, bufor, sizeof(bufor)) > 0) {
        sem_p(sem_id, 0);
        strncpy(shm->tekst, bufor, ROZMIAR_BUFORA);
        shm->ilosc_bajtow = strlen(bufor);
        shm->czy_koniec = 0;
        sem_v(sem_id, 1);
    }
    sem_p(sem_id, 0);
    shm->czy_koniec = 1;
    sem_v(sem_id, 1);
    shmdt(shm); // Dodane odlaczenie pamięci
    close(fd);  // Dodane zamkniecie kolejki
    exit(0);
}

void wykonaj_p1() {
    setup_signals(); // Obsługa sygnałów

    // Podłączenie pamięci
    struct WspolnaPamiec *shm = (struct WspolnaPamiec*)shmat(shm_id, NULL, 0);

    while (1) {
        // Czekamy na dane od P2 (Semafor 1)
        sem_p(sem_id, 1);

        // Sprawdzamy flagę końca
        if (shm->czy_koniec) break;

        // shm->tekst to linia z pliku
        // shm->ilosc_bajtow to liczba policzona przez P2

        // Sprawdzamy, czy tekst ma enter na końcu, żeby ładnie wyglądało
        int len = strlen(shm->tekst);
        if (len > 0 && shm->tekst[len-1] == '\n') {
            printf("[P1 Otrzymal]: %s [Dlugosc wg P2: %d]\n", shm->tekst, shm->ilosc_bajtow);
        } else {
            // Tekst nie ma entera, dodajemy go:
            printf("[P1 Otrzymal]: %s\n [Dlugosc wg P2: %d]\n", shm->tekst, shm->ilosc_bajtow);
        }

        fflush(stdout); // Wypychamy na ekran natychmiast

        // Zwalniamy miejsce dla P2 (Semafor 0)
        sem_v(sem_id, 0);
    }

    shmdt(shm); // Odłączamy pamięć
    exit(0); // Kończymy p1
}

int main(int argc, char *argv[]) {
    // 1. BUDOWANIE INFRASTRUKTURY (Zasoby IPC)
    // Tworzymy Pamięć Współdzieloną
    shm_id = shmget(KLUCZ_SHM, sizeof(struct WspolnaPamiec), 0666 | IPC_CREAT);

    // Tworzymy 3 Semafory (2 do pamięci współdzielonej i jeden dla p3)
    sem_id = semget(KLUCZ_SEM, 3, 0666 | IPC_CREAT);

    // Tworzymy Kolejkę Komunikatów
    msg_id = msgget(KLUCZ_MSG, 0666 | IPC_CREAT);

    // 2. USTAWIANIE Semaforów
    // Semafor 0 = 1 (ZIELONE): "Pamięć jest pusta, P2 może pisać".
    semctl(sem_id, 0, SETVAL, 1);
    // Semafor 1 = 0 (CZERWONE): "Pamięć jest pusta, P1 musi czekać".
    semctl(sem_id, 1, SETVAL, 0);
    // Semafor 2 = 0 (ZAMKNIĘTY): PM czeka, aż P3 podniesie go po setupie
    semctl(sem_id, 2, SETVAL, 0);

    // 3. TWORZENIE FIFO
    // Tworzymy plik specjalny "kolejka_fifo" na dysku (kanał P3 -> P2).
    mkfifo(NAZWA_FIFO, 0666);

    // Informacja dla uzytkownika o trybie pracy
    if (argc > 1) {
        printf("[PM] Uruchamianie w trybie PLIKOWYM. Zrodlo: %s\n", argv[1]);
    } else {
        printf("[PM] Brak argumentu (pliku). Uruchamianie w trybie INTERAKTYWNYM (stdin).\n");
    }

    // 4. Forkowanie
    // PM klonuje się 3 razy
    // Realizuje to wymóg: "Wszystkie trzy procesy powinny być powoływane automatycznie z jednego procesu inicjującego".

    if ((p1 = fork()) == 0) wykonaj_p1(); // Dziecko 1 staje się P1
    if ((p2 = fork()) == 0) wykonaj_p2(); // Dziecko 2 staje się P2

    // Dziecko 3 staje się P3 i dostaje nazwę pliku z argumentów (argv[1])
    // Jeśli argv[1] istnieje, P3 czyta z pliku. Jeśli nie (NULL), czyta z klawiatury.
    if ((p3 = fork()) == 0) wykonaj_p3(argc > 1 ? argv[1] : NULL);

    // 5. INICJACJA SCENARIUSZA POWIADAMIANIA
    // Czekamy na semafor nr 2 od P3
    sem_p(sem_id, 2);

    // TO JEST KLUCZOWE: PM wysyła pierwszy sygnał do P3.
    // Realizuje to dokładnie instrukcję z maila: "Proces macierzysty zapisuje wartość sygnału... oraz wysyła powiadomienie do procesu 3".
    // Bez tego łańcuch sygnałów (PM -> P3 -> P2 -> P1) by nie ruszył.
    send_status(p3, SIGUSR1);

    // 6. PM czeka, aż wszystkie 3 procesy zakończą pracę (np. po skończeniu pliku).
    for(int i=0; i<3; i++) wait(NULL);

    // 7. SPRZĄTANIE PO IMPREZIE
    // Usuwamy pamięć, semafory i kolejkę wiadomości
    shmctl(shm_id, IPC_RMID, NULL);
    semctl(sem_id, 0, IPC_RMID);
    msgctl(msg_id, IPC_RMID, NULL);

    // Usuwamy plik rury z dysku.
    unlink(NAZWA_FIFO);
    printf("[PM] Koniec pracy systemu.\n");

    return 0;
}