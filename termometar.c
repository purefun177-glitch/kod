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
#define SENSOR_ID   "e723360"

/* ---- Pinovi za LCD (wiringPi oznake, 4-bitni mod kao na DVK512) ---- */
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

/* ---- Globalne promjenljive dijeljene između niti ---- */
static double g_temperatura    = 0.0;
static double g_temp_min       = 9999.0;
static double g_temp_max       = -9999.0;
static double g_prag           = DEFAULT_THRESHOLD;
static int    g_alarm_aktivan  = 0;
static int    g_kraj           = 0;

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
             "/sys/bus/w1/devices/28-00000%s/w1_slave", SENSOR_ID);

    ft = fopen(putanja, "r");
    if (ft == NULL) {
        fprintf(stderr, "Greska: ne mogu otvoriti senzor: %s\n", putanja);
        return -999.0;
    }

    fgets(buffer, sizeof(buffer), ft); /* prva linija (CRC) */
    fgets(buffer, sizeof(buffer), ft); /* druga linija (temperatura) */
    fclose(ft);

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
/*  Nit za LED alarm (trepće kada je alarm aktivan)                   */
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
/*  Nit za ažuriranje LCD ekrana                                      */
/* ------------------------------------------------------------------ */
void *nit_lcd(void *arg)
{
    int lcd_h  = *(int *)arg;
    int prikaz = 0;   /* 0 = trenutna+prag, 1 = min+max */
    int brojac = 0;

    while (!g_kraj) {
        lcdClear(lcd_h);

        if (prikaz == 0) {
            /* Prikaz 1: trenutna temperatura i prag */
            lcdPosition(lcd_h, 0, 0);
            if (g_alarm_aktivan)
                lcdPrintf(lcd_h, "Temp:%.1fC ALARM!", g_temperatura);
            else
                lcdPrintf(lcd_h, "Temp: %.1f C", g_temperatura);

            lcdPosition(lcd_h, 0, 1);
            lcdPrintf(lcd_h, "Prag: %.1f C", g_prag);
        } else {
            /* Prikaz 2: minimalna i maksimalna temperatura */
            lcdPosition(lcd_h, 0, 0);
            lcdPrintf(lcd_h, "Min: %.1f C",
                      g_temp_min > 9000 ? 0.0 : g_temp_min);

            lcdPosition(lcd_h, 0, 1);
            lcdPrintf(lcd_h, "Max: %.1f C",
                      g_temp_max < -9000 ? 0.0 : g_temp_max);
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
/*  Nit za tastere (softversko diferenciranje)                        */
/* ------------------------------------------------------------------ */
void *nit_tasteri(void *arg)
{
    int stanje_key0 = HIGH;  /* prethodno stanje KEY0 */
    int stanje_key1 = HIGH;  /* prethodno stanje KEY1 */
    int stanje_key2 = HIGH;  /* prethodno stanje KEY2 */
    int trenutno;

    /* Podesi tastere kao ulaze */
    pinMode(KEY0, INPUT);
    pinMode(KEY1, INPUT);
    pinMode(KEY2, INPUT);

    while (!g_kraj) {

        /* KEY0 - povećaj prag */
        trenutno = digitalRead(KEY0);
        if (trenutno == LOW && stanje_key0 == HIGH) {
            /* taster pritisnut (padajuća ivica) */
            if (g_prag < 125.0) {
                g_prag += 1.0;
                printf("Prag povećan na: %.1f C\n", g_prag);
            }
        }
        stanje_key0 = trenutno;

        /* KEY1 - smanji prag */
        trenutno = digitalRead(KEY1);
        if (trenutno == LOW && stanje_key1 == HIGH) {
            if (g_prag > -55.0) {
                g_prag -= 1.0;
                printf("Prag smanjen na: %.1f C\n", g_prag);
            }
        }
        stanje_key1 = trenutno;

        /* KEY2 - izlaz iz programa */
        trenutno = digitalRead(KEY2);
        if (trenutno == LOW && stanje_key2 == HIGH) {
            printf("Izlaz iz programa...\n");
            g_kraj = 1;
        }
        stanje_key2 = trenutno;

        delay(50); /* debounce - čekaj 50ms između provjera */
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

    /* Čekaj da nit za tastere postavi g_kraj = 1 (KEY2) */
    pthread_join(t_tasteri, NULL);

    g_kraj = 1;

    pthread_join(t_temp, NULL);
    pthread_join(t_led,  NULL);
    pthread_join(t_lcd,  NULL);

    lcdClear(lcd_h);
    printf("Program završen.\n");

    return 0;
}
