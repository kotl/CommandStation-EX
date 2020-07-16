/*
 *  © 2020, Chris Harlow. All rights reserved.
 *  
 *  This file is part of Asbelos DCC API
 *
 *  This is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  It is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "WifiInterface.h"
#include "Config.h"
#include "DIAG.h"
#include "StringFormatter.h"
#include "WiThrottle.h"
#include "HTTPParser.h" 
const char  PROGMEM READY_SEARCH[]  ="\r\nready\r\n";
const char  PROGMEM OK_SEARCH[] ="\r\nOK\r\n";
const char  PROGMEM END_DETAIL_SEARCH[] ="@ 1000";
const char  PROGMEM PROMPT_SEARCH[] =">";
const char  PROGMEM SEND_OK_SEARCH[] ="\r\nSEND OK\r\n";
const unsigned long LOOP_TIMEOUT=2000;
bool WifiInterface::connected=false;
bool WifiInterface::closeAfter=false;
DCCEXParser  WifiInterface::parser;
byte WifiInterface::loopstate=0;
unsigned long WifiInterface::loopTimeoutStart=0;
int WifiInterface::datalength=0;
int WifiInterface::connectionId;
byte WifiInterface::buffer[MAX_WIFI_BUFFER];
MemStream  WifiInterface::streamer(buffer,sizeof(buffer));

void WifiInterface::setup(Stream & wifiStream,  const __FlashStringHelper* SSid, const __FlashStringHelper* password,
                 const __FlashStringHelper* hostname, const __FlashStringHelper* servername, int port) {
  
  DIAG(F("\n++++++ Wifi Setup In Progress ++++++++\n"));
  connected=setup2(wifiStream, SSid, password,hostname, servername,port);
  // TODO calloc the buffer and streamer and parser etc 
  DIAG(F("\n++++++ Wifi Setup %S ++++++++\n"), connected?F("OK"):F("FAILED"));
}

bool WifiInterface::setup2(Stream & wifiStream, const __FlashStringHelper* SSid, const __FlashStringHelper* password,
                 const __FlashStringHelper* hostname, const __FlashStringHelper* servername, int port) {
  
  delay(1000);

  StringFormatter::send(wifiStream,F("AT+RST\r\n")); // reset module
  //checkForOK(wifiStream,5000,END_DETAIL_SEARCH,true);  // Show startup but ignore unreadable upto ready
  checkForOK(wifiStream,5000,READY_SEARCH,true); 
  
  StringFormatter::send(wifiStream,F("AT+CWMODE=1\r\n")); // configure as access point
  if (!checkForOK(wifiStream,10000,OK_SEARCH,true)) return false;

  // StringFormatter::send(wifiStream, F("AT+CWHOSTNAME=\"%S\"\r\n"), hostname); // Set Host name for Wifi Client
  // checkForOK(wifiStream,5000, OK_SEARCH, true);


  StringFormatter::send(wifiStream,F("AT+CWJAP=\"%S\",\"%S\"\r\n"),SSid,password);
  if (!checkForOK(wifiStream,20000,OK_SEARCH,true)) return false;
  
  StringFormatter::send(wifiStream,F("AT+CIFSR\r\n")); // get ip address //192.168.4.1
  if (!checkForOK(wifiStream,10000,OK_SEARCH,true)) return false;

  
  StringFormatter::send(wifiStream,F("AT+CIPMUX=1\r\n")); // configure for multiple connections
  if (!checkForOK(wifiStream,10000,OK_SEARCH,true)) return false;
  
  StringFormatter::send(wifiStream,F("AT+CIPSERVER=1,%d\r\n"),port); // turn on server on port 80
  if (!checkForOK(wifiStream,10000,OK_SEARCH,true)) return false;

 // StringFormatter::send(wifiStream, F("AT+MDNS=1,\"%S.local\",\"%S.local\",%d\r\n"), hostname, servername, port); // Setup mDNS for Server
 // if (!checkForOK(wifiStream,5000, OK_SEARCH, true)) return false;
 
  return true;
}

bool WifiInterface::checkForOK(Stream & wifiStream, const unsigned int timeout, const char * waitfor, bool echo) {
  unsigned long  startTime = millis();
   char  const *locator=waitfor;
  DIAG(F("\nWifi Check: [%E]"),waitfor);
  while( millis()-startTime < timeout) {
    while(wifiStream.available()) {
      int ch=wifiStream.read();
      if (echo) StringFormatter::printEscape(Serial,ch);  /// THIS IS A DIAG IN DISGUISE
      if (ch!=pgm_read_byte_near(locator)) locator=waitfor;
      if (ch==pgm_read_byte_near(locator)) {
        locator++;
        if (!pgm_read_byte_near(locator)) {
          DIAG(F("\nFound in %dms"),millis()-startTime);
          return true;
        }
      }
    }
  }
  DIAG(F("\nTIMEOUT after %dms\n"),timeout);
  return false;
}

bool WifiInterface::isHTML() {
  
  // POST GET PUT PATCH DELETE
  // You may think a simple strstr() is better... but not when ram & time is in short supply  
  switch (buffer[0]) {
    case 'P':
         if (buffer[1]=='U' && buffer[2]=='T' && buffer[3]==' ' ) return true; 
         if (buffer[1]=='O' && buffer[2]=='S' && buffer[3]=='T' && buffer[4]==' ') return true; 
         if (buffer[1]=='A' && buffer[2]=='T' && buffer[3]=='C' && buffer[4]=='H' && buffer[5]==' ') return true;
         return false; 
    case 'G':
         if (buffer[1]=='E' && buffer[2]=='T' && buffer[3]==' ' ) return true; 
         return false;
    case 'D':
         if (buffer[1]=='E' && buffer[2]=='L' && buffer[3]=='E' && buffer[4]=='T' && buffer[5]=='E' && buffer[6]==' ') return true; 
         return false;
    default:
       return false;
  } 
}
 
void WifiInterface::loop(Stream & wifiStream) {
    if (!connected) return; 
    
    WiThrottle::loop();  // check heartbeats 
    
    // read anything into a buffer, collecting info on the way  
    while (loopstate!=99 && wifiStream.available()) { 
      int ch=wifiStream.read();
      
      // echo the char to the diagnostic stream in escaped format
      StringFormatter::printEscape(Serial,ch); // DIAG in disguise
      
      switch (loopstate) {
           case 0:  // looking for +IPD
                connectionId=0;
                if (ch=='+') loopstate=1;
                break;
           case 1:  // Looking for I   in +IPD  
                loopstate= (ch=='I')?2:0;
                break; 
           case 2:  // Looking for P   in +IPD  
                loopstate= (ch=='P')?3:0;
                break; 
           case 3:  // Looking for D   in +IPD  
                loopstate= (ch=='D')?4:0;
                break; 
           case 4:  // Looking for ,   After +IPD  
                loopstate= (ch==',')?5:0;
                break; 
           case 5:  // reading connection id
                if (ch==',') loopstate=6;
                else connectionId=10*connectionId+(ch-'0');
                break;
           case 6: // reading for length
                if (ch==':') loopstate=(datalength==0)?99:7;  // 99 is getout without reading next char
                else datalength=datalength*10 + (ch-'0');
                streamer.flush();  // basically sets write point at start of buffer
                break;
           case 7: // reading data 
                streamer.write(ch);
                datalength--;
                if (datalength==0) loopstate=99;
                break;

           case 10:  // Waiting for > so we can send reply
                if (millis()-loopTimeoutStart > LOOP_TIMEOUT) {
                  DIAG(F("\nWifi TIMEOUT on wait for > prompt or ERROR\n"));
                  loopstate=0; // go back to +IPD
                  break;
                } 
                if (ch=='>'){
                  DIAG(F("\n> [%e]\n"),buffer);
                  wifiStream.print((char *) buffer);
                  loopTimeoutStart=millis();
                  loopstate=closeAfter?11:0;
                }
                break;
           case 11: // Waiting for SEND OK or ERROR to complete so we can closeAfter
                if (millis()-loopTimeoutStart > LOOP_TIMEOUT) {
                  DIAG(F("\nWifi TIMEOUT on wait for SEND OK or ERROR\n"));
                  loopstate=0; // go back to +IPD
                  break;
                }
                if (ch=='K') { // assume its in  SEND OK 
                  StringFormatter::send(wifiStream,F("AT+CIPCLOSE=%d\r\n"),connectionId);
                  loopstate=0; // wait for +IPD
                }
                break;                    
        } // switch 
    } // while
    if (loopstate!=99) return; 
    
    // AT this point we have read an incoming message into the buffer
    streamer.write((byte)0); // null the end of the buffer so we can treat it as a string

    DIAG(F("\nWifiRead:%d:%e\n"),connectionId,buffer);
    streamer.setBufferContentPosition(0,0);  // reset write position to start of buffer
    // SIDE EFFECT WARNING::: 
    //  We know that parser will read the entire buffer before starting to write to it.
    //  Otherwise we would have to copy the buffer elsewhere and RAM is in short supply.

   closeAfter=false;
   // Intercept HTTP requests 
    if (isHTML()) {
      HTTPParser::parse(streamer,buffer);
      closeAfter=true;
    }
    else if (buffer[0]=='<')  parser.parse(streamer,buffer, true);    // tell JMRI parser that callbacks are diallowed because we dont want to handle the async 
 
    else WiThrottle::getThrottle(connectionId)->parse(streamer, buffer);

    if (streamer.available()==0) {
      // No reply
      if (closeAfter) StringFormatter::send(wifiStream,F("AT+CIPCLOSE=%d\r\n"),connectionId);
      loopstate=0; // go back to waiting for +IPD
      return;    
    }
    // prepare to send reply 
    streamer.write((byte)0x00); // just put a null byte on end of buffer so we can mark the end.
    DIAG(F("\nWiFiInterface reply c(%d) l(%d) [%e]\n"),connectionId,streamer.available()-1,buffer);
    StringFormatter::send(wifiStream,F("AT+CIPSEND=%d,%d\r\n"),connectionId,streamer.available()-1);
    loopTimeoutStart=millis();
    loopstate=10; // non-blocking loop waits for > before sending
    }
