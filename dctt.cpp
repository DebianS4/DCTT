/* =====================================================================
 * DCTT - Diagnostico y Codificacion Taller TT
 * Audi TT Mk1 (8N) 2005 - cable KKL 409.1 (FTDI FT232RL) - Windows XP SP3
 *
 * UN SOLO ARCHIVO. Nada de headers, nada de makefiles:
 *   gcc -O2 -mwindows -o DCTT.exe dctt.cpp -lwinmm -lgdi32 -luser32
 *
 * Contenido, en orden:
 *   1. Tipos y constantes del protocolo KWP1281
 *   2. Puerto serie Win32 y bit-bang a 5 baudios con ftd2xx.dll
 *   3. Bloques, ACK 0x03 y comandos 02/03/04/05/07/08/10/11/16
 *   4. Interfaz Win32 (ventana, controles, registro)
 * ===================================================================== */
/* Compatibilidad Windows XP SP3: fijamos la version de destino en el propio
   codigo para que no dependa de las banderas del compilador. */
#ifndef WINVER
#define WINVER 0x0501
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0500
#endif
#ifndef _WIN32_WINDOWS
#define _WIN32_WINDOWS 0x0501
#endif

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== */
/* 1. Tipos y constantes                                                 */
/* ===================================================================== */
/* =====================================================================
 * DCTT - Diagnóstico y Codificación Taller TT
 * kwp1281.h - enlace K-line con protocolo VAG KWP1281
 *
 * Pensado para un cable KKL 409.1 (FTDI FT232RL) sobre Windows XP SP3.
 * Compilado con MinGW gcc o Visual C++ 6.0. Sin MFC, sin .NET.
 * ===================================================================== */

#include <windows.h>

#define KWP_MAX_BLOCK   128
#define KWP_MAX_DTC      64
#define KWP_TEXT_LEN     96

/* Direcciones de los módulos del Audi TT 8N */
#define ECU_ENGINE       0x01
#define ECU_ABS          0x03
#define ECU_AIRBAG       0x15
#define ECU_INSTRUMENTS  0x17
#define ECU_COMFORT      0x35

typedef struct {
    HANDLE hCom;
    char   portName[16];   /* "\\\\.\\COM1" */
    DWORD  baud;
    BOOL   open;
    BYTE   counter;        /* contador de bloque, incrementa en cada envío */
    BYTE   ecu;            /* dirección del módulo con el que hablamos */
    char   ident[KWP_TEXT_LEN];
} KwpLink;

typedef struct {
    int  code;             /* 17544, 00515... valor decimal del par de bytes */
    char text[KWP_TEXT_LEN];
} KwpDtc;

/* --- puerto serie ---------------------------------------------------- */
BOOL kwpOpen(KwpLink *link, const char *portName, DWORD baud);
void kwpClose(KwpLink *link);
BOOL kwpSetBaud(KwpLink *link, DWORD baud);

/* --- inicialización a 5 baudios (bit-bang del FT232RL) --------------- */
BOOL kwpFiveBaudInit(KwpLink *link, BYTE address);

/* --- sesión KWP1281 -------------------------------------------------- */
BOOL kwpConnect(KwpLink *link, BYTE address);
BOOL kwpSendBlock(KwpLink *link, const BYTE *payload, int len);
int  kwpReadBlock(KwpLink *link, BYTE *out, int max, DWORD timeoutMs);
void kwpEndSession(KwpLink *link);

/* --- funciones de taller -------------------------------------------- */
BOOL kwpReadIdentification(KwpLink *link);
BOOL kwpReadDtc(KwpLink *link, KwpDtc *list, int *count);
BOOL kwpClearDtc(KwpLink *link);
BOOL kwpReadCoding(KwpLink *link, char *digits, int size);
BOOL kwpWriteCoding(KwpLink *link, const char *digits);
BOOL kwpReadMeasuringGroup(KwpLink *link, BYTE group, char fields[4][24]);
BOOL kwpOutputTest(KwpLink *link, BYTE step);

/* --- funciones avanzadas (equivalente a VCDS-Lite y extras) ---------- */
BOOL kwpBasicSettings(KwpLink *link, BYTE group);
BOOL kwpReadAdaptation(KwpLink *link, BYTE channel, char *out, int size);
BOOL kwpWriteAdaptation(KwpLink *link, BYTE channel, int value);
BOOL kwpLogin(KwpLink *link, WORD code);
BOOL kwpReadiness(KwpLink *link, char *out, int size);
BOOL kwpLogGroupCsv(KwpLink *link, BYTE group, const char *path);
BOOL kwpAutoScan(KwpLink *link, const char *portName, BYTE first, BYTE last,
                 char *report, int size);

/* --- utilidades ------------------------------------------------------ */
int  kwpLoadDtcFile(const char *path, KwpDtc *list, int max);
const char *kwpFindDtc(const KwpDtc *list, int count, int code);

/* ===================================================================== */
/* 2 y 3. Enlace KWP1281                                                  */
/* ===================================================================== */

/* =====================================================================
 * DCTT - Diagnóstico y Codificación Taller TT
 * kwp1281.cpp - enlace serie + protocolo VAG KWP1281
 *
 * Orden real de la sesión (no lo cambies sin leer los comentarios):
 *   1. Dirección del módulo a 5 baudios, bit a bit, con el FT232RL en
 *      modo bit-bang (un puerto serie normal no puede bajar a 5 baudios).
 *   2. El ECU contesta 0x55, luego KW1 y KW2, también a 5 baudios.
 *   3. El tester devuelve el complemento de KW2 a 5 baudios.
 *   4. Se cierra el bit-bang, se abre el COM a 9600 y, si no hay
 *      bloques válidos, se reintenta a 10400.
 *   5. Cada bloque recibido se confirma con 0x03 o la sesión se cae.
 *
 * IMPORTANTE: los tiempos y el comportamiento del eco dependen del cable
 * KKL concreto. El programa registra cada byte para que puedas ajustar.
 * ===================================================================== */


#define KWP_5BAUD_MS   200     /* 1 bit = 200 ms = 5 baudios */
#define KWP_BAUD_FAST  9600
#define KWP_BAUD_ALT   10400
#define KWP_SYNC       0x55
#define KWP_ACK        0x03
#define KWP_TITLE_ACK  0x03
#define KWP_TITLE_DATA 0xF6

/* ------------------------------------------------------------------ */
/* FTDI D2XX cargado dinámicamente: sin ftd2xx.dll no hay 5 baudios.  */
/* ------------------------------------------------------------------ */
typedef unsigned long FT_STATUS;
typedef FT_STATUS (WINAPI *PFN_FT_OPEN)(int, void **);
typedef FT_STATUS (WINAPI *PFN_FT_CLOSE)(void *);
typedef FT_STATUS (WINAPI *PFN_FT_SET_BAUD)(void *, unsigned long);
typedef FT_STATUS (WINAPI *PFN_FT_SET_BITMODE)(void *, unsigned char, unsigned char);
typedef FT_STATUS (WINAPI *PFN_FT_WRITE)(void *, void *, unsigned long, unsigned long *);
typedef FT_STATUS (WINAPI *PFN_FT_GET_BITMODE)(void *, unsigned char *);
typedef FT_STATUS (WINAPI *PFN_FT_PURGE)(void *, unsigned long);

static PFN_FT_OPEN        pFtOpen;
static PFN_FT_CLOSE       pFtClose;
static PFN_FT_SET_BAUD    pFtSetBaud;
static PFN_FT_SET_BITMODE pFtSetBitMode;
static PFN_FT_WRITE       pFtWrite;
static PFN_FT_GET_BITMODE pFtGetBitMode;
static PFN_FT_PURGE       pFtPurge;

static BOOL ftLoad(void)
{
    HMODULE dll;
    if (pFtOpen) return TRUE;
    dll = LoadLibraryA("ftd2xx.dll");
    if (!dll) {
        OutputDebugStringA("DCTT: ftd2xx.dll no encontrada; sin inicializacion a 5 baudios\n");
        return FALSE;
    }
    pFtOpen        = (PFN_FT_OPEN)GetProcAddress(dll, "FT_Open");
    pFtClose       = (PFN_FT_CLOSE)GetProcAddress(dll, "FT_Close");
    pFtSetBaud     = (PFN_FT_SET_BAUD)GetProcAddress(dll, "FT_SetBaudRate");
    pFtSetBitMode  = (PFN_FT_SET_BITMODE)GetProcAddress(dll, "FT_SetBitMode");
    pFtWrite       = (PFN_FT_WRITE)GetProcAddress(dll, "FT_Write");
    pFtGetBitMode  = (PFN_FT_GET_BITMODE)GetProcAddress(dll, "FT_GetBitMode");
    pFtPurge       = (PFN_FT_PURGE)GetProcAddress(dll, "FT_Purge");
    return pFtOpen && pFtClose && pFtSetBitMode && pFtWrite;
}

/* ------------------------------------------------------------------ */
/* Puerto serie Win32                                                  */
/* ------------------------------------------------------------------ */
BOOL kwpSetBaud(KwpLink *link, DWORD baud)
{
    DCB dcb;

    if (!link->open) return FALSE;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(link->hCom, &dcb)) return FALSE;
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_DISABLE;
    dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX  = FALSE;
    if (!SetCommState(link->hCom, &dcb)) return FALSE;
    link->baud = baud;
    return TRUE;
}

BOOL kwpOpen(KwpLink *link, const char *portName, DWORD baud)
{
    COMMTIMEOUTS to;

    if (link->open) kwpClose(link);
    strncpy(link->portName, portName, sizeof(link->portName) - 1);
    link->portName[sizeof(link->portName) - 1] = '\0';

    link->hCom = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                             OPEN_EXISTING, 0, NULL);
    if (link->hCom == INVALID_HANDLE_VALUE) {
        link->hCom = NULL;
        return FALSE;
    }
    link->open = TRUE;
    SetupComm(link->hCom, 512, 512);
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutMultiplier = 10;
    to.ReadTotalTimeoutConstant = 500;
    SetCommTimeouts(link->hCom, &to);
    return kwpSetBaud(link, baud);
}

void kwpClose(KwpLink *link)
{
    if (link->open && link->hCom) {
        PurgeComm(link->hCom, PURGE_RXCLEAR | PURGE_TXCLEAR);
        CloseHandle(link->hCom);
    }
    link->hCom = NULL;
    link->open = FALSE;
}

static BOOL serWrite(KwpLink *link, const BYTE *data, DWORD len)
{
    DWORD written = 0;
    if (!link->open) return FALSE;
    if (!WriteFile(link->hCom, data, len, &written, NULL)) return FALSE;
    return written == len;
}

static int serReadByte(KwpLink *link, DWORD timeoutMs)
{
    BYTE  byte = 0;
    DWORD got  = 0;
    COMMTIMEOUTS to;

    if (!link->open) return -1;
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = 0;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = timeoutMs;
    SetCommTimeouts(link->hCom, &to);
    if (!ReadFile(link->hCom, &byte, 1, &got, NULL) || got != 1) return -1;
    return (int)byte;
}

/* ------------------------------------------------------------------ */
/* Inicialización a 5 baudios con el FT232RL en bit-bang              */
/* ------------------------------------------------------------------ */
static void ftBit(void *ft, int high)
{
    unsigned char value = high ? 0x01 : 0x00;   /* D0 = TXD */
    unsigned long written = 0;
    pFtWrite(ft, &value, 1, &written);
    Sleep(KWP_5BAUD_MS);
}

static void ftSend5Baud(void *ft, BYTE value)
{
    int i;
    ftBit(ft, 0);                                  /* bit de arranque */
    for (i = 0; i < 7; i++) ftBit(ft, (value >> i) & 1);
    ftBit(ft, 1);                                  /* bit de parada */
}

/* Lee un byte a 5 baudios muestreando la línea K (pin RXD = bit 1). */
static int ftRead5Baud(void *ft, DWORD timeoutMs)
{
    unsigned char pins = 0;
    int i, value = 0;
    DWORD waited = 0;

    /* espera del bit de arranque (línea a 0) */
    while (waited < timeoutMs) {
        if (pFtGetBitMode && pFtGetBitMode(ft, &pins) == 0 && (pins & 0x02) == 0) break;
        Sleep(20);
        waited += 20;
    }
    if (waited >= timeoutMs) return -1;
    Sleep(KWP_5BAUD_MS / 2);
    for (i = 0; i < 8; i++) {
        if (pFtGetBitMode && pFtGetBitMode(ft, &pins) == 0 && (pins & 0x02)) value |= (1 << i);
        Sleep(KWP_5BAUD_MS);
    }
    return value & 0xFF;
}

BOOL kwpFiveBaudInit(KwpLink *link, BYTE address)
{
    const char *digits;
    int index, sync, kw1, kw2;
    void *ft = NULL;

    if (!ftLoad()) return FALSE;
    digits = strrchr(link->portName, 'M');
    index = digits ? atoi(digits + 1) - 1 : 0;
    if (index < 0) index = 0;

    if (pFtOpen(index, &ft) != 0) {
        OutputDebugStringA("DCTT: no se pudo abrir el FT232RL en modo D2XX\n");
        return FALSE;
    }
    pFtSetBaud(ft, 9600);          /* irrelevante en bit-bang, el ritmo lo da Sleep */
    pFtSetBitMode(ft, 0x01, 0x01); /* 0x01 = ASYNC_BITBANG, D0 como salida */
    Sleep(50);
    if (pFtPurge) pFtPurge(ft, 3);

    ftSend5Baud(ft, address);      /* dirección del módulo a 5 baudios */
    Sleep(60);

    /* 0x55 de sincronismo, KW1 y KW2 llegan a 5 baudios */
    sync = ftRead5Baud(ft, 1000);
    if (sync != KWP_SYNC) {
        OutputDebugStringA("DCTT: sin sincronismo 0x55 a 5 baudios (revisa alimentacion y pin 7)\n");
        pFtSetBitMode(ft, 0x00, 0x00);
        pFtClose(ft);
        return FALSE;
    }
    kw1 = ftRead5Baud(ft, 600);
    kw2 = ftRead5Baud(ft, 600);
    if (kw2 >= 0) {
        /* complemento de KW2: la ECU abre la sesión KWP1281 */
        ftSend5Baud(ft, (BYTE)(~kw2 & 0xFF));
        Sleep(60);
    }
    (void)kw1;

    pFtSetBitMode(ft, 0x00, 0x00); /* vuelta al modo serie normal */
    pFtClose(ft);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Bloques KWP1281                                                     */
/* Formato: [longitud][contador][titulo][datos...][checksum XOR]       */
/* ------------------------------------------------------------------ */
BOOL kwpSendBlock(KwpLink *link, const BYTE *payload, int len)
{
    BYTE frame[KWP_MAX_BLOCK];
    int  i, total;
    BYTE checksum = 0;

    if (len <= 0 || len > 40) return FALSE;
    frame[0] = (BYTE)(len + 1);            /* contador + payload */
    frame[1] = link->counter;
    memcpy(&frame[2], payload, len);
    total = len + 2;
    for (i = 0; i < total; i++) checksum ^= frame[i];
    frame[total] = checksum;
    total += 1;

    if (!serWrite(link, frame, (DWORD)total)) return FALSE;
    link->counter = (BYTE)(link->counter + 1);
    return TRUE;
}

int kwpReadBlock(KwpLink *link, BYTE *out, int max, DWORD timeoutMs)
{
    int  len, i, byte, total;
    BYTE checksum = 0;

    len = serReadByte(link, timeoutMs);
    if (len <= 0 || len > max - 2) return -1;

    out[0] = (BYTE)len;
    total = len + 1;                       /* + checksum */
    for (i = 1; i < total; i++) {
        byte = serReadByte(link, 250);
        if (byte < 0) return -1;
        out[i] = (BYTE)byte;
    }
    for (i = 0; i < total - 1; i++) checksum ^= out[i];
    if (checksum != out[total - 1]) return -1;

    /* ACK obligatorio: sin él la ECU deja de enviar bloques */
    {
        BYTE ack = KWP_ACK;
        serWrite(link, &ack, 1);
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Apertura de sesión                                                  */
/* ------------------------------------------------------------------ */
BOOL kwpConnect(KwpLink *link, BYTE address)
{
    BYTE block[KWP_MAX_BLOCK];
    int  n;

    if (link->open) kwpClose(link);
    link->ecu = address;
    link->counter = 0x01;

    if (!kwpFiveBaudInit(link, address)) return FALSE;

    if (!kwpOpen(link, link->portName, KWP_BAUD_FAST)) return FALSE;
    n = kwpReadBlock(link, block, sizeof(block), 1200);
    if (n <= 0) {
        if (!kwpSetBaud(link, KWP_BAUD_ALT)) return FALSE;
        n = kwpReadBlock(link, block, sizeof(block), 1200);
        if (n <= 0) {
            kwpClose(link);
            return FALSE;
        }
    }
    return kwpReadIdentification(link);
}

void kwpEndSession(KwpLink *link)
{
    BYTE bye = 0x06;                       /* fin de sesión */
    if (link->open) {
        serWrite(link, &bye, 1);
        Sleep(80);
    }
    kwpClose(link);
}

BOOL kwpReadIdentification(KwpLink *link)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[2];
    int  n, i, pos = 0;

    payload[0] = 0x00;
    if (!kwpSendBlock(link, payload, 1)) return FALSE;
    n = kwpReadBlock(link, block, sizeof(block), 1500);
    if (n <= 0) return FALSE;

    link->ident[0] = '\0';
    for (i = 3; i < n - 1 && pos < KWP_TEXT_LEN - 1; i++) {
        char c = (char)block[i];
        link->ident[pos++] = (c >= 32 && c < 127) ? c : ' ';
    }
    link->ident[pos] = '\0';
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Averías: 02 leer / 05 borrar                                        */
/* ------------------------------------------------------------------ */
BOOL kwpReadDtc(KwpLink *link, KwpDtc *list, int *count)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[2];
    int  n, i, found = 0, guard = 0;

    *count = 0;
    payload[0] = 0x02;
    if (!kwpSendBlock(link, payload, 1)) return FALSE;

    while (guard++ < 24) {
        n = kwpReadBlock(link, block, sizeof(block), 1200);
        if (n <= 2) break;                            /* sin más datos */
        if (block[2] == KWP_TITLE_ACK) continue;      /* ACK intermedio */
        for (i = 3; i + 1 < n - 1; i += 2) {
            if (found >= KWP_MAX_DTC) break;
            list[found].code = (block[i] << 8) | block[i + 1];
            list[found].text[0] = '\0';
            found++;
        }
    }
    *count = found;
    return TRUE;
}

BOOL kwpClearDtc(KwpLink *link)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[2];

    payload[0] = 0x05;
    if (!kwpSendBlock(link, payload, 1)) return FALSE;
    return kwpReadBlock(link, block, sizeof(block), 1500) > 0;
}

/* ------------------------------------------------------------------ */
/* Codificación: 07 leer / escribir                                    */
/* ------------------------------------------------------------------ */
BOOL kwpReadCoding(KwpLink *link, char *digits, int size)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[2];
    int  n, i, pos = 0;

    payload[0] = 0x07;
    if (!kwpSendBlock(link, payload, 1)) return FALSE;
    n = kwpReadBlock(link, block, sizeof(block), 1500);
    if (n <= 3) return FALSE;

    for (i = 3; i < n - 1 && pos < size - 1; i++) {
        char c = (char)block[i];
        if (c >= '0' && c <= '9') digits[pos++] = c;
    }
    digits[pos] = '\0';
    return pos > 0;
}

BOOL kwpWriteCoding(KwpLink *link, const char *digits)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[24];
    int  len = (int)strlen(digits), i;

    if (len < 5 || len > 13) return FALSE;   /* CCCCC + WSC + importador */
    payload[0] = 0x07;
    for (i = 0; i < len; i++) payload[i + 1] = (BYTE)digits[i];
    if (!kwpSendBlock(link, payload, len + 1)) return FALSE;
    return kwpReadBlock(link, block, sizeof(block), 2000) > 0;
}

/* ------------------------------------------------------------------ */
/* Bloques de medición: 08 + número de grupo                           */
/* ------------------------------------------------------------------ */
BOOL kwpReadMeasuringGroup(KwpLink *link, BYTE group, char fields[4][24])
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[3];
    int  n, i;

    payload[0] = 0x08;
    payload[1] = group;
    if (!kwpSendBlock(link, payload, 2)) return FALSE;
    n = kwpReadBlock(link, block, sizeof(block), 1500);
    if (n < 11) return FALSE;

    /* cuatro valores de dos bytes; la escala real la fija el byte de
       formato que cierra el bloque, por eso aquí se muestra en bruto. */
    for (i = 0; i < 4; i++) {
        int raw = (block[3 + i * 2] << 8) | block[4 + i * 2];
        sprintf(fields[i], "%d (0x%04X)", raw, raw);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Pruebas de actuadores: 03 + paso                                    */
/* ------------------------------------------------------------------ */
BOOL kwpOutputTest(KwpLink *link, BYTE step)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[3];

    payload[0] = 0x03;
    payload[1] = step;
    if (!kwpSendBlock(link, payload, 2)) return FALSE;
    return kwpReadBlock(link, block, sizeof(block), 2000) > 0;
}

/* ------------------------------------------------------------------ */
/* Funciones avanzadas                                                 */
/* Los números de comando 0x04 / 0x10 / 0x11 son los que usan los      */
/* módulos del grupo VAG de esa época; confírmalos con la etiqueta del  */
/* módulo antes de escribir valores de adaptación.                      */
/* ------------------------------------------------------------------ */
BOOL kwpBasicSettings(KwpLink *link, BYTE group)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[3];

    payload[0] = 0x04;
    payload[1] = group;
    if (!kwpSendBlock(link, payload, 2)) return FALSE;
    return kwpReadBlock(link, block, sizeof(block), 3000) > 0;
}

BOOL kwpReadAdaptation(KwpLink *link, BYTE channel, char *out, int size)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[3];
    int  n, i, pos = 0;

    payload[0] = 0x10;
    payload[1] = channel;
    if (!kwpSendBlock(link, payload, 2)) return FALSE;
    n = kwpReadBlock(link, block, sizeof(block), 2000);
    if (n <= 3) return FALSE;

    for (i = 3; i < n - 1 && pos < size - 1; i++) {
        char c = (char)block[i];
        if (c >= 32 && c < 127) out[pos++] = c;
    }
    out[pos] = '\0';
    return pos > 0;
}

BOOL kwpWriteAdaptation(KwpLink *link, BYTE channel, int value)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[8];
    int  len;

    len = sprintf((char *)&payload[2], "%05d", value);
    payload[0] = 0x10;
    payload[1] = channel;
    if (!kwpSendBlock(link, payload, len + 2)) return FALSE;
    return kwpReadBlock(link, block, sizeof(block), 2000) > 0;
}

BOOL kwpLogin(KwpLink *link, WORD code)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[8];
    int  len;

    len = sprintf((char *)&payload[2], "%05u", (unsigned)code);
    payload[0] = 0x11;
    payload[1] = 0x00;
    if (!kwpSendBlock(link, payload, len + 2)) return FALSE;
    return kwpReadBlock(link, block, sizeof(block), 2000) > 0;
}

BOOL kwpReadiness(KwpLink *link, char *out, int size)
{
    BYTE block[KWP_MAX_BLOCK];
    BYTE payload[3];
    int  n, i, pos = 0;

    payload[0] = 0x05;
    payload[1] = 0x00;
    if (!kwpSendBlock(link, payload, 2)) return FALSE;
    n = kwpReadBlock(link, block, sizeof(block), 2500);
    if (n <= 3) return FALSE;

    for (i = 3; i < n - 1 && pos < size - 1; i++) {
        char c = (char)block[i];
        out[pos++] = (c >= 32 && c < 127) ? c : ' ';
    }
    out[pos] = '\0';
    return pos > 0;
}

/* Guarda una muestra del grupo en CSV, sin recompilar y sin Excel. */
BOOL kwpLogGroupCsv(KwpLink *link, BYTE group, const char *path)
{
    char fields[4][24];
    FILE *f;
    int  i;

    if (!kwpReadMeasuringGroup(link, group, fields)) return FALSE;
    f = fopen(path, "a");
    if (!f) return FALSE;
    fprintf(f, "%03d;%s;%s;%s;%s\r\n", group, fields[0], fields[1], fields[2], fields[3]);
    fclose(f);
    return TRUE;
}

/* Recorre un rango de direcciones y deja un informe legible. */
BOOL kwpAutoScan(KwpLink *link, const char *portName, BYTE first, BYTE last,
                 char *report, int size)
{
    char  line[160];
    BYTE  address;
    int   used = 0;
    BOOL  any = FALSE;

    report[0] = '\0';
    for (address = first; address <= last; address++) {
        if (address == 0x00) continue;
        if (kwpConnect(link, address)) {
            KwpDtc list[KWP_MAX_DTC];
            int    count = 0;
            kwpReadDtc(link, list, &count);
            sprintf(line, "0x%02X  %-28s averias: %d\r\n", address, link->ident, count);
            any = TRUE;
            kwpEndSession(link);
        } else {
            sprintf(line, "0x%02X  sin respuesta\r\n", address);
        }
        if (used + (int)strlen(line) < size - 1) {
            strcat(report, line);
            used += (int)strlen(line);
        }
        if (address == 0xFF) break;
    }
    (void)portName;
    return any;
}

/* ------------------------------------------------------------------ */
/* Base de averías externa (CSV o TXT, sin recompilar)                 */
/* ------------------------------------------------------------------ */
int kwpLoadDtcFile(const char *path, KwpDtc *list, int max)
{
    FILE *f;
    char  line[256];
    int   count = 0;

    f = fopen(path, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f) && count < max) {
        char *sep, *text;
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        sep = strpbrk(line, ";,\t");
        if (!sep) continue;
        *sep = '\0';
        text = sep + 1;
        text[strcspn(text, "\r\n")] = '\0';
        list[count].code = atoi(line);
        if (list[count].code == 0) continue;
        strncpy(list[count].text, text, KWP_TEXT_LEN - 1);
        list[count].text[KWP_TEXT_LEN - 1] = '\0';
        count++;
    }
    fclose(f);
    return count;
}

const char *kwpFindDtc(const KwpDtc *list, int count, int code)
{
    int i;
    for (i = 0; i < count; i++) {
        if (list[i].code == code) return list[i].text;
    }
    return NULL;
}

/* ===================================================================== */
/* 4. Interfaz Win32                                                     */
/* ===================================================================== */

/* =====================================================================
 * DCTT - Diagnóstico y Codificación Taller TT
 * dctt.cpp - interfaz Win32 nativa (Windows XP SP3, 32 bits)
 *
 *   gcc -O2 -mwindows -o DCTT.exe dctt.cpp kwp1281.cpp -lwinmm
 *
 * Sin MFC, sin .NET, sin runtime moderno.
 * ===================================================================== */


#define IDC_PORT        1001
#define IDC_TEST        1002
#define IDC_ECU_BASE    1010   /* 1010..1014 = los cinco módulos */
#define IDC_DTC_LIST    1020
#define IDC_DTC_READ    1021
#define IDC_DTC_CLEAR   1022
#define IDC_CODING      1030
#define IDC_WSC         1031
#define IDC_CODE_READ   1032
#define IDC_CODE_WRITE  1033
#define IDC_GROUP       1040
#define IDC_GROUP_READ  1041
#define IDC_FIELD_BASE  1050   /* 1050..1053 */
#define IDC_TEST_START  1060
#define IDC_TEST_NEXT   1061
#define IDC_LOG         1070
#define IDC_AUTOSCAN    1080
#define IDC_CHANNEL     1081
#define IDC_VALUE       1082
#define IDC_ADAPT_READ  1083
#define IDC_ADAPT_WRITE 1084
#define IDC_BASIC       1085
#define IDC_LOGIN       1086
#define IDC_EXPORT      1087

static const BYTE  g_modules[5]   = { ECU_ENGINE, ECU_ABS, ECU_AIRBAG, ECU_INSTRUMENTS, ECU_COMFORT };
static const char *g_modNames[5]  = { "Motor 01", "ABS 03", "Airbag 15", "Cuadro 17", "Confort 35" };

static KwpLink g_link;
static KwpDtc  g_db[KWP_MAX_DTC];
static int     g_dbCount = 0;
static int     g_testStep = 0;
static HWND    g_log = NULL;

static void Log(const char *fmt, ...)
{
    char  buffer[1024];
    char  current[8192];
    va_list args;
    int   len;

    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);

    if (!g_log) return;
    GetWindowTextA(g_log, current, sizeof(current));
    len = (int)strlen(current);
    if (len < (int)sizeof(current) - 300) {
        strcat(current, buffer);
        strcat(current, "\r\n");
        SetWindowTextA(g_log, current);
        SendMessageA(g_log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_log, EM_SCROLLCARET, 0, 0);
    }
}

static void FillPorts(HWND combo)
{
    int i;
    char name[16];
    for (i = 1; i <= 4; i++) {
        sprintf(name, "COM%d", i);
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)name);
    }
    SendMessageA(combo, CB_SETCURSEL, 0, 0);
}

static void PortName(HWND hwnd, char *out, int size)
{
    HWND combo = GetDlgItem(hwnd, IDC_PORT);
    char text[16] = "COM1";
    GetWindowTextA(combo, text, sizeof(text));
    sprintf(out, "\\\\.\\%s", text);
    (void)size;
}

static void ReadDtcDatabase(void)
{
    g_dbCount = kwpLoadDtcFile("averias.csv", g_db, KWP_MAX_DTC);
    if (g_dbCount == 0) g_dbCount = kwpLoadDtcFile("averias-ejemplo.csv", g_db, KWP_MAX_DTC);
    Log("Base de averias: %d descripciones cargadas.", g_dbCount);
}

static void ConnectModule(HWND hwnd, BYTE address)
{
    char port[24];
    PortName(hwnd, port, sizeof(port));
    Log("--- Abriendo modulo 0x%02X en %s ---", address, port);

    if (kwpConnect(&g_link, address)) {
        char title[160];
        sprintf(title, "DCTT - modulo 0x%02X conectado", address);
        SetWindowTextA(hwnd, title);
        Log("Sesion abierta a %lu bps. Identificacion: %s", (unsigned long)g_link.baud, g_link.ident);
    } else {
        Log("Fallo de enlace. Comprueba contacto, pin 7 del conector y el puerto elegido.");
    }
}

static void ReadDtc(HWND hwnd)
{
    KwpDtc list[KWP_MAX_DTC];
    int    count = 0, i;
    HWND   box = GetDlgItem(hwnd, IDC_DTC_LIST);

    SendMessageA(box, LB_RESETCONTENT, 0, 0);
    if (!g_link.open) { Log("Sin sesion activa: abre un modulo primero."); return; }
    if (!kwpReadDtc(&g_link, list, &count)) { Log("El modulo no respondio a la lectura de averias."); return; }

    Log("Averias leidas: %d", count);
    for (i = 0; i < count; i++) {
        const char *text = kwpFindDtc(g_db, g_dbCount, list[i].code);
        char line[256];
        sprintf(line, "%05d  %s", list[i].code, text ? text : "(sin descripcion)");
        SendMessageA(box, LB_ADDSTRING, 0, (LPARAM)line);
    }
}

static void ReadCoding(HWND hwnd)
{
    char digits[32];

    if (!g_link.open) { Log("Sin sesion activa."); return; }
    if (kwpReadCoding(&g_link, digits, sizeof(digits))) {
        char soft[8];
        strncpy(soft, digits, 5);
        soft[5] = '\0';
        SetWindowTextA(GetDlgItem(hwnd, IDC_CODING), soft);
        if (strlen(digits) >= 10) {
            char wsc[8];
            strncpy(wsc, digits + 5, 5);
            wsc[5] = '\0';
            SetWindowTextA(GetDlgItem(hwnd, IDC_WSC), wsc);
        }
        Log("Codificacion actual: %s", digits);
    } else {
        Log("El modulo no devolvio codificacion.");
    }
}

/* Escribe la codificacion con red de seguridad:
   1. lee y guarda el valor actual en respaldos-codificacion.txt
   2. pide confirmacion explicita en una ventana
   3. solo entonces envia el comando 07 */
static void WriteCoding(HWND hwnd)
{
    char soft[8] = "00000";
    char wsc[8]  = "12345";
    char digits[24];
    char original[32];
    char pregunta[320];
    FILE *f;

    if (!g_link.open) { Log("Sin sesion activa."); return; }
    GetWindowTextA(GetDlgItem(hwnd, IDC_CODING), soft, sizeof(soft));
    GetWindowTextA(GetDlgItem(hwnd, IDC_WSC), wsc, sizeof(wsc));

    if (!kwpReadCoding(&g_link, original, sizeof(original))) {
        Log("No se pudo leer la codificacion actual: no se escribe nada.");
        MessageBoxA(hwnd, "Sin lectura previa no hay respaldo, y sin respaldo no se escribe.",
                    "DCTT - escritura bloqueada", MB_ICONWARNING | MB_OK);
        return;
    }
    f = fopen("respaldos-codificacion.txt", "a");
    if (f) {
        fprintf(f, "modulo 0x%02X; original; %s\r\n", g_link.ecu, original);
        fclose(f);
        Log("Respaldo guardado: %s", original);
    } else {
        Log("No se pudo escribir el respaldo: escritura cancelada.");
        MessageBoxA(hwnd, "No se pudo guardar respaldos-codificacion.txt. Se cancela la escritura.",
                    "DCTT - escritura bloqueada", MB_ICONERROR | MB_OK);
        return;
    }

    sprintf(digits, "%s%s000", soft, wsc);
    sprintf(pregunta,
            "Modulo 0x%02X\n\nCodificacion actual: %s\nNueva codificacion:  %s\nWSC: %s\n\n"
            "Se ha guardado el valor original en respaldos-codificacion.txt.\n\nEscribir ahora?",
            g_link.ecu, original, soft, wsc);
    if (MessageBoxA(hwnd, pregunta, "DCTT - confirmar recode",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        Log("Recode cancelado por el usuario.");
        return;
    }

    Log("Enviando recode 07: %s", digits);
    if (kwpWriteCoding(&g_link, digits)) Log("Recode aceptado por el modulo.");
    else                                Log("Recode rechazado: vuelve a escribir %s", original);
}

static void ReadGroup(HWND hwnd)
{
    char text[8] = "001";
    char fields[4][24];
    int  group, i;

    if (!g_link.open) { Log("Sin sesion activa."); return; }
    GetWindowTextA(GetDlgItem(hwnd, IDC_GROUP), text, sizeof(text));
    group = atoi(text);
    if (group < 1 || group > 255) { Log("Grupo fuera de rango."); return; }

    if (kwpReadMeasuringGroup(&g_link, (BYTE)group, fields)) {
        for (i = 0; i < 4; i++) SetWindowTextA(GetDlgItem(hwnd, IDC_FIELD_BASE + i), fields[i]);
        Log("Grupo %03d leido.", group);
    } else {
        Log("El modulo no devolvio el grupo %03d.", group);
    }
}

static void AutoScanAll(HWND hwnd)
{
    static char report[8192];

    (void)hwnd;
    Log("--- Escaneo completo 0x01-0x7F ---");
    if (kwpAutoScan(&g_link, g_link.portName, 0x01, 0x7F, report, sizeof(report))) Log("%s", report);
    else Log("Ningun modulo respondio. Revisa contacto y puerto.");
}

static void ReadAdaptation(HWND hwnd)
{
    char channel[8] = "001";
    char text[64];

    if (!g_link.open) { Log("Sin sesion activa."); return; }
    GetWindowTextA(GetDlgItem(hwnd, IDC_CHANNEL), channel, sizeof(channel));
    if (kwpReadAdaptation(&g_link, (BYTE)atoi(channel), text, sizeof(text))) {
        SetWindowTextA(GetDlgItem(hwnd, IDC_VALUE), text);
        Log("Adaptacion canal %s: %s", channel, text);
    } else {
        Log("El modulo no devolvio el canal %s.", channel);
    }
}

static void WriteAdaptation(HWND hwnd)
{
    char channel[8] = "001";
    char value[8] = "00000";

    if (!g_link.open) { Log("Sin sesion activa."); return; }
    GetWindowTextA(GetDlgItem(hwnd, IDC_CHANNEL), channel, sizeof(channel));
    GetWindowTextA(GetDlgItem(hwnd, IDC_VALUE), value, sizeof(value));
    Log("Adaptacion canal %s -> %s", channel, value);
    if (kwpWriteAdaptation(&g_link, (BYTE)atoi(channel), atoi(value))) Log("Valor aceptado.");
    else Log("Valor rechazado: hace falta login o el canal no existe.");
}

static void ExportLog(HWND hwnd)
{
    char  path[MAX_PATH];
    char  text[8192];
    FILE *f;

    (void)hwnd;
    sprintf(path, "dctt-registro.txt");
    GetWindowTextA(g_log, text, sizeof(text));
    f = fopen(path, "a");
    if (!f) { Log("No se pudo escribir el registro."); return; }
    fprintf(f, "%s\r\n", text);
    fclose(f);
    Log("Registro guardado en %s", path);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HDC dc = GetDC(hwnd);
        int i;
        HWND combo;

        g_log = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                              ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                              12, 452, 690, 130, hwnd, (HMENU)IDC_LOG, NULL, NULL);
        SendMessageA(g_log, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);

        CreateWindowA("STATIC", "Puerto:", WS_CHILD | WS_VISIBLE, 12, 16, 50, 18, hwnd, NULL, NULL, NULL);
        combo = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                              70, 12, 80, 120, hwnd, (HMENU)IDC_PORT, NULL, NULL);
        FillPorts(combo);
        CreateWindowA("BUTTON", "Test", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      160, 12, 70, 24, hwnd, (HMENU)IDC_TEST, NULL, NULL);

        CreateWindowA("BUTTON", "Modulos del TT 8N", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      12, 44, 330, 100, hwnd, NULL, NULL, NULL);
        for (i = 0; i < 5; i++) {
            CreateWindowA("BUTTON", g_modNames[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          24 + (i % 3) * 104, 68 + (i / 3) * 30, 100, 26,
                          hwnd, (HMENU)(IDC_ECU_BASE + i), NULL, NULL);
        }

        CreateWindowA("BUTTON", "Averias (02 / 05)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      12, 152, 330, 150, hwnd, NULL, NULL, NULL);
        CreateWindowA("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                      24, 176, 306, 88, hwnd, (HMENU)IDC_DTC_LIST, NULL, NULL);
        CreateWindowA("BUTTON", "Leer", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      24, 270, 80, 24, hwnd, (HMENU)IDC_DTC_READ, NULL, NULL);
        CreateWindowA("BUTTON", "Borrar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      112, 270, 80, 24, hwnd, (HMENU)IDC_DTC_CLEAR, NULL, NULL);

        CreateWindowA("BUTTON", "Codificacion (07)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      12, 310, 330, 120, hwnd, NULL, NULL, NULL);
        CreateWindowA("STATIC", "Soft coding:", WS_CHILD | WS_VISIBLE, 24, 336, 78, 18, hwnd, NULL, NULL, NULL);
        CreateWindowA("EDIT", "00000", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                      108, 334, 60, 22, hwnd, (HMENU)IDC_CODING, NULL, NULL);
        CreateWindowA("STATIC", "WSC:", WS_CHILD | WS_VISIBLE, 180, 336, 40, 18, hwnd, NULL, NULL, NULL);
        CreateWindowA("EDIT", "12345", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                      220, 334, 60, 22, hwnd, (HMENU)IDC_WSC, NULL, NULL);
        CreateWindowA("BUTTON", "Leer", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      24, 392, 80, 24, hwnd, (HMENU)IDC_CODE_READ, NULL, NULL);
        CreateWindowA("BUTTON", "Escribir", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      112, 392, 80, 24, hwnd, (HMENU)IDC_CODE_WRITE, NULL, NULL);

        CreateWindowA("BUTTON", "Bloques de medicion (08)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      356, 44, 346, 170, hwnd, NULL, NULL, NULL);
        CreateWindowA("STATIC", "Grupo:", WS_CHILD | WS_VISIBLE, 368, 70, 46, 18, hwnd, NULL, NULL, NULL);
        CreateWindowA("EDIT", "001", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                      418, 68, 50, 22, hwnd, (HMENU)IDC_GROUP, NULL, NULL);
        CreateWindowA("BUTTON", "Leer grupo", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      478, 66, 90, 24, hwnd, (HMENU)IDC_GROUP_READ, NULL, NULL);
        for (i = 0; i < 4; i++) {
            CreateWindowA("STATIC", "---", WS_CHILD | WS_VISIBLE | SS_SUNKEN,
                          368 + i * 82, 108, 76, 26, hwnd, (HMENU)(IDC_FIELD_BASE + i), NULL, NULL);
        }
        CreateWindowA("STATIC", "Los valores se muestran en bruto: la escala la fija el byte de formato del bloque.",
                      WS_CHILD | WS_VISIBLE, 368, 146, 320, 40, hwnd, NULL, NULL, NULL);

        CreateWindowA("BUTTON", "Pruebas de actuadores (03)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      356, 224, 346, 78, hwnd, NULL, NULL, NULL);
        CreateWindowA("BUTTON", "Iniciar secuencia", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      368, 250, 120, 26, hwnd, (HMENU)IDC_TEST_START, NULL, NULL);
        CreateWindowA("BUTTON", "Siguiente", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      498, 250, 100, 26, hwnd, (HMENU)IDC_TEST_NEXT, NULL, NULL);

        CreateWindowA("BUTTON", "Avanzado", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      356, 310, 346, 130, hwnd, NULL, NULL, NULL);
        CreateWindowA("BUTTON", "Escaneo completo", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      368, 334, 120, 24, hwnd, (HMENU)IDC_AUTOSCAN, NULL, NULL);
        CreateWindowA("STATIC", "Canal:", WS_CHILD | WS_VISIBLE, 500, 338, 40, 18, hwnd, NULL, NULL, NULL);
        CreateWindowA("EDIT", "001", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                      542, 334, 40, 22, hwnd, (HMENU)IDC_CHANNEL, NULL, NULL);
        CreateWindowA("EDIT", "00000", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                      590, 334, 60, 22, hwnd, (HMENU)IDC_VALUE, NULL, NULL);
        CreateWindowA("BUTTON", "Leer adapt.", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      368, 366, 90, 24, hwnd, (HMENU)IDC_ADAPT_READ, NULL, NULL);
        CreateWindowA("BUTTON", "Escribir adapt.", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      464, 366, 100, 24, hwnd, (HMENU)IDC_ADAPT_WRITE, NULL, NULL);
        CreateWindowA("BUTTON", "Ajuste basico", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      572, 366, 100, 24, hwnd, (HMENU)IDC_BASIC, NULL, NULL);
        CreateWindowA("BUTTON", "Login 16", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      368, 398, 90, 24, hwnd, (HMENU)IDC_LOGIN, NULL, NULL);
        CreateWindowA("BUTTON", "Exportar registro", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      464, 398, 120, 24, hwnd, (HMENU)IDC_EXPORT, NULL, NULL);

        CreateWindowA("STATIC", "Registro", WS_CHILD | WS_VISIBLE, 12, 430, 100, 18, hwnd, NULL, NULL, NULL);

        ReleaseDC(hwnd, dc);
        Log("DCTT listo. Contacto puesto, motor parado, cable KKL 409.1 en el conector.");
        ReadDtcDatabase();
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= IDC_ECU_BASE && id < IDC_ECU_BASE + 5) {
            ConnectModule(hwnd, g_modules[id - IDC_ECU_BASE]);
            return 0;
        }
        switch (id) {
        case IDC_TEST: {
            char port[24];
            PortName(hwnd, port, sizeof(port));
            if (kwpOpen(&g_link, port, 9600)) {
                Log("Puerto %s abierto a 9600 bps, 8N1, sin flujo.", port);
                kwpClose(&g_link);
            } else {
                Log("No se pudo abrir %s. Revisa el driver FTDI y el numero de COM.", port);
            }
            return 0;
        }
        case IDC_DTC_READ:  ReadDtc(hwnd); return 0;
        case IDC_DTC_CLEAR:
            if (g_link.open && kwpClearDtc(&g_link)) {
                SendMessageA(GetDlgItem(hwnd, IDC_DTC_LIST), LB_RESETCONTENT, 0, 0);
                Log("Memoria de averias borrada. Vuelve a leer para confirmar.");
            } else {
                Log("No se pudo borrar la memoria de averias.");
            }
            return 0;
        case IDC_CODE_READ:  ReadCoding(hwnd); return 0;
        case IDC_CODE_WRITE: WriteCoding(hwnd); return 0;
        case IDC_GROUP_READ: ReadGroup(hwnd); return 0;
        case IDC_TEST_START:
            if (!g_link.open) { Log("Sin sesion activa."); return 0; }
            g_testStep = 1;
            Log(kwpOutputTest(&g_link, (BYTE)g_testStep) ? "Prueba de salida 1 activa." : "El modulo rechazo la prueba.");
            return 0;
        case IDC_TEST_NEXT:
            if (!g_link.open) { Log("Sin sesion activa."); return 0; }
            g_testStep++;
            Log(kwpOutputTest(&g_link, (BYTE)g_testStep) ? "Siguiente salida activa." : "Fin de la secuencia.");
            return 0;
        case IDC_AUTOSCAN:    AutoScanAll(hwnd); return 0;
        case IDC_ADAPT_READ:  ReadAdaptation(hwnd); return 0;
        case IDC_ADAPT_WRITE: WriteAdaptation(hwnd); return 0;
        case IDC_BASIC:
            if (g_link.open && kwpBasicSettings(&g_link, (BYTE)atoi("060"))) Log("Ajuste basico del grupo 060 ejecutado.");
            else Log("El modulo rechazo el ajuste basico.");
            return 0;
        case IDC_LOGIN:
            if (g_link.open && kwpLogin(&g_link, 0)) Log("Login enviado (codigo 00000). Cambia el valor en el codigo para tu modulo.");
            else Log("Login rechazado.");
            return 0;
        case IDC_EXPORT:      ExportLog(hwnd); return 0;
        default:
            return 0;
        }
    }

    case WM_DESTROY:
        kwpEndSession(&g_link);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdLine, int show)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;

    (void)previous;
    (void)cmdLine;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = "DCTT_WINDOW";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon         = LoadIconA(instance, MAKEINTRESOURCEA(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIconA(instance, MAKEINTRESOURCEA(1));
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;   /* XP sin icono pequeno */
    RegisterClassA(&wc);

    hwnd = CreateWindowA("DCTT_WINDOW", "DCTT - Audi TT 8N - KKL 409.1",
                         WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                         CW_USEDEFAULT, CW_USEDEFAULT, 730, 640,
                         NULL, NULL, instance, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
