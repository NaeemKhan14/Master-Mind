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

#define LEDR 13
#define LED 5
#define BUTTON 19
#define STRB_PIN 24
#define RS_PIN   25
#define DATA0_PIN 23
#define DATA1_PIN 10
#define DATA2_PIN 27
#define DATA3_PIN 22
#define DELAY 200

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
#define	PI_GPIO_MASK	(0xFFFFFFC0)

#define	LCD_FUNC_F	0x04
#define	LCD_FUNC_N	0x08
#define	LCD_FUNC_DL	0x10

#define	LCD_CDSHIFT_RL	0x04
#define	LCD_BLINK_CTRL		0x01
#define	LCD_CURSOR_CTRL		0x02
#define	LCD_DISPLAY_CTRL	0x04

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

static volatile unsigned int gpiobase ;
static volatile uint32_t *gpio ;

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

struct lcdDataStruct
{
  int bits, rows, cols ;
  int rsPin, strbPin ;
  int dataPins [8] ;
  int cx, cy ;
};

static int lcdControl;

void pinMode(volatile uint32_t *gpio , int pin ,int state) {

    int fSel = (pin/10)*4;  //finds the fsel register
    int shift= (pin%10)*3;  //finds the position in the calculated register 
    asm volatile(
      "\tLDR R1, %[gpio]\n"     //loads gpio
      "\tADD R0, R1, %[fSel]\n"  
      "\tLDR R1, [R0, #0]\n"    
      "\tMOV R2, #0b111\n"
      "\tLSL R2, %[shift]\n"
      "\tBIC R1, R1, R2\n"
      "\tMOV R2, #1\n"
      "\tLSL R2, %[shift]\n"
      "\tORR R1, R2\n"
      "\tSTR R1, [R0, #0]\n"
      :
      : [fSel] "r" (fSel) 
      , [gpio] "m" (gpio)
      , [shift] "r" (shift)
      : "r0", "r1", "r2", "cc");
}    
void digitalWrite(volatile uint32_t *gpio, int pin, int theValue) {
  
	int off;
	
	if(pin > 31) {
		off = (theValue == LOW) ? 11 : 8;
	} else {
		off = (theValue == LOW) ? 10 : 7;
	}    
	asm volatile (	     
		"\tLDR R0, %[gpio]\n"
		"\tADD R0, R0, %[off]\n"   // replace with off
		"\tMOV R2, #1\n"
		"\tMOV R1, %[act]\n"
		"\tAND R1, #31\n"     
		"\tLSL R2, R1\n"
		"\tSTR R2, [R0, #0]\n"
		: 
		: [gpio] "m" (gpio)
		, [act] "r" (pin)   //pin number
		, [off] "r" (off*4)
		: "r0", "r1", "r2", "cc");

}

int readPin(volatile uint32_t *gpio, int pin) {
	
	int off=0,res=0;
	
	if(pin > 31) {
		off =  14 ;
	} else {
		off = 13;
	}  
	asm volatile (	     
		"\tLDR R0, %[gpio]\n"
		"\tADD R0, R0, %[off]\n"
		"\tLDR R3, [R0]\n" 
		"\tMOV R2, #1\n"
		"\tMOV R1, %[pin]\n"
		"\tAND R1, #31\n"     
		"\tLSL R2, R1\n"
		"\tAND R3, R2\n"
		"\tMOV %[res], R3\n"
		: [res] "=r" (res)
		: [gpio] "m" (gpio)
		, [pin] "r" (pin)
		, [off] "r" (off*4)
		: "r0", "r1", "r2", "r3", "cc");
	return res;
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

void delay (unsigned int howLong)
{
  struct timespec sleeper, dummy ;

  sleeper.tv_sec  = (time_t)(howLong / 1000) ;
  sleeper.tv_nsec = (long)(howLong % 1000) * 1000000 ;

  nanosleep (&sleeper, &dummy) ;
}

void strobe (const struct lcdDataStruct *lcd)
{
  digitalWrite (gpio, lcd->strbPin, 1) ; delayMicroseconds (50) ;
  digitalWrite (gpio, lcd->strbPin, 0) ; delayMicroseconds (50) ;
}

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

void lcdHome (struct lcdDataStruct *lcd)
{
  lcdPutCommand (lcd, LCD_HOME) ;
  lcd->cx = lcd->cy = 0 ;
  delay (5) ;
}

void lcdClear (struct lcdDataStruct *lcd)
{
  
  lcdPutCommand (lcd, LCD_CLEAR) ;
  lcdPutCommand (lcd, LCD_HOME) ;
  lcd->cx = lcd->cy = 0 ;
  delay (5);
  
}

void lcdPosition (struct lcdDataStruct *lcd, int x, int y)
{
  if ((x > lcd->cols) || (x < 0))
    return ;
  if ((y > lcd->rows) || (y < 0))
    return ;

  lcdPutCommand (lcd, x + (LCD_DGRAM | (y>0 ? 0x40 : 0x00))) ;

  lcd->cx = x ;
  lcd->cy = y ;
}

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

void lcdPutchar (struct lcdDataStruct *lcd, unsigned char data)
{
  digitalWrite (gpio, lcd->rsPin, 1) ;
  sendDataCmd  (lcd, data) ;

  if (++lcd->cx == lcd->cols)
  {
    lcd->cx = 0 ;
    if (++lcd->cy == lcd->rows)
      lcd->cy = 0 ;
    
    lcdPutCommand (lcd, lcd->cx + (LCD_DGRAM | (lcd->cy>0 ? 0x40 : 0x00))) ;
  }
}

void lcdPuts (struct lcdDataStruct *lcd, const char *string)
{
  while (*string)
    lcdPutchar (lcd, *string++) ;
}
/*
 * This function turns the given LED on, then waits until 300ms before
 * turning it off again. It runs @count number of times.
 */
void bling (int pin, int count) {
  int y;
  
  for(y = 0; y < count; y++){
    digitalWrite(gpio, pin, 1);
    delay(300);
    digitalWrite(gpio, pin, 0);
    delay(200);
  }
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
          if (readPin(gpio,19) != 0) {
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
    bling(LED,1);
    bling(LEDR,count);
}
    bling(LED,2);
  return guess;
}
/*
 * This function compares the secret and input values. It returns the 
 * result as a 3 value array. result[0] is the success and fail flag. 0
 * means the guess was wrong, 1 means the guess was right. result[1] is
 * the number of correct guesses where index[x] == secret[x]. result[2]
 * means the number of input values which are in secret sequence but not
 * in the right order.
 */
int *compare(int * secret, int *userInput, int length) {

  static int result[3];
  int correctNumber = 0, positionMatch = 0;
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
    if(userInput[x] == secret[x]) {
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
        if ((secret[y] == userInput[x]) && !forgetSecret[y]) {
          if (!found) {
            correctNumber++;
            found = 1;
          }
          forgetSecret[y] = 1;
        }
      }
    }
  }
  /* If correct guesses at correct positions are the same as number
   * of length, that means we have guessed all the colors correctly.
   * So in that csae, result[0] is set to 1 to be returned.
   */
  (positionMatch == length) ? result[0] = 1 : 0;
  
  result[1] = positionMatch;
  result[2] = correctNumber;
  
  bling(LEDR, positionMatch);
  bling(LED,1);
  
  bling(LEDR, correctNumber);
  
  return result;
}
/*
 * Converts an integer into a string. It returns a static variable 
 * (because it is a local variable that we need the value of later in 
 * code). The length of it is 2. One is to store the converted char, the
 * second is the string terminator. It returns a string because it will
 * be used with strcat which requires this to be returned as a string.
 */
char *intToString(int value) {

  static char tempString[2];
  sprintf(tempString, "%d", value);
  tempString[1] = '\0';

  return tempString;
}

/* Main ----------------------------------------------------------------------------- */
int main (int argc, char **argv)
{
  int pinLED = LED, pinButton = BUTTON,pinLEDR = LEDR;
  int fSel, shift, pin,  clrOff, setOff, off, fd,j;
  int theValue, thePin;
  unsigned int howLong = DELAY;
  uint32_t res;
  
  if (geteuid () != 0)
    fprintf (stderr, "setup: Must be root. (Did you forget sudo?)\n") ;

  // constants for RPi2
  gpiobase = 0x3F200000 ;

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
  pinMode(gpio, LED, 1);
  pinMode(gpio, LEDR, 1);
  
  struct lcdDataStruct *lcd ;
  int bits, rows, cols, i ;
  unsigned char func ;
  struct tm *t ;
  time_t tim ;
  char buf [32] ;
  // hard-coded: 16x2 display, using a 4-bit connection
  bits = 4; 
  cols = 16; 
  rows = 2; 
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
  // -----------------------------------------------------------------------------
  // Initial Welcome screen
  lcdPosition (lcd, 0, 0) ; lcdPuts (lcd, "MasterMind") ;
  lcdPosition (lcd, 0, 1) ; lcdPuts (lcd, "") ;
  int length, numRange, random;
  // Take length and range from user
  printf("Please enter the length\n");
  scanf("%d", &length);
  
  printf("Please enter the numRange\n");
  scanf("%d", &numRange);
  
  /*
   * We are using uninitialized integer as seed for random because this
   * will point to a random location in the memory everytime, so our
   * secret sequence will be unique. 
   */
  int randSeed;
  srand(randSeed);
  int secret[length];
  
  // Populate the secret values
  for(j = 0; j < length; j++) {
    secret[j]=rand()%numRange + 1;
    // If debug mode param is present, display secret
    if(argc == 2 && argv[1][0] == 'd') {
      if(j == 0) { printf("Secret: "); }
      printf("%d\t", secret[j]);
    }
  }
  
  printf("\nStart pressing the button\n");

  int success = 0, tries = 0;
  // Keep running the loop until termination flag is received.
  while (success != 1) {
    
    lcdClear(lcd);
    lcdPosition (lcd, 0, 0) ; lcdPuts (lcd, "Starting") ;
    
    // Process the user input and store it here.
    int *userInput = input(length, numRange);

    char resultStringTop[13] = "";
    tries++;
    
    // Compile the string for the top line of LCD 
    int *result = compare(secret, userInput, length);
    
    // Compile the first line of game to be displayed on LCD
    strcat(resultStringTop, "Guess ");
    strcat(resultStringTop, intToString(tries));
    strcat(resultStringTop, ": ");
    strcat(resultStringTop, intToString(result[1]));
    strcat(resultStringTop, " ");
    strcat(resultStringTop, intToString(result[2]));
    resultStringTop[12] = '\0';
    
    // Compile the string for the bottom line of LCD
    char *resultStringBottom = malloc(length * sizeof(char));
    
    // Compile the user input to be displayed on LCD
    for(j = 0; j < length; j++) {
      strcat(resultStringBottom, intToString(userInput[j]));
      strcat(resultStringBottom, " ");
    }
    
    // If user guessed the sequence correctly
    if(result[0] == 1) {
      // Display the success message
      success = 1;
      lcdClear(lcd);
      lcdPosition (lcd, 0, 0) ; lcdPuts (lcd, resultStringTop) ;
      lcdPosition (lcd, 0, 1) ; lcdPuts (lcd, resultStringBottom) ;
      delay(3000);
      
      char attempts[13] = "Attempts = ";
      strcat(attempts, intToString(tries));
      attempts[12] = '\0';
        
      lcdClear(lcd);
      lcdPosition (lcd, 0, 0) ; lcdPuts (lcd, "Success") ;
      lcdPosition (lcd, 0, 1) ; lcdPuts (lcd, attempts) ;
      
      printf("Game finished in %d attempts\n", tries);
        
      digitalWrite(gpio, LED, 1);
      bling(LEDR, 3);
      digitalWrite(gpio, LED, 0);
      break;
    }
    // If the user guess is wrong, update the LCD output and blink LED
    lcdClear(lcd);
    lcdPosition (lcd, 0, 0) ; lcdPuts (lcd, resultStringTop) ;
    lcdPosition (lcd, 0, 1) ; lcdPuts (lcd, resultStringBottom) ;
    delay(3000);

    bling(LED,3);
  }
}




