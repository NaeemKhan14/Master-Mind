#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

// original code based in wiringPi library by Gordon Henderson
// #include "wiringPi.h"

// =======================================================
// Tunables
// PINs (based on BCM numbering)
#define LED 13
#define LEDR 5
#define BUTTON 19
// delay for loop iterations (mainly), in ms
#define DELAY 200
// =======================================================

#ifndef	TRUE
#define	TRUE	(1==1)
#define	FALSE	(1==2)
#endif

#define	PAGE_SIZE		(4*1024)
#define	BLOCK_SIZE		(4*1024)

#define	INPUT			 0
#define	OUTPUT			 1

#define	LOW			 0
#define	HIGH			 1

static volatile unsigned int gpiobase ;
static volatile uint32_t *gpio ;


// Mask for the bottom 64 pins which belong to the Raspberry Pi
//	The others are available for the other devices

#define	PI_GPIO_MASK	(0xFFFFFFC0)

/* ------------------------------------------------------- */
#define STRB_PIN 24
#define RS_PIN   25
#define DATA0_PIN 23
#define DATA1_PIN 10
#define DATA2_PIN 27
#define DATA3_PIN 22

// char data for the CGRAM, i.e. defining new characters for the display

static unsigned char newChar [8] = 
{
  0b11111,
  0b10001,
  0b10001,
  0b10101,
  0b11111,
  0b10001,
  0b10001,
  0b11111,
} ;

/* bit pattern to feed into lcdCharDef to define a new character */

static unsigned char hawoNewChar [8] = 
{
  0b11111,
  0b10001,
  0b10001,
  0b10001,
  0b10001,
  0b10001,
  0b10001,
  0b11111,
} ;

// data structure holding data on the representation of the LCD
struct lcdDataStruct
{
  int bits, rows, cols ;
  int rsPin, strbPin ;
  int dataPins [8] ;
  int cx, cy ;
} ;

static int lcdControl ;

/* ***************************************************************************** */
/* INLINED fcts from wiringPi/devLib/lcd.c: */
// HD44780U Commands (see Fig 11, p28 of the Hitachi HD44780U datasheet)

#define	LCD_CLEAR	0x01
#define	LCD_HOME	0x02
#define	LCD_ENTRY	0x04
#define	LCD_CTRL	0x08
#define	LCD_CDSHIFT	0x10
#define	LCD_FUNC	0x20
#define	LCD_CGRAM	0x40
#define	LCD_DGRAM	0x80

// Bits in the entry register

#define	LCD_ENTRY_SH		0x01
#define	LCD_ENTRY_ID		0x02

// Bits in the control register

#define	LCD_BLINK_CTRL		0x01
#define	LCD_CURSOR_CTRL		0x02
#define	LCD_DISPLAY_CTRL	0x04

// Bits in the function register

#define	LCD_FUNC_F	0x04
#define	LCD_FUNC_N	0x08
#define	LCD_FUNC_DL	0x10

#define	LCD_CDSHIFT_RL	0x04

int failure (int fatal, const char *message, ...);
void waitForEnter (void);

/* ------------------------------------------------------- */
/* low-level interface to the hardware */

/* digitalWrite is now in a separate file: lcdBinary.c */
void digitalWrite (uint32_t *gpio, int pin, int value);

void waitForEnter (void)
{
  printf ("Press ENTER to continue: ") ;
  (void)fgetc (stdin) ;
}


void strobe (const struct lcdDataStruct *lcd)
{

  // Note timing changes for new version of delayMicroseconds ()
  digitalWrite (gpio, lcd->strbPin, 1) ; delayMicroseconds (50) ;
  digitalWrite (gpio, lcd->strbPin, 0) ; delayMicroseconds (50) ;
}

/*
 * sentDataCmd:
 *	Send an data or command byte to the display.
 *********************************************************************************
 */

void sendDataCmd (const struct lcdDataStruct *lcd, unsigned char data)
{
  register unsigned char myData = data ;
  unsigned char          i, d4 ;

  if (lcd->bits == 4)
  {
    d4 = (myData >> 4) & 0x0F;
    for (i = 0 ; i < 4 ; ++i)
    {
      digitalWrite (gpio, lcd->dataPins [i], (d4 & 1)) ;
      d4 >>= 1 ;
    }
    strobe (lcd) ;

    d4 = myData & 0x0F ;
    for (i = 0 ; i < 4 ; ++i)
    {
      digitalWrite (gpio, lcd->dataPins [i], (d4 & 1)) ;
      d4 >>= 1 ;
    }
  }
  else
  {
    for (i = 0 ; i < 8 ; ++i)
    {
      digitalWrite (gpio, lcd->dataPins [i], (myData & 1)) ;
      myData >>= 1 ;
    }
  }
  strobe (lcd) ;
}

/*
 * lcdPutCommand:
 *	Send a command byte to the display
 *********************************************************************************
 */

void lcdPutCommand (const struct lcdDataStruct *lcd, unsigned char command)
{
#ifdef DEBUG
 // fprintf(stderr, "lcdPutCommand: digitalWrite(%d,%d) and sendDataCmd(%d,%d)\n", lcd->rsPin,   0, lcd, command);
#endif
  digitalWrite (gpio, lcd->rsPin,   0) ;
  sendDataCmd  (lcd, command) ;
  delay (2) ;
}

void lcdPut4Command (const struct lcdDataStruct *lcd, unsigned char command)
{
  register unsigned char myCommand = command ;
  register unsigned char i ;

  digitalWrite (gpio, lcd->rsPin,   0) ;

  for (i = 0 ; i < 4 ; ++i)
  {
    digitalWrite (gpio, lcd->dataPins [i], (myCommand & 1)) ;
    myCommand >>= 1 ;
  }
  strobe (lcd) ;
}

/*
 * lcdHome: lcdClear:
 *	Home the cursor or clear the screen.
 *********************************************************************************
 */

void lcdHome (struct lcdDataStruct *lcd)
{
#ifdef DEBUG
  //f(stderr, "lcdHome: lcdPutCommand(%d,%d)\n", lcd, LCD_HOME);
#endif
  lcdPutCommand (lcd, LCD_HOME) ;
  lcd->cx = lcd->cy = 0 ;
  delay (5) ;
}

void lcdClear (struct lcdDataStruct *lcd)
{
#ifdef DEBUG
 // fprintf(stderr, "lcdClear: lcdPutCommand(%d,%d) and lcdPutCommand(%d,%d)\n", lcd, LCD_CLEAR, lcd, LCD_HOME);
#endif
  lcdPutCommand (lcd, LCD_CLEAR) ;
  lcdPutCommand (lcd, LCD_HOME) ;
  lcd->cx = lcd->cy = 0 ;
  delay (5) ;
}

/*
 * lcdPosition:
 *	Update the position of the cursor on the display.
 *	Ignore invalid locations.
 *********************************************************************************
 */

void lcdPosition (struct lcdDataStruct *lcd, int x, int y)
{
  // struct lcdDataStruct *lcd = lcds [fd] ;

  if ((x > lcd->cols) || (x < 0))
    return ;
  if ((y > lcd->rows) || (y < 0))
    return ;

  lcdPutCommand (lcd, x + (LCD_DGRAM | (y>0 ? 0x40 : 0x00)  /* rowOff [y] */  )) ;

  lcd->cx = x ;
  lcd->cy = y ;
}


void delayMicroseconds (unsigned int howLong)
{
  struct timespec sleeper ;
  unsigned int uSecs = howLong % 1000000 ;
  unsigned int wSecs = howLong / 1000000 ;

  /**/ if (howLong ==   0)
    return ;
#if 0
  else if (howLong  < 100)
    delayMicrosecondsHard (howLong) ;
#endif
  else
  {
    sleeper.tv_sec  = wSecs ;
    sleeper.tv_nsec = (long)(uSecs * 1000L) ;
    nanosleep (&sleeper, NULL) ;
  }
}

/*
 * lcdDisplay: lcdCursor: lcdCursorBlink:
 *	Turn the display, cursor, cursor blinking on/off
 *********************************************************************************
 */

void lcdDisplay (struct lcdDataStruct *lcd, int state)
{
  if (state)
    lcdControl |=  LCD_DISPLAY_CTRL ;
  else
    lcdControl &= ~LCD_DISPLAY_CTRL ;

  lcdPutCommand (lcd, LCD_CTRL | lcdControl) ; 
}

void lcdCursor (struct lcdDataStruct *lcd, int state)
{
  if (state)
    lcdControl |=  LCD_CURSOR_CTRL ;
  else
    lcdControl &= ~LCD_CURSOR_CTRL ;

  lcdPutCommand (lcd, LCD_CTRL | lcdControl) ; 
}

void lcdCursorBlink (struct lcdDataStruct *lcd, int state)
{
  if (state)
    lcdControl |=  LCD_BLINK_CTRL ;
  else
    lcdControl &= ~LCD_BLINK_CTRL ;

  lcdPutCommand (lcd, LCD_CTRL | lcdControl) ; 
}

/*
 * lcdPutchar:
 *	Send a data byte to be displayed on the display. We implement a very
 *	simple terminal here - with line wrapping, but no scrolling. Yet.
 *********************************************************************************
 */

void lcdPutchar (struct lcdDataStruct *lcd, unsigned char data)
{
  digitalWrite (gpio, lcd->rsPin, 1) ;
  sendDataCmd  (lcd, data) ;

  if (++lcd->cx == lcd->cols)
  {
    lcd->cx = 0 ;
    if (++lcd->cy == lcd->rows)
      lcd->cy = 0 ;
    
    // TODO: inline computation of address and eliminate rowOff
    lcdPutCommand (lcd, lcd->cx + (LCD_DGRAM | (lcd->cy>0 ? 0x40 : 0x00)   /* rowOff [lcd->cy] */  )) ;
  }
}
/*
 * lcdPuts:
 *	Send a string to be displayed on the display
 *********************************************************************************
 */
void lcdPuts (struct lcdDataStruct *lcd, const char *string)
{
  while (*string)
    lcdPutchar (lcd, *string++) ;
}

/* ----------------------------------------------------------------------------- */

int LCDmain (char *string, char *string2)
{
  int i ;
  struct lcdDataStruct *lcd ;
  int bits, rows, cols ;
  unsigned char func ;

  struct tm *t ;
  time_t tim ;

  int   fd ;

  char buf [32] ;

  // hard-coded: 16x2 display, using a 4-bit connection
  bits = 4; 
  cols = 16; 
  rows = 2; 

  //printf ("Raspberry Pi LCD driver, for a %dx%d display (%d-bit wiring) \n", cols, rows, bits) ;

  if (geteuid () != 0)
    fprintf (stderr, "setup: Must be root. (Did you forget sudo?)\n") ;

  // -----------------------------------------------------------------------------
  // constants for RPi2
  gpiobase = 0x3F200000 ;

  // -----------------------------------------------------------------------------
  // memory mapping 
  // Open the master /dev/memory device

  if ((fd = open ("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC) ) < 0)
    return failure (FALSE, "setup: Unable to open /dev/mem: %s\n", strerror (errno)) ;

  // GPIO:
  gpio = (uint32_t *)mmap(0, BLOCK_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, gpiobase) ;
  if ((int32_t)gpio == -1)
    return failure (FALSE, "setup: mmap (GPIO) failed: %s\n", strerror (errno)) ;

  // ------
  // INLINED version of hawoLcdInit (can only deal with one LCD attached to the RPi):
  // Create a new LCD:
  lcd = (struct lcdDataStruct *)malloc (sizeof (struct lcdDataStruct)) ;
  if (lcd == NULL)
    return -1 ;

  // hard-wired GPIO pins
  lcd->rsPin   = RS_PIN ;
  lcd->strbPin = STRB_PIN ;
  lcd->bits    = 4 ;
  lcd->rows    = rows ;  // # of rows on the display
  lcd->cols    = cols ;  // # of cols on the display
  lcd->cx      = 0 ;     // x-pos of cursor
  lcd->cy      = 0 ;     // y-pos of curosr

  lcd->dataPins [0] = DATA0_PIN ;
  lcd->dataPins [1] = DATA1_PIN ;
  lcd->dataPins [2] = DATA2_PIN ;
  lcd->dataPins [3] = DATA3_PIN ;
  // lcd->dataPins [4] = d4 ;
  // lcd->dataPins [5] = d5 ;
  // lcd->dataPins [6] = d6 ;
  // lcd->dataPins [7] = d7 ;

  // lcds [lcdFd] = lcd ;

  digitalWrite (gpio, lcd->rsPin,   0) ; pinMode (gpio, lcd->rsPin,   OUTPUT) ;
  digitalWrite (gpio, lcd->strbPin, 0) ; pinMode (gpio, lcd->strbPin, OUTPUT) ;

  for (i = 0 ; i < bits ; ++i)
  {
    digitalWrite (gpio, lcd->dataPins [i], 0) ;
    pinMode      (gpio, lcd->dataPins [i], OUTPUT) ;
  }
  delay (35) ; // mS

  if (bits == 4)
  {
    func = LCD_FUNC | LCD_FUNC_DL ;			// Set 8-bit mode 3 times
    lcdPut4Command (lcd, func >> 4) ; delay (35) ;
    lcdPut4Command (lcd, func >> 4) ; delay (35) ;
    lcdPut4Command (lcd, func >> 4) ; delay (35) ;
    func = LCD_FUNC ;					// 4th set: 4-bit mode
    lcdPut4Command (lcd, func >> 4) ; delay (35) ;
    lcd->bits = 4 ;
  }
  else
  {
    failure(TRUE, "setup: only 4-bit connection supported\n");
    func = LCD_FUNC | LCD_FUNC_DL ;
    lcdPutCommand  (lcd, func     ) ; delay (35) ;
    lcdPutCommand  (lcd, func     ) ; delay (35) ;
    lcdPutCommand  (lcd, func     ) ; delay (35) ;
  }

  if (lcd->rows > 1)
  {
    func |= LCD_FUNC_N ;
    lcdPutCommand (lcd, func) ; delay (35) ;
  }

  // Rest of the initialisation sequence
  lcdDisplay     (lcd, TRUE) ;
  lcdCursor      (lcd, FALSE) ;
  lcdCursorBlink (lcd, FALSE) ;
  lcdClear       (lcd) ;

  lcdPutCommand (lcd, LCD_ENTRY   | LCD_ENTRY_ID) ;    // set entry mode to increment address counter after write
  lcdPutCommand (lcd, LCD_CDSHIFT | LCD_CDSHIFT_RL) ;  // set display shift to right-to-left
  // ------
  // -----------------------------------------------------------------------------
  // INLINED:
  //fprintf(stderr,"Printing welcome message ...\n");
  lcdPosition (lcd, 0, 0) ; lcdPuts (lcd, string) ;
  lcdPosition (lcd, 0, 1) ; lcdPuts (lcd, string2) ;

  //waitForEnter () ; // -------------------------------------------------------

  //lcdClear    (lcd) ;

  // TODO: print a longer message and make it scroll on the display

  return 0 ;
}

int failure (int fatal, const char *message, ...)
{
  va_list argp ;
  char buffer [1024] ;

  if (!fatal) //  && wiringPiReturnCodes)
    return -1 ;

  va_start (argp, message) ;
  vsnprintf (buffer, 1023, message, argp) ;
  va_end (argp) ;

  fprintf (stderr, "%s", buffer) ;
  exit (EXIT_FAILURE) ;

  return 0 ;
}

void delay (unsigned int howLong)
{
  struct timespec sleeper, dummy ;

  sleeper.tv_sec  = (time_t)(howLong / 1000) ;
  sleeper.tv_nsec = (long)(howLong % 1000) * 1000000 ;

  nanosleep (&sleeper, &dummy) ;
}

int *input(int length, int numRange) {
  int pinLED = LED, pinButton = BUTTON,pinLEDR = LEDR;
  int fSel, shift, pin,  clrOff, setOff, off;
  int y,x,j;
  int *guess = malloc(length * sizeof(int));

  for(x=0; x<length; x++){
    int count=0,prev=0;
    time_t startT = time(NULL);
    
    while((time(NULL)-startT) < numRange*1.5) {
      int curr=0;
      for (j=0; j<numRange*2; j++) {
          if ((*(gpio + 13) & (1 << (BUTTON & 31))) != 0) {
            curr=1;
            if(prev != curr){
              prev=1;
            
            if(count >= numRange) {
              count=numRange;
              break;
            } 
              count++;
              break;
            }
          } else {
            prev=0;
          }
          delay(200);
      }
    }
    
    guess[x] = count;
    
    if ((pinLED & 0xFFFFFFC0 /*PI_GPIO_MASK */) == 0) {
      setOff = 7; // GPSET0 for pin 23
      *(gpio + setOff) = 1 << (LED & 31) ;
      delay(500);
      clrOff = 10; // GPCLR0 for pin 23
      *(gpio + clrOff) = 1 << (LED & 31) ;
    }
    for(y=0; y<count; y++) {
      setOff = 7; // GPSET0 for pin 23
      *(gpio + setOff) = 1 << (LEDR & 31) ;
      delay(300);
      clrOff = 10; // GPCLR0 for pin 23
      *(gpio + clrOff) = 1 << (LEDR & 31) ;
      delay(300);
    }

}
// Clean up: write LOW

  *(gpio + 7) = 1 << (23 & 31) ;

  return guess;
}

int compare( int * secret, int *inp, int length){

  int result = 0, correctNumber = 0, positionMatch = 0;
  int x, y;
  int forgetSecret[length];
  int forgetInput[length];

  for(x = 0; x < length; x++) {
	forgetSecret[x] = 0;
	forgetInput[x] = 0;
  }
  /*
   * Visit x'th index of both input and secret arrays and mark the
   * values that match. That is the correct guessed values which
   * are at the right index in the array. We mark it to be never
   * used again.
   */
  for( x = 0; x < length; x++) {
    if(inp[x] == secret[x]) {
      positionMatch++;
      forgetSecret[x] = forgetInput[x] = 1;
    }
  }
  /*
   * First we check if the x is an index we didn't visit before.
   * If it isn't, we search for that value in the secret array.
   * If the value is matched and it wasn't marked before, the
   * program increments the correctMatches counter and sets a 
   * flag that no other values should be taken into account for
   * x'th element from input array.
   */
  for(x = 0; x < length; x++) {
    if(!forgetInput[x]) {
      int found = 0;

      for(y = 0; y < length; y++) {
        if ((secret[y] == inp[x]) && !forgetSecret[y]) {
          if (!found) {
	    correctNumber++;
	    found = 1;
	  }
          forgetSecret[y] = 1;
        }
      }
    }
  }
  printf("Total correct Matches = %d\n", correctNumber);
  printf("Total correct positions = %d\n", positionMatch);
  /* If correct guesses at correct positions are the same as number
   * of length, that means we have guessed all the colors correctly.
   * So in that csae, result is set to 1 to be returned.
   */

  return result;
}


/* Main ----------------------------------------------------------------------------- */
int main (void)
{
  int pinLED = LED, pinButton = BUTTON,pinLEDR = LEDR;
  int fSel, shift, pin,  clrOff, setOff, off, fd,j;
  int theValue, thePin;
  unsigned int howLong = DELAY;
  uint32_t res; /* testing only */

  printf ("Raspberry Pi button controlled LED (button in %d, led out %d)\n", BUTTON, LED) ;

  if (geteuid () != 0)
    fprintf (stderr, "setup: Must be root. (Did you forget sudo?)\n") ;

  // -----------------------------------------------------------------------------
  // constants for RPi2
  gpiobase = 0x3F200000 ;

  // -----------------------------------------------------------------------------
  // memory mapping 
  // Open the master /dev/memory device

  if ((fd = open ("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC) ) < 0)
    return failure (FALSE, "setup: Unable to open /dev/mem: %s\n", strerror (errno)) ;

  // GPIO:
  gpio = (uint32_t *)mmap(0, BLOCK_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, gpiobase) ;
  if ((int32_t)gpio == -1)
    return failure (FALSE, "setup: mmap (GPIO) failed: %s\n", strerror (errno)) ;

  // -----------------------------------------------------------------------------
  // setting the mode

  // controlling LED pin 23
  fSel =  2;    // GPIO 23 lives in register 2 (GPFSEL2)
  shift =  9;  // GPIO 23 sits in slot 3 of register 2, thus shift by 3*3 (3 bits per pin)
  // C version of setting LED
  *(gpio + fSel) = (*(gpio + fSel) & ~(7 << shift)) | (1 << shift);  // Sets bits to one = output

  // controlling button pin 24
  fSel =  2;    // GPIO 24 lives in register 2 (GPFSEL2)
  shift =  12;  // GPIO 24 sits in slot 4 of register 3, thus shift by 4*3 (3 bits per pin)
  // C version of mode input for button
  *(gpio + fSel) = *(gpio + fSel) & ~(7 << shift); // Sets bits to one = output

  // -----------------------------------------------------------------------------
  LCDmain("Master Mind", "");
  int length, numRange, random;
  printf("Please enter the length\n");
  scanf("%d", &length);
  
  printf("Please enter the numRange\n");
  scanf("%d", &numRange);
  
  int randSeed;
  srand(randSeed);
  printf("Secret: ");
  int secret[length];

  for(j = 0; j < length; j++) {
    secret[j]=rand()%numRange+1;
    printf("%d\t", secret[j]);
  }
  printf("\nStart pressing the button\n");

  int success = 0, tries = 0;

  while (success != 1) {

    int *inp = input(length, numRange);

    char *resultString = malloc(10 + length * sizeof(char));

    for(j = 0; j < length; j++) {
      char tempChar[2];
      sprintf(tempChar, "%d", inp[j]);
      tempChar[1] = '\0';
      strcat(resultString, tempChar);
      strcat(resultString, " ");
    }
    int x;

    int result = compare(secret, inp, length);
    if(result == 1) {
      success = 1;
      LCDmain("Success", "Correct Seq");
      break;
    }
    
    LCDmain("Incorrect Seq", resultString);
    delay(3000);
  }
}


