/*
 * dijagnostika_tastera.c
 *
 * Pomocni program za pronalazenje TACNOG wiringPi broja pina
 * na koji je spojen "treci" taster (KEY3/KEY0) na DVK512 plocici.
 *
 * Posto 'gpio readall' i 'pinctrl' ne radi pouzdano na ovoj
 * Raspberry Pi Zero 2 W kombinaciji, ovaj program direktno preko
 * wiringPi biblioteke ocitava vise kandidata pinova u petlji i
 * ispisuje SVAKU promjenu stanja, sa brojem pina.
 *
 * KAKO KORISTITI:
 *   1. Kompajliraj:
 *        gcc dijagnostika_tastera.c -o dijagnostika -lwiringPi
 *   2. Pokreni sa sudo:
 *        sudo ./dijagnostika
 *   3. Pritiskaj REDOM: KEY0/KEY3, KEY1, KEY2 - jedan po jedan,
 *      sacekaj 1-2 sekunde izmedju pritisaka.
 *   4. Program ce ispisati nesto poput:
 *        >>> PROMJENA: pin 25 sa HIGH na LOW
 *      Taj broj (25) je TACAN wiringPi broj tog tastera.
 *   5. Zaustavi program sa CTRL+C kad nadjes sve pinove koji te
 *      interesuju.
 *
 * Lista kandidata ispod pokriva najcesce wiringPi brojeve koji se
 * koriste za tastere na DVK512-slicnim plocicama, plus standardne
 * GPIO header pinove. Slobodno dodaj/ukloni brojeve iz niza.
 */

#include <stdio.h>
#include <wiringPi.h>

/* Lista wiringPi pinova koje pratimo. Dodaj ili ukloni po potrebi. */
static const int kandidati[] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    10, 11, 12, 13, 14,
    21, 22, 23, 24, 25, 26, 27, 28, 29
};
static const int broj_kandidata = sizeof(kandidati) / sizeof(kandidati[0]);

static int prethodno[64];

int main(void)
{
    int i;

    if (wiringPiSetup() < 0) {
        fprintf(stderr, "Greska: wiringPiSetup() nije uspio!\n");
        return 1;
    }

    printf("Pokrecem dijagnostiku. Pratim %d pinova.\n", broj_kandidata);
    printf("Podesavam svaki pin kao INPUT sa PUD_UP...\n");

    for (i = 0; i < broj_kandidata; i++) {
        int pin = kandidati[i];
        pinMode(pin, INPUT);
        pullUpDnControl(pin, PUD_UP);
    }

    delay(300); /* stabilizacija */

    for (i = 0; i < broj_kandidata; i++) {
        prethodno[i] = digitalRead(kandidati[i]);
    }

    printf("\nSpreman! Pritiskaj tastere JEDAN PO JEDAN.\n");
    printf("Svaka promjena stanja bice ispisana ispod.\n");
    printf("Zaustavi sa CTRL+C kada zavrsis.\n\n");

    while (1) {
        for (i = 0; i < broj_kandidata; i++) {
            int pin = kandidati[i];
            int trenutno = digitalRead(pin);

            if (trenutno != prethodno[i]) {
                printf(">>> PROMJENA: pin %d sa %s na %s\n",
                       pin,
                       prethodno[i] == HIGH ? "HIGH" : "LOW",
                       trenutno   == HIGH ? "HIGH" : "LOW");
                prethodno[i] = trenutno;
            }
        }
        delay(20);
    }

    return 0;
}
