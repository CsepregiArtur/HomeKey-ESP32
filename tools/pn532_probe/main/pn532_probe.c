/*
 * PN532 SPI pinout / connection probe.
 *
 * Standalone ESP32-C3 Mini diagnostic for a PN532 connected over SPI.
 *
 * This project uses only ESP-IDF GPIO/SPI APIs and local PN532 test frames.
 *
 * Tests:
 *   [0] Optional isolated GPIO loopback; a detected jumper selects loopback-only mode.
 *   [1] MISO idle-state check. High impedance is normal while SS is high.
 *   [2] Repeated PN532 status-byte reads.
 *   [3] GetFirmwareVersion ACK and response-frame validation.
 *   [4] SPI clock sweep using full valid command/response exchanges.
 *   [5] SAMConfiguration ACK and response-frame validation.
 *
 * BUILD / RUN
 *   ./scripts/build_pn532_probe.sh flash /dev/cu.usbmodemXXXX
 *   ./scripts/build_pn532_probe.sh monitor /dev/cu.usbmodemXXXX
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------------------------------------------------------------------------
// Fixed ESP32-C3 Mini wiring: SS=7, SCK=4, MISO=5, MOSI=6.
// ---------------------------------------------------------------------------
#if !CONFIG_IDF_TARGET_ESP32C3
#error "This standalone diagnostic only supports ESP32-C3"
#endif
#define PIN_SS 7
#define PIN_SCK 4
#define PIN_MISO 5
#define PIN_MOSI 6

#define SPI_HOST_USED SPI2_HOST
#define PROBE_SPI_CLOCK_HZ 100000

// PN532 SPI command bytes. Data is transferred LSB-first.
#define CMD_STATUS_READ 0x02
#define CMD_DATA_READ 0x03
#define CMD_DATA_WRITE 0x01

static const uint8_t PN532_ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// GetFirmwareVersion: preamble, startcode, LEN=2, LCS, TFI=D4, CMD=02, DCS, postamble
static const uint8_t GET_FW[9] = {0x00, 0x00, 0xFF, 0x02, 0xFE,
                                  0xD4, 0x02, 0x2A, 0x00};

static spi_device_handle_t s_spi = NULL;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void hexdump(const char *label, const uint8_t *b, size_t n) {
  printf("  %-12s", label);
  for (size_t i = 0; i < n; ++i) {
    printf(" %02X", b[i]);
  }
  printf("\n");
}

static void cs_set(int level) { gpio_set_level(PIN_SS, level); }

static void xfer(const uint8_t *tx, size_t n, uint8_t *rx) {
  if (!s_spi) {
    return;
  }
  spi_transaction_t t = {0};
  t.length = n * 8;
  if (rx) {
    t.rxlength = n * 8;
  }
  t.tx_buffer = tx;
  t.rx_buffer = rx;
  if (spi_device_transmit(s_spi, &t) != ESP_OK) {
    printf("  (SPI transmit failed)\n");
  }
}

// ---------------------------------------------------------------------------
// Phase 0: bit-bang loopback -- the SPI peripheral is NOT involved.
//
// The SPI loopback drives the loopback through the peripheral, so a failure leaves
// three suspects: the pins, the jumper, or the SPI peripheral/pin-mux. This test
// removes the third one. GPIO_MOSI is driven by hand, GPIO_MISO is read by hand,
// and MISO gets an internal pull-up so the "no jumper" case is deterministic
// instead of floating:
//
//   no jumper        -> MISO is pulled to 1 no matter what MOSI does
//   good jumper      -> MISO follows MOSI, so it reads 0 then 1
//   wire to GND      -> MISO reads 0 then 0
//   inverted reading -> the jumper is on the wrong pins
// ---------------------------------------------------------------------------
static bool phase_bitbang_loopback(void) {
  printf("\n[0] Optional MCU GPIO loopback detection\n");
  printf("    For an isolated test, unplug the PN532 and jumper MISO (GPIO%d) to\n",
    PIN_MISO);
  printf("    MOSI (GPIO%d). A detected jumper selects loopback-only mode.\n",
    PIN_MOSI);

  // De-select the module before touching MOSI, so nothing fights us even if
  // the module is still plugged in.
  gpio_config_t ss = {0};
  ss.pin_bit_mask = 1ULL << PIN_SS;
  ss.mode = GPIO_MODE_OUTPUT;
  ss.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&ss);
  cs_set(1);

  gpio_config_t out = {0};
  out.pin_bit_mask = 1ULL << PIN_MOSI;
  out.mode = GPIO_MODE_OUTPUT;
  out.pull_up_en = GPIO_PULLUP_DISABLE;
  out.pull_down_en = GPIO_PULLDOWN_DISABLE;
  out.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&out);

  gpio_config_t in = {0};
  in.pin_bit_mask = 1ULL << PIN_MISO;
  in.mode = GPIO_MODE_INPUT;
  in.pull_up_en = GPIO_PULLUP_ENABLE;  // makes "nothing connected" read 1, not noise
  in.pull_down_en = GPIO_PULLDOWN_DISABLE;
  in.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&in);

  esp_rom_delay_us(200);
  gpio_set_level(PIN_MOSI, 0);
  esp_rom_delay_us(500);
  int lo = gpio_get_level(PIN_MISO);
  gpio_set_level(PIN_MOSI, 1);
  esp_rom_delay_us(500);
  int hi = gpio_get_level(PIN_MISO);

  printf("    MOSI driven LOW  -> MISO reads %d  (a good jumper gives 0)\n", lo);
  printf("    MOSI driven HIGH -> MISO reads %d  (a good jumper gives 1)\n", hi);

  if (lo == 0 && hi == 1) {
    printf("    => PASS. GPIO%d drives, GPIO%d reads, and the jumper connects them.\n",
           PIN_MOSI, PIN_MISO);
  } else if (lo == 1 && hi == 1) {
        printf("    => No loopback jumper detected; normal PN532 diagnostics will continue.\n");
        printf("       For the MCU-only test, unplug the module and bridge GPIO%d to GPIO%d.\n",
          PIN_MISO, PIN_MOSI);
  } else if (lo == 0 && hi == 0) {
    printf("    => MISO IS STUCK LOW. Shorted to GND, or GPIO%d is dead.\n", PIN_MISO);
  } else {
    printf("    => INVERTED. The jumper looks miswired rather than broken.\n");
  }

  // Leave both pins floating again so phase [1]'s pull test stays meaningful.
  gpio_reset_pin(PIN_MOSI);
  gpio_reset_pin(PIN_MISO);
  esp_rom_delay_us(200);
  return lo == 0 && hi == 1;
}

// Read a pin with a chosen internal pull. Used only by phase [1], before the SPI
// driver takes the pin over.
static int read_with_pull(gpio_num_t pin, gpio_pullup_t up, gpio_pulldown_t down) {
  gpio_config_t c = {0};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = up;
  c.pull_down_en = down;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);
  // Let the internal pull (roughly 45 kOhm) charge the line and any stray
  // capacitance before sampling; 2 ms is ample for a jumper wire.
  esp_rom_delay_us(2000);
  return gpio_get_level(pin);
}

// ---------------------------------------------------------------------------
// Phase 1: is anything driving MISO?
// ---------------------------------------------------------------------------
// Returns true when the line follows whichever pull is enabled, i.e. it is
// floating and nothing on the board is driving it.
static bool phase_miso_floating(void) {
  printf("\n[1] Is anything driving MISO (GPIO%d)?\n", PIN_MISO);

  int with_up = read_with_pull((gpio_num_t)PIN_MISO, GPIO_PULLUP_ENABLE,
                               GPIO_PULLDOWN_DISABLE);
  int with_down = read_with_pull((gpio_num_t)PIN_MISO, GPIO_PULLUP_DISABLE,
                                 GPIO_PULLDOWN_ENABLE);

  printf("  MISO with internal pull-up   -> %d\n", with_up);
  printf("  MISO with internal pull-down -> %d\n", with_down);

  if (with_up == 1 && with_down == 0) {
    printf("  => HIGH-Z: the line follows the pull, so nothing is driving it now.\n");
    printf("     This is EXPECTED while SS is high: an unselected PN532 tri-states\n");
    printf("     MISO on purpose. It only indicates a fault if the line also stays\n");
    printf("     idle during a transfer, which phases [2] and [3] check.\n");
    return true;
  }
  if (with_up == with_down) {
    printf("  => DRIVEN %s by something on the board.\n", with_up ? "HIGH" : "LOW");
    return false;
  }
  printf("  => INCONCLUSIVE (unexpected combination) - suspect a bad ground.\n");
  return false;
}

// ---------------------------------------------------------------------------
// Phase 2: raw status reads
// ---------------------------------------------------------------------------
static uint8_t phase_status_reads(void) {
  printf("\n[2] Status read (send 0x02, read one byte), 8 times:\n");

  int saw_rdy = 0;
  uint8_t last = 0;
  for (int i = 0; i < 8; ++i) {
    uint8_t cmd = CMD_STATUS_READ;
    uint8_t dummy = 0x00;
    uint8_t st = 0;

    cs_set(0);
    esp_rom_delay_us(100);  // PN532 needs the falling edge to settle
    xfer(&cmd, 1, NULL);
    xfer(&dummy, 1, &st);
    cs_set(1);

    printf("  status[%d] = 0x%02X   (RDY bit = %d)\n", i, st, st & 0x01);
    if (st & 0x01) {
      ++saw_rdy;
    }
    last = st;
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (saw_rdy == 0) {
    printf("  => RDY stayed low; this bit alone does not indicate an SPI failure.\n");
  } else if (saw_rdy < 8) {
    printf("  => RDY came and went (%d/8). Unstable line.\n", saw_rdy);
  } else {
    printf("  => RDY asserted on every read.\n");
  }
  return last;
}

// ---------------------------------------------------------------------------
// Runs only after phase [0] detects the isolated MISO-to-MOSI jumper.
// It checks the SPI peripheral and pin mux without involving the PN532.
// ---------------------------------------------------------------------------
static void phase_loopback(void) {
  static const uint8_t pattern[4] = {0xA5, 0x5A, 0x3C, 0xC3};
  uint8_t got[4] = {0};

  printf("\n[SPI LOOPBACK] MCU-side test\n");

  cs_set(1);  // keep the PN532 deselected so it cannot drive MISO
  esp_rom_delay_us(100);
  xfer(pattern, sizeof(pattern), got);
  cs_set(1);

  printf("    sent:       %02X %02X %02X %02X\n", pattern[0], pattern[1], pattern[2],
         pattern[3]);
  printf("    read back:  %02X %02X %02X %02X\n", got[0], got[1], got[2], got[3]);

  bool same = true;
  for (size_t i = 0; i < sizeof(pattern); ++i) {
    if (got[i] != pattern[i]) {
      same = false;
    }
  }

  if (same) {
    printf("    => PASS. The MCU, the SPI peripheral and GPIO%d/GPIO%d/GPIO%d all\n",
           PIN_MISO, PIN_MOSI, PIN_SCK);
    printf("       pass the isolated loopback test. Reconnect the PN532 for its test.\n");
  } else {
    printf("    => NO LOOPBACK. Either the jumper is missing, or this MCU side is\n");
    printf("       faulty. Re-check the jumper first; then suspect the pin wiring.\n");
  }
}

// ---------------------------------------------------------------------------
// Phase 3 + 4: GetFirmwareVersion and an SPI clock sweep
// ---------------------------------------------------------------------------

// One GetFirmwareVersion exchange. Returns true when ACK and response are valid.
// `verbose` prints the raw bytes; the clock sweep uses the quiet form so a long
// sweep does not bury the result in hex.
static bool fw_exchange(bool verbose, uint8_t *resp_out, size_t resp_len) {
  uint8_t wr = CMD_DATA_WRITE;
  uint8_t rd = CMD_DATA_READ;
  uint8_t dummy = 0x00;
  uint8_t ack[6] = {0};
  uint8_t resp[13] = {0};

  // Send the command frame. CS stays low for the command byte plus the whole frame.
  cs_set(0);
  esp_rom_delay_us(100);
  xfer(&wr, 1, NULL);
  xfer(GET_FW, sizeof(GET_FW), NULL);
  cs_set(1);

  // Give the PN532 time to process before polling for the reply.
  vTaskDelay(pdMS_TO_TICKS(50));

  // ACK read: the 0x03 command byte, then the six ACK bytes.
  cs_set(0);
  esp_rom_delay_us(100);
  xfer(&rd, 1, NULL);
  for (size_t i = 0; i < 6; ++i) {
    xfer(&dummy, 1, &ack[i]);
  }
  cs_set(1);

  bool ok = memcmp(ack, PN532_ACK, sizeof(PN532_ACK)) == 0;
  if (verbose) {
    hexdump("ACK:", ack, 6);
  }
  if (!ok) {
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(50));

  // Response read: 0x03 again, then the payload.
  cs_set(0);
  esp_rom_delay_us(100);
  xfer(&rd, 1, NULL);
  for (size_t i = 0; i < sizeof(resp); ++i) {
    xfer(&dummy, 1, &resp[i]);
  }
  cs_set(1);

  static const uint8_t RESPONSE_PREFIX[7] = {0x00, 0x00, 0xFF, 0x06,
                                              0xFA, 0xD5, 0x03};
  uint8_t checksum = 0;
  for (size_t i = 5; i < 11; ++i) {
    checksum += resp[i];
  }
  bool response_ok = memcmp(resp, RESPONSE_PREFIX, sizeof(RESPONSE_PREFIX)) == 0 &&
                     (uint8_t)(checksum + resp[11]) == 0 && resp[12] == 0x00;

  if (verbose) {
    hexdump("response:", resp, sizeof(resp));
    //   [0]      preamble 00
    //   [1][2]   startcode 00 FF
    //   [3]      LEN
    //   [4]      LCS
    //   [5]      TFI 0xD5 (PN532 -> host)
    //   [6]      command 0x03 (GetFirmwareVersion response)
    //   [7][8][9][10] IC, Ver, Rev, Support
    //   [11]     DCS
    //   [12]     postamble 00
    if (response_ok) {
      printf("  => chip IC=0x%02X  firmware %d.%d  support=0x%02X\n", resp[7],
             resp[8], resp[9], resp[10]);
      printf("     (IC 0x32 = PN532, so the reader is genuine and answering)\n");
    } else {
      printf("  => invalid GetFirmwareVersion response frame\n");
    }
  }

  if (resp_out) {
    size_t n = resp_len < sizeof(resp) ? resp_len : sizeof(resp);
    memcpy(resp_out, resp, n);
  }
  return response_ok;
}

// Re-add the SPI device at a different clock. Changing the speed in place is not
// possible, and removing/re-adding is exactly what a driver would do to retune.
static bool set_clock(int hz) {
  if (s_spi) {
    spi_bus_remove_device(s_spi);
    s_spi = NULL;
  }
  spi_device_interface_config_t dev = {0};
  dev.mode = 0;
  dev.clock_speed_hz = hz;
  dev.spics_io_num = -1;  // CS driven manually, as the real firmware does
  dev.flags = SPI_DEVICE_BIT_LSBFIRST;
  dev.queue_size = 1;
  return spi_bus_add_device(SPI_HOST_USED, &dev, &s_spi) == ESP_OK;
}

// Phase 5: send an independent SAMConfiguration command to the PN532.
static void phase_sam_configuration(void) {
  printf("\n[5] SAMConfiguration test at %d Hz\n", PROBE_SPI_CLOCK_HZ);
  if (!set_clock(PROBE_SPI_CLOCK_HZ)) {
    printf("  could not configure probe SPI clock\n");
    return;
  }

  uint8_t wr = CMD_DATA_WRITE;
  uint8_t rd = CMD_DATA_READ;
  uint8_t dummy = 0x00;
  // Match the working setup: normal mode, timeout 0x14, IRQ enabled.
  static const uint8_t SAM_CONFIG[12] = {0x00, 0x00, 0xFF, 0x05, 0xFB, 0xD4,
                                         0x14, 0x01, 0x14, 0x01, 0x02, 0x00};
  uint8_t ack[6];

  cs_set(0);
  esp_rom_delay_us(500);
  xfer(&wr, 1, NULL);
  xfer(SAM_CONFIG, sizeof(SAM_CONFIG), NULL);
  cs_set(1);
  vTaskDelay(pdMS_TO_TICKS(50));

  cs_set(0);
  esp_rom_delay_us(100);
  xfer(&rd, 1, NULL);
  for (size_t i = 0; i < sizeof(ack); ++i) {
    xfer(&dummy, 1, &ack[i]);
  }
  cs_set(1);

  hexdump("SAM ACK:", ack, sizeof(ack));
  if (memcmp(ack, PN532_ACK, sizeof(ack)) != 0) {
    printf("  => invalid ACK; response read skipped\n");
    return;
  }

  static const uint8_t EXPECTED_RESPONSE[9] = {0x00, 0x00, 0xFF, 0x02, 0xFE,
                                                0xD5, 0x15, 0x16, 0x00};
  uint8_t response[sizeof(EXPECTED_RESPONSE)] = {0};
  vTaskDelay(pdMS_TO_TICKS(50));
  cs_set(0);
  esp_rom_delay_us(100);
  xfer(&rd, 1, NULL);
  for (size_t i = 0; i < sizeof(response); ++i) {
    xfer(&dummy, 1, &response[i]);
  }
  cs_set(1);

  hexdump("SAM response:", response, sizeof(response));
  printf("  => %s\n", memcmp(response, EXPECTED_RESPONSE, sizeof(response)) == 0
                         ? "valid SAMConfiguration response"
                         : "invalid SAMConfiguration response");
}

// Sweep the clock and return the fastest rate that passed every attempt (0 if none).
static int phase_clock_sweep(void) {
  static const int rates[] = {25000, 50000, 100000, 250000, 500000,
                              1000000, 2000000, 4000000};
  int highest_ok = 0;

  printf("\n[4] SPI clock sweep (reference: %d Hz)\n", PROBE_SPI_CLOCK_HZ);
  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
    if (!set_clock(rates[i])) {
      printf("  %7d Hz : could not configure\n", rates[i]);
      continue;
    }
    // Three attempts: a marginal link often works intermittently, and a single
    // pass would hide that.
    int passes = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (fw_exchange(false, NULL, 0)) {
        ++passes;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("  %7d Hz : %d/3 valid exchanges %s\n", rates[i], passes,
           passes == 3 ? "" : (passes == 0 ? "<-- FAILS" : "<-- UNRELIABLE"));
    if (passes == 3) {
      highest_ok = rates[i];
    }
  }
  return highest_ok;
}

static void verdict(bool exchange_ok, int highest_ok) {
  printf("\n================= RESULT =================\n");
  if (exchange_ok) {
    printf("PN532 GetFirmwareVersion returned a valid ACK and response frame.\n");
    if (highest_ok > 0) {
      printf("Fastest tested clock with 3/3 valid exchanges: %d Hz.\n", highest_ok);
    }
  } else {
    printf("No valid GetFirmwareVersion exchange completed at %d Hz.\n",
           PROBE_SPI_CLOCK_HZ);
    printf("Run the isolated C3 loopback mode first: unplug PN532 and bridge GPIO5 to GPIO6.\n");
    printf("If loopback passes, check PN532 power, SPI mode selection, pin labels, and ground.\n");
    printf("Wiring: SS=GPIO7, SCK=GPIO4, MISO=GPIO5, MOSI=GPIO6.\n");
  }
  printf("==========================================\n");
}

// ---------------------------------------------------------------------------

void app_main(void) {
  printf("\n\n==========================================\n");
  printf("        PN532 SPI probe\n");
  printf("==========================================\n");
  printf("Pins in use:  SS=GPIO%d  SCK=GPIO%d  MISO=GPIO%d  MOSI=GPIO%d\n", PIN_SS,
         PIN_SCK, PIN_MISO, PIN_MOSI);
  printf("SPI: SPI2_HOST, mode 0, LSB-first\n");

  bool loopback_mode = phase_bitbang_loopback();
  if (!loopback_mode) {
    (void)phase_miso_floating();
  } else {
    printf("\nLoopback setup detected; PN532 communication tests are skipped.\n");
  }

  spi_bus_config_t bus = {0};
  bus.mosi_io_num = PIN_MOSI;
  bus.miso_io_num = PIN_MISO;
  bus.sclk_io_num = PIN_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;

  esp_err_t err = spi_bus_initialize(SPI_HOST_USED, &bus, SPI_DMA_DISABLED);
  if (err != ESP_OK) {
    printf("\nSPI bus init FAILED: %s\n", esp_err_to_name(err));
    if (!loopback_mode) {
      verdict(false, 0);
    }
    return;
  }
  if (!set_clock(PROBE_SPI_CLOCK_HZ)) {
    printf("\nSPI add device FAILED\n");
    if (!loopback_mode) {
      verdict(false, 0);
    }
    return;
  }
  printf("\nSPI bus ready at %d Hz.\n", PROBE_SPI_CLOCK_HZ);

  if (loopback_mode) {
    phase_loopback();
    printf("\nRemove the jumper, reconnect the PN532, then reset for reader tests.\n");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }

  (void)phase_status_reads();

  printf("\n[3] GetFirmwareVersion round trip at %d Hz\n", PROBE_SPI_CLOCK_HZ);
  bool exchange_ok = fw_exchange(true, NULL, 0);
  if (!exchange_ok) {
    printf("  => GetFirmwareVersion exchange failed: ACK or response was invalid.\n");
  }

  int highest_ok = phase_clock_sweep();
  phase_sam_configuration();
  verdict(exchange_ok, highest_ok);

  printf("\nDone. Press the reset button to run the whole probe again.\n");
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
