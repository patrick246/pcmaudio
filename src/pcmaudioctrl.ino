#include <IRremote.h>

const int receivePin = 11;
IRrecv ir(receivePin);

void setup() {
  Serial.begin(9600);
  ir.enableIRIn();
}

void ir_handler(decode_results& result)
{
  switch(result.value)
  {
    case 0xD7E84B1B: // Next
      Serial.println("next");
    break;
    case 0x20FE4DBB: // Play/Pause
      Serial.println("togglepp");
    break;
    case 0xE5CFBD7F: // Stop
      Serial.println("stop");
    break;
  }
}

void loop() {
  decode_results res;
  if(ir.decode(&res))
  {
    ir_handler(res);
    ir.resume();
  }
}
