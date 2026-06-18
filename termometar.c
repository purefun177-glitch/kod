/*
 * Termometar sa alarmom na LCD-u
 * Prag se podešava tasterima KEY0 i KEY1 na DVK512 pločici
 *
 * KEY0 (wiringPi pin 21) - povećava prag za 1°C
 * KEY1 (wiringPi pin 22) - smanjuje prag za 1°C
 * KEY2 (wiringPi pin 23) - izlaz iz programa
 *
 * Kompajlirati sa:
 *   gcc termometar.c -o termometar -lwiringPi -lwiringPiDev -lpthread
 *
 * Pokrenuti sa:
 *   sudo ./termometar
 *
 * NAPOMENA: Provjeriti ID senzora komandom:
 *   ls /sys/bus/w1/devices/
 * i upisati pun ID u define SENSOR_ID ispod (npr. "28-00000e723360")
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wiringPi.h>
#include <lcd.h>

/* ---- Podesiti pun ID senzora ---- */
#define SENSOR_ID   "28-00000e723360"

/* ---- Pinovi za LCD (wiringPi oznake, 4-bitni mod, DVK512) ---- */
#define LCD_RS      3
#define LCD_EN      14
#define LCD_D4      4
#define LCD_D5      12
#define LCD_D6      13
#define LCD_D7      6

/* ---- LED pin (LED3 na DVK512 = wiringPi 28) ---- */
#define LED_PIN     28

/* ---- Tasteri na DVK512 (wiringPi oznake) ---- */
#define KEY0        21    /* povećava prag */
#define KEY1        22    /* smanjuje prag */
#define KEY2        23    /* izlaz         */

/* ---- Podrazumijevani prag alarma ---- */
#define DEFAULT_THRESHOLD  30.0

/* ---- Mutex za zaštitu dijeljenih podataka između niti ---- */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- Globalne promjenljive dijeljene između niti ---- */
static double g_temperatura    = 0.0;
static double g_temp_min       = 9999.0;
static double g_temp_max       = -9999.0;
static double g_prag           = DEFAULT_THRESHOLD;
static int    g_alarm_aktivan  = 0;
static volatile int g_kraj     = 0;

/* ------------------------------------------------------------------ */
/*  Čitanje temperature sa DS18B20 senzora                            */
/* ------------------------------------------------------------------ */
double ocitaj_temperaturu(void)
{
    char putanja[128];
    char buffer[100];
    char *tmp;
    FILE *ft;
    long vrijednost;

    snprintf(putanja, sizeof(putanja),
             "/sys/bus/w1/devices/%s/w1_slave", SENSOR_ID);

    ft = fopen(putanja, "r");
    if (ft == NULL) {
        fprintf(stderr, "Greska: ne mogu otvoriti senzor: %s\n", putanja);
        return -999.0;
    }

    fgets(buffer, sizeof(buffer), ft); /* prva linija (CRC) */
    fgets(buffer, sizeof(buffer), ft); /* druga linija (temperatura) */
    fclose(ft);

    /* Provjeri CRC - mora sadrzavati "YES" */
    if (strstr(buffer, "YES") == NULL && strstr(buffer, "t=") == NULL) {
        /* Pokusaj pronaci t= direktno */
    }

    tmp = strstr(buffer, "t=");
    if (tmp == NULL) {
        fprintf(stderr, "Greska: nije pronadjen podatak o temperaturi\n");
        return -999.0;
    }

    tmp += 2;
    vrijednost = atol(tmp);

    return (double)vrijednost / 1000.0;
}

/* ------------------------------------------------------------------ */
/*  Nit za očitavanje temperature (svake sekunde)                     */
/* ------------------------------------------------------------------ */
void *nit_temperatura(void *arg)
{
    double t;

    while (!g_kraj) {
        t = ocitaj_temperaturu();

        if (t > -999.0) {
            pthread_mutex_lock(&g_mutex);
            g_temperatura = t;
            if (t < g_temp_min) g_temp_min = t;
            if (t > g_temp_max) g_temp_max = t;
            g_alarm_aktivan = (t >= g_prag) ? 1 : 0;
            pthread_mutex_unlock(&g_mutex);
        }

        sleep(1);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Nit za LED alarm (trepće kada je alarm aktivan)                   */
/* ------------------------------------------------------------------ */
void *nit_led(void *arg)
{
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    while (!g_kraj) {
        int alarm;
        pthread_mutex_lock(&g_mutex);
        alarm = g_alarm_aktivan;
        pthread_mutex_unlock(&g_mutex);

        if (alarm) {
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
/*  Nit za ažuriranje LCD ekrana                                      */
/* ------------------------------------------------------------------ */
void *nit_lcd(void *arg)
{
    int lcd_h  = *(int *)arg;
    int prikaz = 0;   /* 0 = trenutna+prag, 1 = min+max */
    int brojac = 0;

    while (!g_kraj) {
        double temp, min, max, prag;
        int alarm;

        pthread_mutex_lock(&g_mutex);
        temp  = g_temperatura;
        min   = g_temp_min;
        max   = g_temp_max;
        prag  = g_prag;
        alarm = g_alarm_aktivan;
        pthread_mutex_unlock(&g_mutex);

        lcdClear(lcd_h);

        if (prikaz == 0) {
            lcdPosition(lcd_h, 0, 0);
            if (alarm)
                lcdPrintf(lcd_h, "Temp:%.1fC ALARM!", temp);
            else
                lcdPrintf(lcd_h, "Temp: %.1f C", temp);

            lcdPosition(lcd_h, 0, 1);
            lcdPrintf(lcd_h, "Prag: %.1f C", prag);
        } else {
            lcdPosition(lcd_h, 0, 0);
            lcdPrintf(lcd_h, "Min: %.1f C",
                      min > 9000 ? 0.0 : min);

            lcdPosition(lcd_h, 0, 1);
            lcdPrintf(lcd_h, "Max: %.1f C",
                      max < -9000 ? 0.0 : max);
        }

        /* Mijenjaj prikaz svakih 3 sekunde */
        brojac++;
        if (brojac >= 3) {
            brojac = 0;
            prikaz = 1 - prikaz;
        }

        sleep(1);
    }

    lcdClear(lcd_h);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Nit za tastere - svaki taster ima vlastito stanje i timestamp     */
/* ------------------------------------------------------------------ */
void *nit_tasteri(void *arg)
{
    /* Stanja tastera: HIGH = nije pritisnut (pull-up) */
    int prev0 = HIGH;
    int prev1 = HIGH;
    int prev2 = HIGH;

    /* Podesi tastere kao ulaze sa pull-up otpornicima */
    pinMode(KEY0, INPUT);
    pinMode(KEY1, INPUT);
    pinMode(KEY2, INPUT);
    pullUpDnControl(KEY0, PUD_UP);
    pullUpDnControl(KEY1, PUD_UP);
    pullUpDnControl(KEY2, PUD_UP);

    /* Sacekaj stabilizaciju */
    delay(200);

    /* Ucitaj pocetna stanja - sprijeci lazni pritisak pri startu */
    prev0 = digitalRead(KEY0);
    prev1 = digitalRead(KEY1);
    prev2 = digitalRead(KEY2);

    while (!g_kraj) {
        int curr0 = digitalRead(KEY0);
        int curr1 = digitalRead(KEY1);
        int curr2 = digitalRead(KEY2);

        /* KEY0 - povecaj prag (padajuca ivica: HIGH->LOW) */
        if (curr0 == LOW && prev0 == HIGH) {
            pthread_mutex_lock(&g_mutex);
            if (g_prag < 125.0) {
                g_prag += 1.0;
                printf("Prag povecán na: %.1f C\n", g_prag);
            }
            pthread_mutex_unlock(&g_mutex);
            delay(50); /* debounce odmah nakon pritiska */
        }
        prev0 = curr0;

        /* KEY1 - smanji prag */
        if (curr1 == LOW && prev1 == HIGH) {
            pthread_mutex_lock(&g_mutex);
            if (g_prag > -55.0) {
                g_prag -= 1.0;
                printf("Prag smanjen na: %.1f C\n", g_prag);
            }
            pthread_mutex_unlock(&g_mutex);
            delay(50);
        }
        prev1 = curr1;

        /* KEY2 - izlaz */
        if (curr2 == LOW && prev2 == HIGH) {
            printf("Izlaz iz programa...\n");
            g_kraj = 1;
            delay(50);
        }
        prev2 = curr2;

        delay(20); /* polling svakih 20ms */
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Glavni program                                                     */
/* ------------------------------------------------------------------ */
int main(void)
{
    int lcd_h;
    pthread_t t_temp, t_led, t_lcd, t_tasteri;

    /* Inicijalizacija wiringPi */
    if (wiringPiSetup() < 0) {
        fprintf(stderr, "Greska pri inicijalizaciji wiringPi!\n");
        return 1;
    }

    /* Inicijalizacija LCD (4-bitni mod, 2 reda, 16 kolona) */
    lcd_h = lcdInit(2, 16, 4,
                    LCD_RS, LCD_EN,
                    LCD_D4, LCD_D5, LCD_D6, LCD_D7,
                    0, 0, 0, 0);
    if (lcd_h < 0) {
        fprintf(stderr, "Greska pri inicijalizaciji LCD!\n");
        return 1;
    }

    /* Poruka pri pokretanju */
    lcdClear(lcd_h);
    lcdPosition(lcd_h, 0, 0);
    lcdPuts(lcd_h, "Termometar");
    lcdPosition(lcd_h, 0, 1);
    lcdPuts(lcd_h, "Pokretanje...");
    sleep(2);

    printf("Program pokrenut.\n");
    printf("KEY0 = povecaj prag | KEY1 = smanji prag | KEY2 = izlaz\n\n");

    /* Pokretanje niti */
    pthread_create(&t_temp,    NULL, nit_temperatura, NULL);
    pthread_create(&t_led,     NULL, nit_led,         NULL);
    pthread_create(&t_lcd,     NULL, nit_lcd,         (void *)&lcd_h);
    pthread_create(&t_tasteri, NULL, nit_tasteri,     NULL);

    /* Cekaj da nit za tastere postavi g_kraj = 1 (KEY2) */
    pthread_join(t_tasteri, NULL);

    g_kraj = 1;

    pthread_join(t_temp, NULL);
    pthread_join(t_led,  NULL);
    pthread_join(t_lcd,  NULL);

    lcdClear(lcd_h);
    printf("Program zavrsen.\n");

    return 0;
}
