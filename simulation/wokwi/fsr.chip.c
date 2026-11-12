// FSR 402 - حساس ضغط للمحاكاة في Wokwi
// يطلع جهد على OUT حسب الضغط: 0% = 0V ، 100% = 3.3V
// (مثل FSR موصول مع مقاومة 10k - في المحاكاة ما تحتاج المقاومة)
#include "wokwi-api.h"
#include <stdlib.h>

typedef struct {
  pin_t    pin_out;
  uint32_t pressure_attr;
} chip_state_t;

static void chip_timer_event(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  float pressure = attr_read_float(chip->pressure_attr);   // 0..100
  if (pressure < 0) pressure = 0;
  if (pressure > 100) pressure = 100;
  pin_dac_write(chip->pin_out, 3.3f * pressure / 100.0f);
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));
  chip->pin_out       = pin_init("OUT", ANALOG);
  chip->pressure_attr = attr_init_float("pressure", 0);

  const timer_config_t timer_config = {
    .callback  = chip_timer_event,
    .user_data = chip,
  };
  timer_t timer = timer_init(&timer_config);
  timer_start(timer, 10000, true);   // تحديث كل 10ms
}
