#include <Arduino.h>
#include <SPI.h>
#include <UIPEthernet.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <stdlib.h>
#include <stdarg.h>
#include <avr/wdt.h>


// ------------------------------------------------------
// Configuracion de red
// ------------------------------------------------------
byte 			mac[] = 		{ 0xDE, 0xAD, 0xDE, 0xAD, 0xFE, 0xED };
byte 			ip[] = 			{ 192, 168, 1, 202 };
byte 			gateway[] = 		{ 192, 168, 1, 1 };
byte 			subnet[] = 		{ 255, 255, 255, 0 };

#define 		kHTTP_PORT 		5001
EthernetServer 		server = 		EthernetServer(kHTTP_PORT);
int 			num_lineas;
int 			pos;
#define 		kBUFFER_SIZE 		255
char 			buffer[kBUFFER_SIZE];


// ------------------------------------------------------
// Control de consumo
// ------------------------------------------------------
float			average = 		0.0;
int			num_samples=		100;
#define 		kCONSUMO_NULO 		(1023/2)   //Valor del ACS712 que indica consumo 0
#define 		kTOLERANCIA 		5	   //Diferencia con el consumo nulo a partir de la cual consideramos que se esta consumiendo algo
#define			kPIN_SENSOR 		16	   //Pines para los sensores de corriente ACS712
int			consumo = 		0;	   //Consumo de las farolas


// ------------------------------------------------------
// Control de reles
// ------------------------------------------------------
#define			kAPAGADA		1	   //La placa de reles funciona asi: L=ON, H=OFF
#define			kENCENDIDA		0
int			pin_farola[3] = 	{2,3,6};   // Pines que controlaran los reles de las farolas
int			estado_farola[3] = 	{kAPAGADA,kAPAGADA,kAPAGADA};   // Estados iniciales de las farolas

// ------------------------------------------------------
// Temperatura
// ------------------------------------------------------
#define 		PIN_ONEWIRE 9
OneWire 		oneWire(PIN_ONEWIRE);
DallasTemperature 	temp(&oneWire);
DeviceAddress 		direccion_sensor_temp = {0x28, 0xA6, 0x0A, 0xEA, 0x03, 0x00, 0x00, 0xFB};

// Ultimas lecturas correctas, para usar cuando la sonda no devuelva valores correctos
float			ultima_lectura_correcta = 20.0;
int			lecturas_erroneas = 	0;




// ------------------------------------------------------
// INICIALIZACION
// ------------------------------------------------------
void setup()
{
  wdt_disable();

  int i=0;

  consumo=0;
  pinMode(kPIN_SENSOR,INPUT);

  for(i=0;i<3;i++)
  {
     estado_farola[i]=kAPAGADA;
     pinMode(pin_farola[i],OUTPUT);
     digitalWrite(pin_farola[i],estado_farola[i]); //Apagamos las farolas al iniciar
  }

  num_lineas=0;
  for(pos=0;pos<kBUFFER_SIZE;pos++)
     buffer[pos]=0;
  pos=0;

  temp.begin();
  temp.setResolution(direccion_sensor_temp,12);

  Serial.begin(9600);

  Ethernet.begin(mac,ip,gateway,subnet);
  server.begin();

  wdt_enable(WDTO_8S);
}



// ------------------------------------------------------
// BUCLE PRINCIPAL
// ------------------------------------------------------
void loop()
{
  wdt_reset();

  // A) Comprobamos los comandos via HTTP
  EthernetClient client = server.available();
  if(client)
  {
     wdt_reset();
     readHttp(client,buffer);
     wdt_reset();
  }

  // B) Si se han producido demasiados errores en las lecturas de las sondas reseteamos el arduino
  if(lecturas_erroneas==6)
    softReset();
}



void readHttp(EthernetClient client, char* buffer)
{
    boolean currentLineIsBlank = true;
    boolean salir=false;

    while(client.connected() && !salir)
    {
      if(client.available())
      {
        char c = client.read();

        // Si estamos en la primera linea y el caracter es distinto de \n o \r lo añadimos al buffer de linea
        if(num_lineas==0)
        {
           if(c!='\r' && c!='\n')
           {
              buffer[pos]=c;
              if(pos<kBUFFER_SIZE-1) //no sobrepasamos el tamaño del buffer
                pos++;
           }
           else
           {
              buffer[pos]='\0';
           }
        }

        // Si hemos leido la peticion HTTP completa, la procesamos
        if(c == '\n' && currentLineIsBlank) 
        {
          num_lineas=0;
          procesa_peticion(client,buffer);

          salir=true;
        }
        else
        {
          if (c == '\n') 
          {
            num_lineas++;
            pos=0;
            currentLineIsBlank = true;
          }
          else if (c != '\r') 
          {
            currentLineIsBlank = false;
          }
        }
      }
    }

    delay(1);
    client.stop();
}


void procesa_peticion(EthernetClient client, char* buffer)
{
   char *str1, *str2, *str3, *token, *subtoken, *subsubtoken;
   char *saveptr1, *saveptr2, *saveptr3;
   int j,k;
   boolean salir=false;
//   char key[kBUFFER_SIZE];
//   char value[kBUFFER_SIZE]; 
   char *key=NULL;
   char *value=NULL;


   // send a standard http response header
   client.println("HTTP/1.1 200 OK");
   client.println("Content-Type: text/html");
   client.println("Connection: close");
   client.println("Refresh: 5");
   client.println();
   client.println("<!DOCTYPE HTML>");
   client.println("<html>");

   client.print(buffer);
   client.println("<br/>");

   // 0. Debug
   for (j = 1, str1 = buffer, salir=false; !salir; j++, str1 = NULL) 
   {
      token = strtok_r(str1, " ", &saveptr1);
      if (token == NULL)
      {
          salir=true;
      }
      else
      {
        //Argumentos de la peticion
        if(j==2)
        {
           client.print("Args: [<i>"); client.println(token); client.println("</i>]<br/>");

           for (str2 = token, subtoken=token; subtoken!=NULL; str2 = NULL) 
           {
             subtoken = strtok_r(str2, "?/&", &saveptr2);
             if (subtoken != NULL)
             {
               client.print(" --> "); client.println(subtoken); client.println("<br/>");

               //memset(key,0,sizeof(char)*kBUFFER_SIZE);
               //memset(value,0,sizeof(char)*kBUFFER_SIZE);
               for (str3 = subtoken, subsubtoken=subtoken,k=0; ; str3 = NULL,k++) 
               {
                  subsubtoken = strtok_r(str3, "=", &saveptr3);
                  if(k==0 && subsubtoken!=NULL)
                  {
                     //memcpy(key,subsubtoken,strlen(subsubtoken));
                     key=strdup(subsubtoken);
                  }
                  else if(k==1 && subsubtoken!=NULL)
                  {
                     //memcpy(value,subsubtoken,strlen(subsubtoken));
                     value=strdup(subsubtoken);
                  }

                  if (subsubtoken == NULL || k==1)
                     break;
                  client.print("  * "); client.println(subsubtoken); client.println("<br/>");
               }


               processData(client,key,value);

               if(key!=NULL)
                 free(key);
               if(value!=NULL)
                 free(value);
               key=NULL;
               value=NULL;
             }
           }
           client.println("<br/>");
        }
      }
    }


   // 1a. Calculamos las tres medias de consumo
   average = 0;
   for(int i = 0; i < num_samples; i++)
   {
      average = average + analogRead(kPIN_SENSOR);
      delay(2);
   }

   client.print("power");
   client.print(": ");
   client.println(average/num_samples);
   client.print("<br/>");

   if(abs(kCONSUMO_NULO-average/num_samples)>kTOLERANCIA)
   {
       client.print("<b>CONSUMO");
       client.print(": </b><i>[");
       client.print(kCONSUMO_NULO-average/num_samples);
       client.println("]</i>");
   }
   else
   {
       client.print("CONSUMO");
       client.print(": <i>[");
       client.print(kCONSUMO_NULO-average/num_samples);
       client.println("]</i>");
   }
   client.println("<br/>");

   client.print("current");
   client.print(": ");
   client.println(abs(0.0264*(average/num_samples-kCONSUMO_NULO)));
   client.println("<br/>");

   client.print("watts");
   client.print(": ");
   client.println(abs((0.0264*(average/num_samples-kCONSUMO_NULO)*230)));
   client.println("<br/>");

   client.print("tempDS:");
   client.println(readTempSensor(direccion_sensor_temp));
   client.println("<br/>");

   client.print("atemp:");
   client.println(readInternalTemp());
   client.println("<br/>");

   client.print("avcc:");
   client.println(readVcc());
   client.println("<br/>");

   client.print("estado_farola1:");
   client.println(estado_farola[0]);
   client.println("<br/>");
   client.print("estado_farola2:");
   client.println(estado_farola[1]);
   client.println("<br/>");
   client.print("estado_farola3:");
   client.println(estado_farola[2]);
   client.println("<br/>");

   client.print("errores_sensor:");
   client.println(lecturas_erroneas);
   client.println("<br/>");

   client.println("</html>");
}


// ------------------------------------------------------
// Ejecuta la accion correspondiente segun los valores
// ------------------------------------------------------
void processData(EthernetClient client, char* key, char* value)
{
   int valor=0;

   client.print("P key: [");
   if(key!=NULL)
      client.print(key);
   client.print("] value: [");
   if(value!=NULL)
      client.print(value);
   client.println("]<br/>");

   if(key!=NULL)
   {
      if(strcmp(key,"farola1")==0)
      {
         if(value!=NULL)
         {
            valor=atoi(value);
            if(valor==kAPAGADA || valor==kENCENDIDA)
            {
               estado_farola[0]=valor;
               digitalWrite(pin_farola[0],estado_farola[0]);
            }
         }
      }

      if(strcmp(key,"farola2")==0)
      {
         if(value!=NULL)
         {
            valor=atoi(value);
            if(valor==kAPAGADA || valor==kENCENDIDA)
            {
               estado_farola[1]=valor;
               digitalWrite(pin_farola[1],estado_farola[1]);
            }
         }
      }

      if(strcmp(key,"farola3")==0)
      {
         if(value!=NULL)
         {
            valor=atoi(value);
            if(valor==kAPAGADA || valor==kENCENDIDA)
            {
               estado_farola[2]=valor;
               digitalWrite(pin_farola[2],estado_farola[2]);
            }
         }
      }
   }
}


// ------------------------------------------------------
// Auxiliar: Devuelve la temperatura del ATmega328 (ºmC)
// ------------------------------------------------------
long readInternalTemp() 
{
  long result;
  // Read temperature sensor against 1.1V reference
  ADMUX = _BV(REFS1) | _BV(REFS0) | _BV(MUX3);
  delay(2); // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Convert
  while (bit_is_set(ADCSRA,ADSC));
  result = ADCL;
  result |= ADCH<<8;
  result = (result - 125) * 1075;

  return result;
}


// ------------------------------------------------------
// Auxiliar: Devuelve el Vcc del ATmega328 (mV)
// ------------------------------------------------------
long readVcc() 
{
  long result;
  // Read 1.1V reference against AVcc
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delay(2); // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Convert
  while (bit_is_set(ADCSRA,ADSC));
  result = ADCL;
  result |= ADCH<<8;
  result = 1126400L / result; // Back-calculate AVcc in mV
  return result;
}



// ------------------------------------------------------
// Devuelve la temperatura de la sonda especificada
// ------------------------------------------------------
float readTempSensor(DeviceAddress sensor)
{
  int i=10;
  int j=0;
  float valor=-127;

  while((valor<=-55 || valor>=85) && i>0)
  {
     i--; //maximo 10 iteraciones

     cli();
     temp.requestTemperatures();
     valor=temp.getTempC(sensor)/1;
     sei();
     delay(30);
     Serial.print(".");
  }
  Serial.println("-");

  if(valor==-127 || valor==85)
  {
     valor=ultima_lectura_correcta;
     lecturas_erroneas++;
  }
  else
  {
     lecturas_erroneas=0;
  }

  ultima_lectura_correcta=valor;

  return(valor);
}



// ------------------------------------------------------
// Restarts program from beginning but does not reset the peripherals and registers
// ------------------------------------------------------
void softReset()
{
   asm volatile ("  jmp 0");
}
