/**
 * Test #7 - Combine TNFS and SIO to boot Jumpman
 * Yes, this is a proof of concept, watch out for falling rocks.
 */

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

#define TNFS_SERVER "192.168.1.7"
#define TNFS_PORT 16384

enum {ID, COMMAND, AUX1, AUX2, CHECKSUM, ACK, NAK, PROCESS, WAIT} cmdState;

// Uncomment for Debug on 2nd UART (GPIO 2)
// #define DEBUG_S

#define PIN_LED         2
#define PIN_INT         5
#define PIN_PROC        4
#define PIN_MTR        16
#define PIN_CMD        12

#define DELAY_T5          600
#define READ_CMD_TIMEOUT  12
#define CMD_TIMEOUT       50

#define STATUS_SKIP       8

unsigned long cmdTimer = 0;
byte statusSkipCount = 0;

WiFiUDP UDP;
byte tnfs_fd;


union
{
  struct 
  {
    byte session_idl;
    byte session_idh;
    byte retryCount;
    byte command;
    byte data[508];
  };
  byte rawData[512];
} tnfsPacket;

union
{
  struct
  {
    unsigned char devic;
    unsigned char comnd;
    unsigned char aux1;
    unsigned char aux2;
    unsigned char cksum;
  };
  byte cmdFrameData[5];
} cmdFrame;

/**
   calculate 8-bit checksum.
*/
byte sio_checksum(byte* chunk, int length)
{
  int chkSum = 0;
  for (int i = 0; i < length; i++) {
    chkSum = ((chkSum + chunk[i]) >> 8) + ((chkSum + chunk[i]) & 0xff);
  }
  return (byte)chkSum;
}

/**
   ISR for falling COMMAND
*/
void sio_isr_cmd()
{
  if (digitalRead(PIN_CMD) == LOW)
  {
    cmdState = ID;
    cmdTimer = millis();
  }
}

/**
   Get ID
*/
void sio_get_id()
{
  cmdFrame.devic = Serial.read();
  if (cmdFrame.devic == 0x31)
    cmdState = COMMAND;
  else
  {
    cmdState = WAIT;
    cmdTimer = 0;
  }

#ifdef DEBUG_S
  Serial1.print("CMD DEVC: ");
  Serial1.println(cmdFrame.devic, HEX);
#endif
}

/**
   Get Command
*/
void sio_get_command()
{
  cmdFrame.comnd = Serial.read();
  if (cmdFrame.comnd == 'S' && statusSkipCount > STATUS_SKIP)
    cmdState = AUX1;
  else if (cmdFrame.comnd == 'S' && statusSkipCount < STATUS_SKIP)
  {
    statusSkipCount++;
    cmdState = WAIT;
    cmdTimer = 0;
  }
  else if (cmdFrame.comnd == 'R')
    cmdState = AUX1;
  else
  {
    cmdState = WAIT;
    cmdTimer = 0;
  }

#ifdef DEBUG_S
  Serial1.print("CMD CMND: ");
  Serial1.println(cmdFrame.comnd, HEX);
#endif
}

/**
   Get aux1
*/
void sio_get_aux1()
{
  cmdFrame.aux1 = Serial.read();
  cmdState = AUX2;

#ifdef DEBUG_S
  Serial1.print("CMD AUX1: ");
  Serial1.println(cmdFrame.aux1, HEX);
#endif
}

/**
   Get aux2
*/
void sio_get_aux2()
{
  cmdFrame.aux2 = Serial.read();
  cmdState = CHECKSUM;

#ifdef DEBUG_S
  Serial1.print("CMD AUX2: ");
  Serial1.println(cmdFrame.aux2, HEX);
#endif
}

/**
   Get Checksum, and compare
*/
void sio_get_checksum()
{
  byte ck;
  cmdFrame.cksum = Serial.read();
  ck = sio_checksum((byte *)&cmdFrame.cmdFrameData, 4);

#ifdef DEBUG_S
    Serial1.print("CMD CKSM: ");
    Serial1.print(cmdFrame.cksum, HEX);
#endif

    if (ck == cmdFrame.cksum)
    {
#ifdef DEBUG_S
      Serial1.println(", ACK");
#endif
      sio_ack();
    }
    else
    {
#ifdef DEBUG_S
      Serial1.println(", NAK");
#endif
      sio_nak();
    }
}

/**
   Process command
*/

void sio_process()
{
  switch (cmdFrame.comnd)
  {
    case 'R':
      sio_read();
      break;
    case 'S':
      sio_status();
      break;
  }
  
  cmdState = WAIT;
  cmdTimer = 0;
}

/**
   Read
*/
void sio_read()
{
  byte ck;
  byte sector[128];
  int offset =(256 * cmdFrame.aux2)+cmdFrame.aux1;
  offset *= 128;
  offset -= 128;
  offset += 16; // skip 16 byte ATR Header
  tnfs_seek(offset);
  tnfs_read();

  for (int i=0;i<128;i++)
    sector[i]=tnfsPacket.data[i+3];

  ck = sio_checksum((byte *)&sector, 128);

  delayMicroseconds(1500); // t5 delay
  Serial.write('C'); // Completed command
  Serial.flush();

  // Write data frame
  Serial.write(sector,128);
    
  // Write data frame checksum
  Serial.write(ck);
  Serial.flush();
  delayMicroseconds(200);
#ifdef DEBUG_S
  Serial1.print("SIO READ OFFSET: ");
  Serial1.print(offset);
  Serial1.print(" - ");
  Serial1.println((offset + 128));
#endif
}

/**
   Status
*/
void sio_status()
{
  byte status[4] = {0x00, 0xFF, 0xFE, 0x00};
  byte ck;

  ck = sio_checksum((byte *)&status, 4);

  delayMicroseconds(1500); // t5 delay
  Serial.write('C'); // Command always completes.
  Serial.flush();
  delayMicroseconds(200);
  //delay(1);

  // Write data frame
  for (int i = 0; i < 4; i++)
    Serial.write(status[i]);

  // Write checksum
  Serial.write(ck);
  Serial.flush();
  delayMicroseconds(200);
}

/**
   Send an acknowledgement
*/
void sio_ack()
{
  delayMicroseconds(500);
  Serial.write('A');
  Serial.flush();
  //cmdState = PROCESS;
  sio_process();
}

/**
   Send a non-acknowledgement
*/
void sio_nak()
{
  delayMicroseconds(500);
  Serial.write('N');
  Serial.flush();
  cmdState = WAIT;
  cmdTimer = 0;
}

void sio_incoming(){
  switch (cmdState)
  {
    case ID:
      sio_get_id();
      break;
    case COMMAND:
      sio_get_command();
      break;
    case AUX1:
      sio_get_aux1();
      break;
    case AUX2:
      sio_get_aux2();
      break;
    case CHECKSUM:
      sio_get_checksum();
      break;
    case ACK:
      sio_ack();
      break;
    case NAK:
      sio_nak();
      break;
    case PROCESS:
      sio_process();
      break;
    case WAIT:
      Serial.read(); // Toss it for now
      cmdTimer = 0;
      break;
  }
}

/**
 * Mount the TNFS server
 */
void tnfs_mount()
{
  int start=millis();
  int dur=millis()-start;
  
  memset(tnfsPacket.rawData, 0, sizeof(tnfsPacket.rawData));
  tnfsPacket.session_idl=0;
  tnfsPacket.session_idh=0;
  tnfsPacket.retryCount=0;
  tnfsPacket.command=0;
  tnfsPacket.data[0]=0x01;   // vers
  tnfsPacket.data[1]=0x00;   // "  "
  tnfsPacket.data[2]=0x2F;   // /
  tnfsPacket.data[3]=0x00;   // nul 
  tnfsPacket.data[4]=0x00;   // no username
  tnfsPacket.data[5]=0x00;   // no password

  UDP.beginPacket(TNFS_SERVER,TNFS_PORT);
  UDP.write(tnfsPacket.rawData,10);
  UDP.endPacket();

  while (dur < 5000)
  {
    if (UDP.parsePacket())
    {
      UDP.read(tnfsPacket.rawData,512);
      if (tnfsPacket.data[0]==0x00)
      {
        // Successful
        return;  
      }
      else
      {
        // Error
        return;  
      }
    }  
  }
  // Otherwise we timed out.
}

/**
 * Open 'autorun.atr'
 */
void tnfs_open()
{
  int start=millis();
  int dur=millis()-start;
  tnfsPacket.retryCount++;  // increase sequence #
  tnfsPacket.command=0x29;  // OPEN
  tnfsPacket.data[0]=0x01;  // R/O
  tnfsPacket.data[1]=0x00;  //
  tnfsPacket.data[2]=0x00;  // Flags
  tnfsPacket.data[3]=0x00;  //
  tnfsPacket.data[4]='/';   // Filename start
  tnfsPacket.data[5]='a';
  tnfsPacket.data[6]='u';
  tnfsPacket.data[7]='t';
  tnfsPacket.data[8]='o';
  tnfsPacket.data[9]='r';
  tnfsPacket.data[10]='u';
  tnfsPacket.data[11]='n';
  tnfsPacket.data[12]='.';
  tnfsPacket.data[13]='a';
  tnfsPacket.data[14]='t';
  tnfsPacket.data[15]='r';
  tnfsPacket.data[16]=0x00; // NUL terminated
  tnfsPacket.data[17]=0x00; // no username
  tnfsPacket.data[18]=0x00; // no password

  UDP.beginPacket(TNFS_SERVER,TNFS_PORT);
  UDP.write(tnfsPacket.rawData,19+4);
  UDP.endPacket();

  while (dur<5000)
  {
    if (UDP.parsePacket())
    {
      UDP.read(tnfsPacket.rawData,512);
      if (tnfsPacket.data[0]==0x00)
      {
        // Successful
        tnfs_fd=tnfsPacket.data[1];
        return;
      }
      else
      {
        // unsuccessful
        return;  
      }
    }
  }
  // Otherwise, we timed out.
}

/**
 * TNFS read
 */
void tnfs_read()
{
  int start=millis();
  int dur=millis()-start;
  tnfsPacket.retryCount++;  // Increase sequence
  tnfsPacket.command=0x21;  // READ
  tnfsPacket.data[0]=tnfs_fd; // returned file descriptor
  tnfsPacket.data[1]=0x80;  // 128 bytes
  tnfsPacket.data[2]=0x00;  //

  UDP.beginPacket(TNFS_SERVER,TNFS_PORT);
  UDP.write(tnfsPacket.rawData,4+3);
  UDP.endPacket();

  while (dur<5000)
  {
    if (UDP.parsePacket())
    {
      UDP.read(tnfsPacket.rawData,sizeof(tnfsPacket.rawData));
      if (tnfsPacket.data[0]==0x00)
      {
        // Successful
        return;
      }
      else
      {
        // Error
        return;
      }
    }
  }
}

/**
 * TNFS seek
 */
void tnfs_seek(long offset)
{
  int start=millis();
  int dur=millis()-start;
  byte offsetVal[4];

  offsetVal[0] = (int)((offset & 0xFF000000) >> 24 );
  offsetVal[1] = (int)((offset & 0x00FF0000) >> 16 );
  offsetVal[2] = (int)((offset & 0x0000FF00) >> 8 );
  offsetVal[3] = (int)((offset & 0X000000FF));  
  
  tnfsPacket.retryCount++;
  tnfsPacket.command=0x25; // LSEEK
  tnfsPacket.data[0]=tnfs_fd;
  tnfsPacket.data[1]=0x00; // SEEK_SET
  tnfsPacket.data[2]=offsetVal[0];
  tnfsPacket.data[3]=offsetVal[1];
  tnfsPacket.data[4]=offsetVal[2];
  tnfsPacket.data[5]=offsetVal[3];

  UDP.beginPacket(TNFS_SERVER,TNFS_PORT);
  UDP.write(tnfsPacket.rawData,6+4);
  UDP.endPacket();

  while (dur<5000)
  {
    if (UDP.parsePacket())
    {
      UDP.read(tnfsPacket.rawData,sizeof(tnfsPacket.rawData));
      if (tnfsPacket.data[0]==0)
      {
        // Success.
        return;  
      }
      else
      {
        // Error.
        return;  
      }
    }  
  }
}

void setup() 
{
  // Set up pins
#ifdef DEBUG_S
  Serial1.begin(19200);
  Serial1.println();
  Serial1.println("#AtariWifi Test Program #7 started");
#else
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
#endif
  pinMode(PIN_INT, INPUT);
  pinMode(PIN_PROC, INPUT);
  pinMode(PIN_MTR, INPUT);
  pinMode(PIN_CMD, INPUT);

  WiFi.begin("SSID", "PASSWORD");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
  }
  
  UDP.begin(16384);

  tnfs_mount();
  tnfs_open();

  // Set up serial
  Serial.begin(19200);
  Serial.swap();

  // Attach COMMAND interrupt.
  attachInterrupt(digitalPinToInterrupt(PIN_CMD), sio_isr_cmd, FALLING);
  cmdState = WAIT; // Start in wait state
}

void loop() 
{
  if (Serial.available() > 0)
  {
    sio_incoming();
  }
  
  if (millis() - cmdTimer > CMD_TIMEOUT && cmdState != WAIT)
  {
    Serial1.print("SIO CMD TIMEOUT: ");
    Serial1.println(cmdState);
    cmdState = WAIT;
    cmdTimer = 0;
  }
}
