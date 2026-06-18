/*
 * Termometar sa alarmom na LCD-u
 * 
 * Kompajlirati sa:
 *   gcc termometar.c -o termometar -lwiringPi -lwiringPiDev -lpthread
 * 
 * Pokrenuti sa:
 *   sudo ./termometar
 * 
 * NAPOMENA: Prije pokretanja provjeriti ID senzora komandom:
 *   ls /sys/bus/w1/devices/
 * i upisati ga u define SENSOR_ID ispod (bez "28-00000")
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wiringPi.h>
#include <lcd.h>

/* ---- Podesiti ID senzora ---- */
#define SENSOR_ID       "xxxxxxx"   /* <-- upisati ID senzora ovdje */

/* ---- Pinovi za LCD (wiringPi oznake, 4-bitni mod kao na DVK512) ---- */
#define LCD_RS          3
#define LCD_EN          14
#define LCD_D4          4
#define LCD_D5          12
#define LCD_D6          13
#define LCD_D7          6

/* ---- LED pin (wiringPi oznaka, LED3 na DVK512 = GPIO.28 = wiringPi 28) ---- */
#define LED_PIN         28

/* ---- Podrazumijevani prag alarma u stepenima Celzijusa ---- */
#define DEFAULT_THRESHOLD  30.0

/* ---- Globalne promjenljive dijeljene izmedju niti ---- */
static double g_temperatura   = 0.0;   /* trenutna temperatura        */
static double g_temp_min      = 9999.0;/* minimalna izmjerena temp.   */
static double g_temp_max      = -9999.0;/* maksimalna izmjerena temp. */
static double g_prag          = DEFAULT_THRESHOLD; /* prag alarma     */
static int    g_alarm_aktivan = 0;     /* 1 ako je alarm ukljucen     */
static int    g_kraj          = 0;     /* 1 kada treba zavrsiti program */

/* ------------------------------------------------------------------ */
/*  Citanje temperature sa DS18B20 senzora                            */
/* ------------------------------------------------------------------ */
double ocitaj_temperaturu(void)
{
    char putanja[128];
    char buffer[100];
    char *tmp;
    FILE *ft;
    long vrijednost;

    snprintf(putanja, sizeof(putanja),
             "/sys/bus/w1/devices/28-00000%s/w1_slave", SENSOR_ID);

    ft = fopen(putanja, "r");
    if (ft == NULL) {
        fprintf(stderr, "Greska: ne mogu otvoriti senzor na putanji %s\n", putanja);
        return -999.0;
    }

    /* Preskoci prvu liniju, uzmi drugu */
    fgets(buffer, sizeof(buffer), ft); /* prva linija (CRC) */
    fgets(buffer, sizeof(buffer), ft); /* druga linija (temperatura) */
    fclose(ft);

    tmp = strstr(buffer, "t=");
    if (tmp == NULL) {
        fprintf(stderr, "Greska: nije pronadjen podatak o temperaturi\n");
        return -999.0;
    }

    tmp += 2; /* preskoci "t=" */
    vrijednost = atol(tmp);

    return (double)vrijednost / 1000.0;
}

/* ------------------------------------------------------------------ */
/*  Nit za ocitavanje temperature (svake sekunde)                     */
/* ------------------------------------------------------------------ */
void *nit_temperatura(void *arg)
{
    double t;

    while (!g_kraj) {
        t = ocitaj_temperaturu();

        if (t > -999.0) {
            g_temperatura = t;

            if (t < g_temp_min) g_temp_min = t;
            if (t > g_temp_max) g_temp_max = t;

            g_alarm_aktivan = (t >= g_prag) ? 1 : 0;
        }

        sleep(1);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Nit za LED alarm (trepce kada je alarm aktivan)                   */
/* ------------------------------------------------------------------ */
void *nit_led(void *arg)
{
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    while (!g_kraj) {
        if (g_alarm_aktivan) {
            digitalWrite(LED_PIN, HIGH);
            delay(500);
            digitalWrite(LED_PIN, LOW);
            delay(500);
        } else {
            digitalWrite(LED_PIN, LOW);
            delay(100);
        }
    }

    digitalWrite(LED_PIN, LOW);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Nit za azuriranje LCD ekrana                                      */
/* ------------------------------------------------------------------ */
void *nit_lcd(void *arg)
{
    int lcd_h = *(int *)arg;
    int prikaz = 0; /* 0 = trenutna+prag, 1 = min+max */
    int brojac  = 0;

    while (!g_kraj) {
        lcdClear(lcd_h);

        if (prikaz == 0) {
            /* --- Prikaz 1: trenutna temperatura i prag --- */
            lcdPosition(lcd_h, 0, 0);
            if (g_alarm_aktivan)
                lcdPrintf(lcd_h, "Temp:%.1fC ALARM!", g_temperatura);
            else
                lcdPrintf(lcd_h, "Temp: %.1f C", g_temperatura);

            lcdPosition(lcd_h, 0, 1);
            lcdPrintf(lcd_h, "Prag: %.1f C", g_prag);
        } else {
            /* --- Prikaz 2: minimalna i maksimalna temperatura --- */
            lcdPosition(lcd_h, 0, 0);
            lcdPrintf(lcd_h, "Min: %.1f C", g_temp_min > 9000 ? 0.0 : g_temp_min);

            lcdPosition(lcd_h, 0, 1);
            lcdPrintf(lcd_h, "Max: %.1f C", g_temp_max < -9000 ? 0.0 : g_temp_max);
        }

        /* Mijenjaj prikaz svakih 3 sekunde */
        brojac++;
        if (brojac >= 3) {
            brojac  = 0;
            prikaz  = 1 - prikaz; /* prebaci izmedju 0 i 1 */
        }

        sleep(1);
    }

    lcdClear(lcd_h);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Nit za unos praga iz terminala                                    */
/* ------------------------------------------------------------------ */
void *nit_unos(void *arg)
{
    double novi_prag;
    char linija[64];

    printf("\n");
    printf("==============================================\n");
    printf("  Termometar sa alarmom - unos praga\n");
    printf("==============================================\n");
    printf("  Unesite novi prag (npr. 30.5) i pritisnite Enter.\n");
    printf("  Unesite 'q' za izlaz iz programa.\n");
    printf("==============================================\n\n");

    while (!g_kraj) {
        printf("Trenutni prag: %.1f C > ", g_prag);
        fflush(stdout);

        if (fgets(linija, sizeof(linija), stdin) == NULL)
            break;

        /* Provjeri da li korisnik zeli izaci */
        if (linija[0] == 'q' || linija[0] == 'Q') {
            g_kraj = 1;
            break;
        }

        /* Pokusaj parsiranja broja */
        if (sscanf(linija, "%lf", &novi_prag) == 1) {
            if (novi_prag >= -55.0 && novi_prag <= 125.0) {
                g_prag = novi_prag;
                printf("Prag je postavljen na %.1f C\n\n", g_prag);
            } else {
                printf("Greska: prag mora biti izmedju -55 i 125 C\n\n");
            }
        } else {
            printf("Greska: unesite broj ili 'q' za izlaz\n\n");
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Glavni program                                                     */
/* ------------------------------------------------------------------ */
int main(void)
{
    int     lcd_h;
    pthread_t t_temp, t_led, t_lcd, t_unos;

    /* Inicijalizacija wiringPi */
    if (wiringPiSetup() < 0) {
        fprintf(stderr, "Greska pri inicijalizaciji wiringPi!\n");
        return 1;
    }

    /* Inicijalizacija LCD (4-bitni mod, 2 reda, 16 kolona, pinovi kao na DVK512) */
    lcd_h = lcdInit(2, 16, 4,
                    LCD_RS, LCD_EN,
                    LCD_D4, LCD_D5, LCD_D6, LCD_D7,
                    0, 0, 0, 0);
    if (lcd_h < 0) {
        fprintf(stderr, "Greska pri inicijalizaciji LCD!\n");
        return 1;
    }

    lcdClear(lcd_h);
    lcdPosition(lcd_h, 0, 0);
    lcdPuts(lcd_h, "Termometar");
    lcdPosition(lcd_h, 0, 1);
    lcdPuts(lcd_h, "Pokretanje...");
    sleep(2);

    /* Pokretanje niti */
    pthread_create(&t_temp,  NULL, nit_temperatura, NULL);
    pthread_create(&t_led,   NULL, nit_led,         NULL);
    pthread_create(&t_lcd,   NULL, nit_lcd,         (void *)&lcd_h);
    pthread_create(&t_unos,  NULL, nit_unos,        NULL);

    /* Cekaj da nit za unos zavrsi (korisnik ukuca 'q') */
    pthread_join(t_unos,  NULL);

    /* Signaliziraj ostalim nitima da zavrse */
    g_kraj = 1;

    pthread_join(t_temp, NULL);
    pthread_join(t_led,  NULL);
    pthread_join(t_lcd,  NULL);

    lcdClear(lcd_h);
    printf("Program zavrsен.\n");

    return 0;
}
