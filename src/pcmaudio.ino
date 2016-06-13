/*
 * speaker_pcm
 *
 * Plays 8-bit PCM audio on pin 11 using pulse-width modulation (PWM).
 * For Arduino with Atmega168 at 16 MHz.
 *
 * Uses two timers. The first changes the sample value 8000 times a second.
 * The second holds pin 11 high for 0-255 ticks out of a 256-tick cycle,
 * depending on sample value. The second timer repeats 62500 times per second
 * (16000000 / 256), much faster than the playback rate (8000 Hz), so
 * it almost sounds halfway decent, just really quiet on a PC speaker.
 *
 * Takes over Timer 1 (16-bit) for the 8000 Hz timer. This breaks PWM
 * (analogWrite()) for Arduino pins 9 and 10. Takes Timer 2 (8-bit)
 * for the pulse width modulation, breaking PWM for pins 11 & 3.
 *
 * References:
 *     http://www.uchobby.com/index.php/2007/11/11/arduino-sound-part-1/
 *     http://www.atmel.com/dyn/resources/prod_documents/doc2542.pdf
 *     http://www.evilmadscientist.com/article.php/avrdac
 *     http://gonium.net/md/2006/12/27/i-will-think-before-i-code/
 *     http://fly.cc.fer.hr/GDM/articles/sndmus/speaker2.html
 *     http://www.gamedev.net/reference/articles/article442.asp
 *
 * Michael Smith <michael@hurts.ca>
 */

#include <stdint.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>

#include <SPI.h>
#include <SD.h>
#include <LiquidCrystal.h>



/*
 * The audio data needs to be unsigned, 8-bit, 8000 Hz, and small enough
 * to fit in flash. 10000-13000 samples is about the limit.
 *
 * sounddata.h should look like this:
 *     const int sounddata_length=10000;
 *     const unsigned char sounddata_data[] PROGMEM = { ..... };
 *
 * You can use wav2c from GBA CSS:
 *     http://thieumsweb.free.fr/english/gbacss.html
 * Then add "PROGMEM" in the right place. I hacked it up to dump the samples
 * as unsigned rather than signed, but it shouldn't matter.
 *
 * http://musicthing.blogspot.com/2005/05/tiny-music-makers-pt-4-mac-startup.html
 * mplayer -ao pcm macstartup.mp3
 * sox audiodump.wav -v 1.32 -c 1 -r 8000 -u -1 macstartup-8000.wav
 * sox macstartup-8000.wav macstartup-cut.wav trim 0 10000s
 * wav2c macstartup-cut.wav sounddata.h sounddata
 *
 * (starfox) nb. under sox 12.18 (distributed in CentOS 5), i needed to run
 * the following command to convert my wav file to the appropriate format:
 * sox audiodump.wav -c 1 -r 8000 -u -b macstartup-8000.wav
 */

const int chipSelect = 10;
const int speakerPin = 3; // Can be either 3 or 11, two PWM outputs connected to Timer 2
const int RECV_PIN = 4;

volatile uint32_t sample = 0;

const unsigned cache_size = 128;
byte buf1[cache_size] = {0};
byte buf2[cache_size] = {0};
byte* frontBuf = buf1;
byte* backBuf = buf2;
byte* readBuf = frontBuf;
volatile bool bufferSwapped = false;
char filename[8+3+1] = "inv.wav";
bool blinkStatus = false;

unsigned lastMillis = 0;

String command = "";
bool commandComplete = false;

enum PlayState {
  STOPPED,
  PLAYING,
  PAUSED
};

volatile PlayState playState = STOPPED;

uint32_t sampleRate = 16000;

LiquidCrystal lcd(A0, A1, A2, A3, A4, A5);

File soundfile;
File rootDir;

struct WaveFileHeader
{
  uint8_t riffSig[4];
  uint32_t fileSize;
  uint8_t waveSig[4];

  uint8_t fmtSig[4];
  uint32_t fmtHeaderLength;
  uint16_t formatTag;
  uint16_t channels;
  uint32_t sampleRate;
  uint32_t avgBytesPS;
  uint16_t alignment;
  uint16_t bitsPerSample;

  uint8_t dataSig[4];
  uint32_t dataLen;
} __attribute__ ((packed));

/*
 * ******************************************
 * Playback control functions
 * ******************************************
 */
void stopPlayback()
{
  playState = STOPPED;
  soundfile.close();
  memset(buf1, 0, cache_size);
  memset(buf2, 0, cache_size);  
  readBuf = frontBuf;
  lcd.setCursor(0,1);
  lcd.write(byte(STOPPED));
}

void startPlayback()
{
  if(!soundfile.available())
  {
    soundfile = SD.open(filename);
    WaveFileHeader header;
    soundfile.read(reinterpret_cast<uint8_t*>(&header), 38);
    sampleRate = header.sampleRate;
    cli();
    OCR1A = F_CPU / sampleRate;
    sei();
    soundfile.read(frontBuf, cache_size);
    bufferSwapped = true;
    sample = 0;
  }
  playState = PLAYING;
  printState();
}

void pausePlayback()
{
  playState = PAUSED;
  lcd.setCursor(0,1);
  lcd.write(byte(PAUSED));
}

void next()
{
  bool wasPlaying = playState == PLAYING;
  stopPlayback();
  File nextFile;
  do
  {
    nextFile = rootDir.openNextFile();
    if(!nextFile)
    {
      rootDir.rewindDirectory();
      continue;
    }
  } while(!nextFile || !String(nextFile.name()).endsWith(".WAV"));
  
  strncpy(filename, nextFile.name(), 8+3+1);
  filename[11] = '\0';
  
  printTimeTotal(nextFile);
  printTimePlaying();
  nextFile.close();
  if(wasPlaying)
    startPlayback();
  printState();  
}

bool swapBuffers()
{
  byte* temp = frontBuf;
  frontBuf = backBuf;
  backBuf = temp;
  readBuf = frontBuf;
  bufferSwapped = true;
}

/*
 * ******************************************
 * Sample interrupt vector
 * ******************************************
 */
ISR(TIMER1_COMPA_vect) {
  if(playState != PLAYING)
  {
    if (speakerPin == 11) {
      OCR2A = 0;
    } else {
      OCR2B = 0;
    }
    return;
  }
  
  if(sample == soundfile.size())
  {
    next();
    return;
  }
  
  if (readBuf == (frontBuf + cache_size)) {
    swapBuffers();
  }
  if (speakerPin == 11) {
    OCR2A = *readBuf++;
  } else {
    OCR2B = *readBuf++;
  }
  sample++;
}


/*
 * ******************************************
 * PWM setup function
 * ******************************************
 */
void setupPlayback()
{
  pinMode(speakerPin, OUTPUT);

  // Set up Timer 2 to do pulse width modulation on the speaker
  // pin.

  // Use internal clock (datasheet p.160)
  ASSR &= ~(_BV(EXCLK) | _BV(AS2));

  // Set fast PWM mode  (p.157)
  TCCR2A |= _BV(WGM21) | _BV(WGM20);
  TCCR2B &= ~_BV(WGM22);

  if (speakerPin == 11) {
    // Do non-inverting PWM on pin OC2A (p.155)
    // On the Arduino this is pin 11.
    TCCR2A = (TCCR2A | _BV(COM2A1)) & ~_BV(COM2A0);
    TCCR2A &= ~(_BV(COM2B1) | _BV(COM2B0));
    // No prescaler (p.158)
    TCCR2B = (TCCR2B & ~(_BV(CS12) | _BV(CS11))) | _BV(CS10);
    
    OCR2A = 0;
  } else {
    // Do non-inverting PWM on pin OC2B (p.155)
    // On the Arduino this is pin 3.
    TCCR2A = (TCCR2A | _BV(COM2B1)) & ~_BV(COM2B0);
    TCCR2A &= ~(_BV(COM2A1) | _BV(COM2A0));
    // No prescaler (p.158)
    TCCR2B = (TCCR2B & ~(_BV(CS12) | _BV(CS11))) | _BV(CS10);

    OCR2B = 0;
  }

  // Set up Timer 1 to send a sample every interrupt.

  cli();

  // Set CTC mode (Clear Timer on Compare Match) (p.133)
  // Have to set OCR1A *after*, otherwise it gets reset to 0!
  TCCR1B = (TCCR1B & ~_BV(WGM13)) | _BV(WGM12);
  TCCR1A = TCCR1A & ~(_BV(WGM11) | _BV(WGM10));

  // No prescaler (p.134)
  TCCR1B = (TCCR1B & ~(_BV(CS12) | _BV(CS11))) | _BV(CS10);

  // Set the compare register (OCR1A).
  // OCR1A is a 16-bit register, so we have to do this with
  // interrupts disabled to be safe.
  OCR1A = F_CPU / sampleRate;    // 16e6 / 8000 = 2000

  // Enable interrupt when TCNT1 == OCR1A (p.136)
  TIMSK1 |= _BV(OCIE1A);

  sample = 0;
  sei();
}

/*
 * ******************************************
 * General setup function
 * ******************************************
 */
void setup()
{
  byte playSymbol[8] = {
    B00000,
    B10000,
    B11000,
    B11100,
    B11110,
    B11100,
    B11000,
    B10000
  };
  
  byte stopSymbol[8] = {
    B00000,
    B00000,
    B11110,
    B11110,
    B11110,
    B11110,
    B00000,
    B00000
  };
  
  byte pauseSymbol[8] = {
    B00000,
    B11011,
    B11011,
    B11011,
    B11011,
    B11011,
    B11011,
    B00000
  };

  byte blockChar[8] = {
    B11111,
    B11111,
    B11111,
    B11111,
    B11111,
    B11111,
    B11111,
    B11111
  };
  
  lcd.begin(16, 2);
  lcd.print("PCMAudio");
  lcd.createChar(PLAYING, playSymbol);
  lcd.createChar(STOPPED, stopSymbol);
  lcd.createChar(PAUSED, pauseSymbol);
  lcd.createChar(3, blockChar);
  
  
  lcd.setCursor(0,0);
  lcd.print("                "); // 16x space
  
  
  Serial.begin(9600);
  Serial.print(F_CPU);
  if (!SD.begin(chipSelect)) {
    Serial.println("Card failed, or not present");
    return;
  }
  Serial.println("Card initialized");
  rootDir = SD.open("/");
  next();
  
  lcd.setCursor(0,1);
  lcd.write(byte(STOPPED));
  lcd.print(filename);
  setupPlayback();
}

/*
 * ******************************************
 * Loop function
 * ******************************************
 */
void loop()
{
  if(bufferSwapped)
  {
    soundfile.read(backBuf, cache_size);
    bufferSwapped = false;

    printTimePlaying();
  }

  if(playState == PAUSED && (millis() - lastMillis) >= 1000)
  {
    blinkTimePlaying();
    lastMillis = millis();
  }

  if(Serial.available())
  {    
    while(Serial.available())
    {
      char in = Serial.read();
      if(in == '\r') break;
      if(in == '\n') 
      {
        commandComplete = true;
        break;
      }
      command += in;
    }
    if(commandComplete)
    {
      Serial.print("[");
      Serial.print(command);
      Serial.println("]");
      if(command == "play")
      {
        Serial.println("Playing");
        startPlayback();  
      }
      else if(command == "pause")
      {
        Serial.println("Pausing");
        pausePlayback();
      }
      else if(command == "stop")
      {
        Serial.println("Stopping");
        stopPlayback();
      }
      else if(command == "filename")
      {
        Serial.println(filename);
      }
      else if(command == "playstate")
      {
        Serial.println(playState);
      }
      else if(command == "progress")
      {
        Serial.println(soundfile.position() * 100 / soundfile.size());
      }
      else if(command[0] == 's' && command[1] == ' ')
      {
        strncpy(filename, &command[2], 8+3+1);
        stopPlayback();
        filename[11] = '\0';
        startPlayback();
      }
      else if(command == "next")
      {
        next();
        Serial.print("Playing next file: ");
        Serial.println(filename);
      }
      else if(command == "togglepp")
      {
        if(playState == PLAYING)
        {
          pausePlayback();
        } 
        else 
        {
          startPlayback();
        }
        
      }
      else
      {
        Serial.println("Unrecognized command");
      }
      command = "";
      commandComplete = false;
    }
  }
  
}

void printTimePlaying()
{
  lcd.setCursor(0,0);
  int timePlaying = sample / sampleRate;
  int minutes = timePlaying / 60;
  int seconds = timePlaying % 60;

  if(minutes > 10)
  {
    lcd.print("X");
  }
  else
  {
    lcd.print(minutes);
  }
  lcd.print(":");
  if(seconds < 10)
  {
    lcd.print("0");
  }
  lcd.print(seconds);
}

void printTimeTotal(File file)
{
  lcd.setCursor(12,0);
  int timeTotal = file.size() / sampleRate;
  int minutes = timeTotal / 60;
  int seconds = timeTotal % 60;

  if(minutes > 10)
  {
    lcd.print("X");
  }
  else
  {
    lcd.print(minutes);
  }
  lcd.print(":");
  if(seconds < 10)
  {
    lcd.print("0");
  }
  lcd.print(seconds);
}

void blinkTimePlaying()
{
  if(blinkStatus)
  {
    printTimePlaying();
  }
  else
  {
    lcd.setCursor(0,0);
    lcd.print("    "); // 4x space
  }
  blinkStatus = !blinkStatus;
}

void printState()
{
  lcd.setCursor(0,1);
  lcd.write(byte(PLAYING));
  lcd.print("               "); // 15x space
  lcd.setCursor(1,1);
  lcd.print(filename);
}

