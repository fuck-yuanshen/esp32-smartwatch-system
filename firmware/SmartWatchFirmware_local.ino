#include "SmartWatchApp.h"
#include "settings.h"

SmartWatchApp watch(settings);

void setup() {
  watch.init();
}

void loop() {
  watch.tick();
}
