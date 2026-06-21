#define LDR 4

int ldr_pin = 0;

void setup() {
  Serial.begin(9600);
  pinMode(LDR,INPUT);

}

void loop() {
  // put your main code here, to run repeatedly:
  ldr_pin = digitalRead(LDR);

  if(!ldr_pin){
    Serial.println("LDR High");
  }else{
    Serial.println("LDR Low");
  }
  

}
