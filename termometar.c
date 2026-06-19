#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wiringPi.h>
#include <lcd.h>

#define SENSOR_ID          "28-00000d45c605"

#define LCD_RS      3
#define LCD_EN      14
#define LCD_D4      4
#define LCD_D5      12
#define LCD_D6      13
#define LCD_D7      6

#define LED_PIN     28      /* LED3 na DVK512 */

#define KEY3        24      /* povećava prag za 1°C */
#define KEY1        22      /* smanjuje prag za 1°C */
#define KEY2        23      /* izlaz iz programa */

#define DEFAULT_THRESHOLD  30.0

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static double       g_temperatura   = 0.0;
static double       g_temp_min      = 9999.0;
static double       g_temp_max      = -9999.0;
static double       g_prag          = DEFAULT_THRESHOLD;
static int          g_alarm_aktivan = 0;
static volatile int g_kraj          = 0;

/* Čita temperaturu sa DS18B20 senzora putem 1-Wire sysfs interfejsa.
   Vraća temperaturu u °C, ili -999.0 ako čitanje nije uspjelo. */
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

    fgets(buffer, sizeof(buffer), ft); /* prva linija sadrži CRC status */
    fgets(buffer, sizeof(buffer), ft); /* druga linija sadrži t=XXXXX */
    fclose(ft);

    tmp = strstr(buffer, "t=");
    if (tmp == NULL) {
        fprintf(stderr, "Greska: nije pronadjen podatak o temperaturi\n");
        return -999.0;
    }

    tmp += 2;                          /* pomjeri pokazivač iza "t=" */
    vrijednost = atol(tmp);            /* vrijednost je u milistepeni */

    return (double)vrijednost / 1000.0;
}

/* Nit za očitavanje temperature: osvježava globalnu temperaturu,
   min/max i status alarma jednom u sekundi. */
void *nit_temperatura(void *arg)
{
    double t;

    while (!g_kraj) {
        t = ocitaj_temperaturu();

        if (t > -999.0) {
            pthread_mutex_lock(&g_mutex);
            g_temperatura = t;
            if (t < g_temp_min) g_temp_min = t;  /* ažuriraj minimum */
            if (t > g_temp_max) g_temp_max = t;  /* ažuriraj maksimum */
            g_alarm_aktivan = (t >= g_prag) ? 1 : 0;
            pthread_mutex_unlock(&g_mutex);
        }

        sleep(1);
    }
    return NULL;
}

/* Nit za LED alarm: trepće sa periodom 1s dok je alarm aktivan,
   inače drži LED ugašenom. */
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
            delay(500);                /* 500ms uključeno */
            digitalWrite(LED_PIN, LOW);
            delay(500);                /* 500ms isključeno */
        } else {
            digitalWrite(LED_PIN, LOW);
            delay(100);
        }
    }

    digitalWrite(LED_PIN, LOW);        /* ugasi LED pri izlasku */
    return NULL;
}

/* Nit za LCD: naizmjenično prikazuje Temp/Prag i Min/Max,
   sa promjenom ekrana svakih 3 sekunde. */
void *nit_lcd(void *arg)
{
    int lcd_h  = *(int *)arg;
    int prikaz = 0;    /* 0 = trenutna temperatura + prag, 1 = min + max */
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
                lcdPrintf(lcd_h, "Temp:%.1fC ALARM!", temp); /* alarm poruka */
            else
                lcdPrintf(lcd_h, "Temp: %.1f C", temp);

            lcdPosition(lcd_h, 0, 1);
            lcdPrintf(lcd_h, "Prag: %.1f C", prag);
        } else {
            lcdPosition(lcd_h, 0, 0);
            lcdPrintf(lcd_h, "Min: %.1f C",
                      min > 9000 ? 0.0 : min);  /* zaštita od init vrijednosti */

            lcdPosition(lcd_h, 0, 1);
            lcdPrintf(lcd_h, "Max: %.1f C",
                      max < -9000 ? 0.0 : max); /* zaštita od init vrijednosti */
        }

        brojac++;
        if (brojac >= 3) {             /* promjena ekrana svakih 3 sekunde */
            brojac = 0;
            prikaz = 1 - prikaz;
        }

        sleep(1);
    }

    lcdClear(lcd_h);
    return NULL;
}

/* Nit za tastere: polling svakih 20ms sa debounce od 50ms.
   Detekcija pritiska na padajućoj ivici (HIGH -> LOW). */
void *nit_tasteri(void *arg)
{
    int prev3 = HIGH;
    int prev1 = HIGH;
    int prev2 = HIGH;

    pinMode(KEY3, INPUT);
    pinMode(KEY1, INPUT);
    pinMode(KEY2, INPUT);
    pullUpDnControl(KEY3, PUD_UP);     /* interni pull-up otpornici */
    pullUpDnControl(KEY1, PUD_UP);
    pullUpDnControl(KEY2, PUD_UP);

    delay(200);                        /* čekaj stabilizaciju pinova */

    /* učitaj početna stanja da se spriječi lažni pritisak pri startu */
    prev3 = digitalRead(KEY3);
    prev1 = digitalRead(KEY1);
    prev2 = digitalRead(KEY2);

    while (!g_kraj) {
        int curr3 = digitalRead(KEY3);
        int curr1 = digitalRead(KEY1);
        int curr2 = digitalRead(KEY2);

        if (curr3 == LOW && prev3 == HIGH) {   /* KEY3: povećaj prag */
            pthread_mutex_lock(&g_mutex);
            if (g_prag < 125.0)                /* gornja granica DS18B20 */
                g_prag += 1.0;
            pthread_mutex_unlock(&g_mutex);
            delay(50);                         /* debounce */
        }
        prev3 = curr3;

        if (curr1 == LOW && prev1 == HIGH) {   /* KEY1: smanji prag */
            pthread_mutex_lock(&g_mutex);
            if (g_prag > -55.0)                /* donja granica DS18B20 */
                g_prag -= 1.0;
            pthread_mutex_unlock(&g_mutex);
            delay(50);
        }
        prev1 = curr1;

        if (curr2 == LOW && prev2 == HIGH) {   /* KEY2: izlaz iz programa */
            g_kraj = 1;
            delay(50);
        }
        prev2 = curr2;

        delay(20);                             /* polling interval */
    }

    return NULL;
}

int main(void)
{
    int lcd_h;
    pthread_t t_temp, t_led, t_lcd, t_tasteri;

    if (wiringPiSetup() < 0) {
        fprintf(stderr, "Greska pri inicijalizaciji wiringPi!\n");
        return 1;
    }

    /* inicijalizacija LCD u 4-bitnom modu, 2 reda, 16 kolona */
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

    /* pokretanje niti: temperatura, LED, LCD, tasteri */
    pthread_create(&t_temp,    NULL, nit_temperatura, NULL);
    pthread_create(&t_led,     NULL, nit_led,         NULL);
    pthread_create(&t_lcd,     NULL, nit_lcd,         (void *)&lcd_h);
    pthread_create(&t_tasteri, NULL, nit_tasteri,     NULL);

    pthread_join(t_tasteri, NULL);     /* blokiraj dok KEY2 ne postavi g_kraj=1 */

    g_kraj = 1;                        /* osiguraj da sve niti izađu */

    pthread_join(t_temp, NULL);
    pthread_join(t_led,  NULL);
    pthread_join(t_lcd,  NULL);

    lcdClear(lcd_h);
    return 0;
}
