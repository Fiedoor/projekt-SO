#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main() {
    // Określamy liczbę linii - 200 000 powinno zająć system na ok. 30s
    // w zależności od prędkości terminala.
    int liczba_linii = 200000; 
    FILE *plik = fopen("dane.txt", "w");

    if (plik == NULL) {
        perror("Błąd tworzenia pliku");
        return 1;
    }

    srand(time(NULL));

    for (int i = 1; i <= liczba_linii; i++) {
        // Generujemy losowe dane, aby P2 miał co liczyć
        fprintf(plik, "Linia_%d_Dane_testowe_wartosc_%x\n", i, rand() % 10000);
    }

    fclose(plik);
    printf("Pomyślnie wygenerowano plik 'dane.txt' (%d linii).\n", liczba_linii);
    return 0;
}