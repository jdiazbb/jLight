#include <Arduino.h>
#include <SPI.h>
#include <UIPEthernet.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <stdlib.h>
#include <stdarg.h>
#include <avr/wdt.h>


//#define DEBUG

// ------------------------------------------------------
// Configuracion de red
// ------------------------------------------------------
byte 			mac[] = 		{ 0xDE, 0xAD, 0xDE, 0xAD, 0xFE, 0xED };
byte 			ip[] = 			{ 192, 168, 2, 3 };
byte 			gateway[] = 		{ 192, 168, 2, 1 };
byte 			subnet[] = 		{ 255, 255, 255, 0 };

#define 		kHTTP_PORT 		5001
EthernetServer 		server = 		EthernetServer(kHTTP_PORT);
int 			num_lineas;
int 			pos;
#define 		kBUFFER_SIZE 		255
char 			buffer[kBUFFER_SIZE];

unsigned long		ultima_conexion;		   // instante de la ultima conexion de red correcta
unsigned long		conexion_actual;		   // instante de la conexion actual
#define			kTIMEOUT_JDOMO		600	   // segundos que puede permanecer el sistema sin que le lleguen conexiones desde el nodo central
#define 		kPIN_PLC		7	   // pin que controla el rele del PLC
int			num_errores_conexion;		   // numero de errores de timeout desde la ultima consulta de errores por la web


// ------------------------------------------------------
// Control de consumo
// ------------------------------------------------------
float			average = 		0.0;
int			num_samples=		100;
#define 		kCONSUMO_NULO 		(1023/2)   //Valor del ACS712 que indica consumo 0
#define 		kTOLERANCIA 		30	   //Diferencia con el consumo nulo a partir de la cual consideramos que se esta consumiendo algo
#define			kPIN_SENSOR 		16	   //Pines para los sensores de corriente ACS712
int			consumo = 		0;	   //Consumo de las farolas


// ------------------------------------------------------
// Control de reles
// ------------------------------------------------------
#define 		kN_FAROLAS		3	   //Numero de farolas
#define			kAPAGADA		1	   //La placa de reles funciona asi: L=ON, H=OFF
#define			kENCENDIDA		0
#define			kMAX_HORAS		10	   //Numero maximo de horas que permaneceran encendidas las farolas
unsigned long		hora_encendido;			   //Instante en el que se encienden las luces (contador de inicio)
int			pin_farola[kN_FAROLAS]=	{6,2,3};   // Pines que controlaran los reles de las farolas
int			estado_farola[kN_FAROLAS] = 	{kAPAGADA,kAPAGADA,kAPAGADA};   // Estados iniciales de las farolas

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

  hora_encendido=0;
  for(i=0;i<kN_FAROLAS;i++)
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

  num_errores_conexion=0;
  ultima_conexion=millis();
  conexion_actual=ultima_conexion;
  pinMode(kPIN_PLC,OUTPUT);			//Por defecto esta normalmente cerrado (permite que funcione el PLC)
  Ethernet.begin(mac,ip,gateway,subnet);
  server.begin();

  wdt_enable(WDTO_8S);
}



// ------------------------------------------------------
// BUCLE PRINCIPAL
// ------------------------------------------------------
void loop()
{
  bool encendidas=false;

  wdt_reset();

  // A) Comprobamos los comandos via HTTP
  EthernetClient client = server.available();
  if(client)
  {
     ultima_conexion=conexion_actual;
     conexion_actual=millis();

     wdt_reset();
     readHttp(client,buffer);
     wdt_reset();
  }

  // B1) Si se han producido demasiados errores en las lecturas de las sondas reseteamos el arduino o
  if(lecturas_erroneas==6)
    softReset();

  // B2) Si se ha cumplido el tiempo maximo sin conexiones desde el nodo central, suponemos que el PLC se ha bloqueado y lo reiniciamos
  //     ademas, si la diferencia entre los dos instantes es demasiado alta, sera porque ha habido un overflow en millis y lo ignoramos
  if(abs(ultima_conexion-conexion_actual)>kTIMEOUT_JDOMO*1000*3)
    ultima_conexion=conexion_actual=millis();
  else if(abs(ultima_conexion-conexion_actual)>kTIMEOUT_JDOMO*1000)
  {
     digitalWrite(kPIN_PLC,HIGH);
     delay(500);
     digitalWrite(kPIN_PLC,LOW);

     ultima_conexion=conexion_actual=millis();
     num_errores_conexion++;
  }

  // C) Comprobamos el tiempo maximo de encendido de las farolas (1 h = 3600000 ms)
  if(hora_encendido!=0 && abs(millis()-hora_encendido)>=kMAX_HORAS*3600000)
  {
    for(int i=0;i<kN_FAROLAS;i++)
      apagar_farola(i);
    hora_encendido=0;
  }

  // Si se ha encendido una farola y no se ha activado el contador de tiempo maximo de encendido, lo activamos
  if(hora_encendido==0)
  {
     for(int i=0;i<kN_FAROLAS;i++)
        if(estado_farola[i]==kENCENDIDA)
           hora_encendido=millis();
  }
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

#ifdef DEBUG
   client.print(buffer);
   client.println("<br/>");
#endif


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
#ifdef DEBUG
           client.print("Args: [<i>"); client.println(token); client.println("</i>]<br/>");
#endif

           for (str2 = token, subtoken=token; subtoken!=NULL; str2 = NULL) 
           {
             subtoken = strtok_r(str2, "?/&", &saveptr2);
             if (subtoken != NULL)
             {
#ifdef DEBUG
               client.print(" --> "); client.println(subtoken); client.println("<br/>");
#endif

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
#ifdef DEBUG
                  client.print("  * "); client.println(subsubtoken); client.println("<br/>");
#endif
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
#ifdef DEBUG
           client.println("<br/>");
#endif
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

   client.print("power:");
   client.println(average/num_samples);
   client.print("<br/>");

   client.print("consumo:");
   client.print(abs(kCONSUMO_NULO-average/num_samples));
   client.println("<br/>");

   client.print("current:");
   client.println(abs(0.0264*(average/num_samples-kCONSUMO_NULO)));
   client.println("<br/>");

   client.print("watts:");
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

   client.print("errores_timeout_jdomo:");
   client.println(num_errores_conexion);
   client.println("<br/>");

   client.println("</html>");
}


// ------------------------------------------------------
// Ejecuta la accion correspondiente segun los valores
// ------------------------------------------------------
void processData(EthernetClient client, char* key, char* value)
{
   int valor=0;

#ifdef DEBUG
   client.print("P key: [");
   if(key!=NULL)
      client.print(key);
   client.print("] value: [");
   if(value!=NULL)
      client.print(value);
   client.println("]<br/>");
#endif

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

      if(strcmp(key,"encender")==0)
      {
         for(int i=0;i<kN_FAROLAS;i++)
            encender_farola(i);
      }

      if(strcmp(key,"apagar")==0)
      {
         for(int i=0;i<kN_FAROLAS;i++)
           apagar_farola(i);
      }

      if(strcmp(key,"demo")==0)
      {
         // Apagamos todas las farolas
         for(int i=0;i<kN_FAROLAS;i++)
           apagar_farola(i);

         // Encendemos todas las farolas
         for(int i=0;i<kN_FAROLAS;i++)
           encender_farola(i);

         // Apagamos todas las farolas, una cada 500ms
         for(int i=0;i<kN_FAROLAS;i++)
         {
           apagar_farola(i);
           wdt_reset();
           delay(500);
         }

         // Encendemos las farolas, y la apagamos, en sentido inverso
         for(int i=kN_FAROLAS-1;i>=0;i--)
         {
           encender_farola(i);
           wdt_reset();
           delay(500);
           apagar_farola(i);
         }

         // Efecto coche fantastico
         for(int j=0;j<8;j++)
         {
            encender_farola(1);
            delay(100);
            apagar_farola(1);

            encender_farola(2);
            delay(100);
            apagar_farola(2);

            encender_farola(3);
            delay(200);
            apagar_farola(3);

            encender_farola(2);
            delay(100);
            apagar_farola(2);

            encender_farola(1);
            delay(100);

            wdt_reset();
         }

         //Apagamos todas las farolas
         for(int i=0;i<kN_FAROLAS;i++)
           apagar_farola(i);
      }
   }
}



// ------------------------------------------------------
// Auxiliar: Apaga la farola indicada
// ------------------------------------------------------
void apagar_farola(int n_farola)
{
   if(n_farola>=0 && n_farola<kN_FAROLAS)
   {
     estado_farola[n_farola]=kAPAGADA;
     digitalWrite(pin_farola[n_farola],kAPAGADA);
   }
}


// ------------------------------------------------------
// Auxiliar: Enciende la farola indicada
// ------------------------------------------------------
void encender_farola(int n_farola)
{
   if(n_farola>=0 && n_farola<kN_FAROLAS)
   {
     estado_farola[n_farola]=kENCENDIDA;
     digitalWrite(pin_farola[n_farola],kENCENDIDA);
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
