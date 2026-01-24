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
#define KLUCZ_SHM 12345 //To jak numer szafki na basenie. - WSPÓLNA PAMIEC DLA KILKU ROCESÓW
#define KLUCZ_SEM 67890 //To identyfikator zestawu "świateł drogowych" - SEMAFORY - STEROWANIE PROCESAMI
#define KLUCZ_MSG 54321 //To identyfikator skrzynki pocztowej - SYGNAŁY - KONTROLA CO SIE DZIEJE PRZEKAZ INFORMACJI

struct WspolnaPamiec { //(Biurko P2 i P1)
    char tekst[ROZMIAR_BUFORA]; //P2 wpisuje tu to, co odebrał z FIFO.
    int ilosc_bajtow; //To miejsce na wynik pracy procesu P2. P2 liczy znaki (strlen) i wpisuje liczbę tutaj. P1 stąd ją odczytuje.
    int czy_koniec; //To tzw. "flaga". Działa jak włącznik światła.
};

struct Komunikat {//koperta dla komunikatow
    long mtype; // <--- TO JEST KOPERTA (wymagane przez system)
    int wartosc_sygnalu; // <--- TO JEST LIST (Twoje dane)
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
    // W Twoim projekcie latają sygnały SIGUSR1. One przerywają czekanie na semaforze.
    // Jeśli to nie był sygnał (tylko np. błąd pamięci), to przerywamy pętlę (break).
}

void sem_v(int s_id, int s_num) {
    // 1: operacja dodawania     (oddaję zasób, zwiększam licznik)
    struct sembuf op = {s_num, 1, 0};
    // To samo zabezpieczenie przed przerwaniem przez sygnał
    while (semop(s_id, &op, 1) == -1) if (errno != EINTR) break;
}

// --- ZAAWANSOWANA OBSŁUGA SYGNAŁÓW ---
void send_status(int target_pid, int signal_val) {
    struct Komunikat m = {1, signal_val};
    msgsnd(msg_id, &m, sizeof(int), 0); //"Weź dane spod adresu &m, zmierz tylko treść (int), i wrzuć to do kolejki o numerze msg_id, a jak nie ma miejsca, to czekaj (0)."
    kill(target_pid, SIGUSR1); //drze mordę do drugiego procesu: "E! Wstawaj i bierz to, co ci zostawiłem!".
}

void universal_handler(int sig, siginfo_t *info, void *context) {
    struct Komunikat msg;
    if (sig == SIGUSR1) {
        // Każdy proces odbiera komunikat, przetwarza i podaje dalej wg maila [cite: 6, 7, 8]
        if (msgrcv(msg_id, &msg, sizeof(int), 1, IPC_NOWAIT) != -1) {
            printf("[Proces %d] Odebrano info o sygnale %d od PID %d\n", getpid(), msg.wartosc_sygnalu, info->si_pid);

            if (getpid() == p3) send_status(p2, msg.wartosc_sygnalu); // program sprawdza do którego procesu ma wysąłc wiadomosc
            else if (getpid() == p2) send_status(p1, msg.wartosc_sygnalu);
        }
    }
}

void setup_signals() {
    struct sigaction sa;
    sa.sa_sigaction = universal_handler;
    sa.sa_flags = SA_SIGINFO; // Weryfikacja nadawcy
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}
// TO JEST UKRYTE W BIBLIOTECE <signal.h> */
//struct sigaction {
//  void (*sa_sigaction)(int, siginfo_t *, void *); // <-- To pole wypełniamy
//    sigset_t sa_mask;                               // <-- To pole czyścimy
    //int sa_flags;                                   // <-- Tu wpisujemy SA_SIGINFO
  //  void (*sa_restorer)(void);
//};
// --- PROCESY ---
void wykonaj_p3(const char* zrodlo) {
    // 1. WŁĄCZENIE NASŁUCHU (DOMOFONU)
    // Bez tego P3 byłby głuchy na sygnały od Procesu Macierzystego (PM).
    // Musi to zrobić na samym początku, żeby być gotowym na instrukcje "z góry".
    setup_signals();

    // 2. WYBÓR ŹRÓDŁA DANYCH
    // To jest skrócony "if" (operator trójargumentowy).
    // Jeśli 'zrodlo' (nazwa pliku) istnieje -> otwórz ten plik (fopen).
    // Jeśli 'zrodlo' to NULL -> użyj klawiatury (stdin).
    // Realizuje wymóg: "czyta dane ... ze standardowego strumienia wejściowego lub pliku" [cite: 1]
    FILE *f = (zrodlo) ? fopen(zrodlo, "r") : stdin;

    // 3. PODŁĄCZENIE DO RURY (FIFO)
    // Otwieramy plik kolejki o nazwie "kolejka_fifo".
    // O_WRONLY = Open Write Only (Tylko do zapisu).
    // To jest moment, w którym P3 dzwoni do P2: "Halo, jestem, będę nadawał".
    int fd = open(NAZWA_FIFO, O_WRONLY);

    // 4. PRZYGOTOWANIE PUDEŁKA
    // Rezerwujemy pamięć (np. 256 znaków) na pojedynczą linię tekstu.
    char bufor[ROZMIAR_BUFORA];

    // 5. PĘTLA PRZETWARZANIA
    // fgets czyta plik linijka po linijce, aż dojedzie do końca (NULL).
    // Realizuje wymóg: "czyta dane (pojedyncze wiersze)" [cite: 1]
    while (fgets(bufor, sizeof(bufor), f) != NULL) {

        // 6. WYSYŁKA DO RURY
        // P3 wrzuca to, co przeczytał, prosto do rury (do zmiennej fd).
        // Wysyła całe pudełko (sizeof), żeby P2 zawsze wiedział ile odczytać.
        // Realizuje wymóg: "przekazuje je w niezmienionej formie do procesu 2" [cite: 1]
        write(fd, bufor, sizeof(bufor));

        // (Tutaj w wersji z wyświetlaniem byłby printf i fflush)
    }

    // 7. SPRZĄTANIE I KONIEC
    // Zamykamy wejście do rury. To sygnał dla P2: "Koniec transmisji, nic więcej nie będzie".
    // Gdybyś tego nie zamknął, P2 wisiałby i czekał w nieskończoność.
    close(fd);

    // Zabijamy proces P3, bo skończył robotę.
    exit(0);
}

void wykonaj_p2() {
    setup_signals();
    struct WspolnaPamiec *shm = (struct WspolnaPamiec*)shmat(shm_id, NULL, 0);
    int fd = open(NAZWA_FIFO, O_RDONLY);
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
    exit(0);
}

void wykonaj_p1() {
    setup_signals(); // Obsługa sygnałów (wymóg projektu)

    // Podłączenie pamięci
    struct WspolnaPamiec *shm = (struct WspolnaPamiec*)shmat(shm_id, NULL, 0);

    while (1) {
        // Czekamy na dane od P2 (Semafor 1)
        sem_p(sem_id, 1);

        // Sprawdzamy flagę końca
        if (shm->czy_koniec) break;

        // --- ZMIANA: Wyświetlanie danych ---
        // Usuwamy zbędne "P1 Wyjście" i wyświetlamy sam tekst + obliczenie.
        // shm->tekst to linia z pliku
        // shm->ilosc_bajtow to liczba policzona przez P2

        // Sprawdzamy, czy tekst ma enter na końcu, żeby ładnie wyglądało
        int len = strlen(shm->tekst);
        if (len > 0 && shm->tekst[len-1] == '\n') {
            // Tekst ma już enter, więc wypisujemy go tak:
            printf("%s [Dlugosc wg P2: %d] ", shm->tekst, shm->ilosc_bajtow);
        } else {
            // Tekst nie ma entera, dodajemy go:
            printf("%s\n [Dlugosc wg P2: %d]\n ", shm->tekst, shm->ilosc_bajtow);
        }

        fflush(stdout); // Wypychamy na ekran natychmiast

        // Zwalniamy miejsce dla P2 (Semafor 0)
        sem_v(sem_id, 0);
    }

    shmdt(shm); // Odłączamy pamięć
    exit(0);
}

int main(int argc, char *argv[]) {
    // 1. BUDOWANIE INFRASTRUKTURY (Zasoby IPC)
    // Tworzymy Pamięć Współdzieloną (biurko P2 i P1) [cite: 1]
    shm_id = shmget(KLUCZ_SHM, sizeof(struct WspolnaPamiec), 0666 | IPC_CREAT);

    // Tworzymy 2 Semafory (światła drogowe dla pamięci) [cite: 1]
    sem_id = semget(KLUCZ_SEM, 2, 0666 | IPC_CREAT);

    // Tworzymy Kolejkę Komunikatów (Paczkomat na sygnały) [cite: 4]
    msg_id = msgget(KLUCZ_MSG, 0666 | IPC_CREAT);

    // 2. USTAWIANIE ŚWIATEŁ (Semaforów)
    // Semafor 0 = 1 (ZIELONE): "Pamięć jest pusta, P2 może pisać".
    semctl(sem_id, 0, SETVAL, 1);
    // Semafor 1 = 0 (CZERWONE): "Pamięć jest pusta, P1 musi czekać".
    semctl(sem_id, 1, SETVAL, 0);

    // 3. TWORZENIE RURY (FIFO)
    // Tworzymy plik specjalny "kolejka_fifo" na dysku (kanał P3 -> P2)[cite: 1].
    mkfifo(NAZWA_FIFO, 0666);

    // 4. ZATRUDNIANIE PRACOWNIKÓW (Forkowanie)
    // PM klonuje się 3 razy. Każdy klon staje się innym pracownikiem.
    // Realizuje to wymóg: "Wszystkie trzy procesy powinny być powoływane automatycznie z jednego procesu inicjującego".

    if ((p1 = fork()) == 0) wykonaj_p1(); // Dziecko 1 staje się P1
    if ((p2 = fork()) == 0) wykonaj_p2(); // Dziecko 2 staje się P2

    // Dziecko 3 staje się P3 i dostaje nazwę pliku z argumentów (argv[1])
    if ((p3 = fork()) == 0) wykonaj_p3(argc > 1 ? argv[1] : NULL);

    // 5. INICJACJA SCENARIUSZA POWIADAMIANIA
    // Dajemy chwilę (1s), żeby pracownicy zdążyli włączyć nasłuch (setup_signals).
    sleep(1);

    // TO JEST KLUCZOWE: PM wysyła pierwszy sygnał do P3.
    // Realizuje to dokładnie instrukcję z maila: "Proces macierzysty zapisuje wartość sygnału... oraz wysyła powiadomienie do procesu 3"[cite: 6].
    // Bez tego łańcuch sygnałów (PM -> P3 -> P2 -> P1) by nie ruszył.
    send_status(p3, SIGUSR1);

    // 6. NADZÓR (Czekanie na fajrant)
    // PM czeka, aż wszystkie 3 procesy zakończą pracę (np. po skończeniu pliku).
    for(int i=0; i<3; i++) wait(NULL);

    // 7. SPRZĄTANIE PO IMPREZIE
    // Usuwamy biurko (SHM), światła (SEM) i paczkomat (MSG) z systemu.
    shmctl(shm_id, IPC_RMID, NULL);
    semctl(sem_id, 0, IPC_RMID);
    msgctl(msg_id, IPC_RMID, NULL);

    // Usuwamy plik rury z dysku.
    unlink(NAZWA_FIFO);

    return 0;
}