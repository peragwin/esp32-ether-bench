#include <Arduino.h>
#include <ETH.h>
#include <esp_eth.h>
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define NUMSTRIPS 7 // 8
#define NUM_LEDS_PER_STRIP 510
#define SNAKEPATTERN 0
#define ALTERNATEPATTERN 0
#include <I2SClockBasedLedDriver.h>

#include <ArtnetWiFi.h>
#define PIXELS_PER_UNIVERSE 170

struct CRGB
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

int LED_PINS[] = {32, 33, 14, 12, 13, 15, 2}; // 4
const int CLOCK_PIN = 4;                      // 16
I2SClockBasedLedDriver led_driver;

ArtnetWiFiReceiver artnet;
void handle_frame();

struct CRGBA
{
  uint8_t a;
  uint8_t r;
  uint8_t g;
  uint8_t b;

  CRGBA() {}
  CRGBA(CRGB &c)
  {
    const uint16_t max_brightness = 31;
    uint16_t brightness = ((((uint16_t)max(max(c.r, c.g), c.b) + 1) * max_brightness - 1) >> 8) + 1;
    a = 0xE0 | (brightness & 31);
    r = (max_brightness * c.r + (brightness >> 1)) / brightness;
    g = (max_brightness * c.g + (brightness >> 1)) / brightness;
    b = (max_brightness * c.b + (brightness >> 1)) / brightness;
  }
};

#define NUM_PIXELS (NUMSTRIPS * NUM_LEDS_PER_STRIP)
CRGB pixel_data[NUM_PIXELS];
CRGBA hdr_pixel_data[NUM_PIXELS];

static bool eth_connected = false;

TaskHandle_t udp_task;
TaskHandle_t tcp_server_task_h;
TaskHandle_t artnet_task_hdl;

SemaphoreHandle_t artnet_frame_ready_sem = NULL;

static void artnet_task(void *pv_params)
{
  // artnet.begin(NUM_PIXELS, 170);
  artnet.subscribe([](const uint32_t universe, const uint8_t *data, const uint16_t size)
                   {
    // if (universe == 0xffff) {
    //   // trigger frame_sync
    //   xTaskNotify()..
    //   handle_frame();
    //   return;
    // }
    auto offset = universe * PIXELS_PER_UNIVERSE * sizeof(CRGB);
    if (offset + size >= sizeof(pixel_data)) {
      return;
    }
    memcpy(((uint8_t*)pixel_data)+offset, data, size); });

  artnet.begin();

  while (1)
  {
    artnet.parse();
    // Serial.println("parsed");
    // yield();
    // taskYIELD();
  }
}

static void udp_server_task(void *pvParameters)
{
  char rx_buffer[1460];
  char addr_str[128];
  int addr_family;
  int ip_protocol;

  int packet_count = 0;
  long byte_count = 0;
  uint32_t last_report = 0;

  uint8_t dummy[1024] = {0xaa};

  while (1)
  {

    // #ifdef CONFIG_EXAMPLE_IPV4
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(1337);
    //         addr_family = AF_INET;
    //         ip_protocol = IPPROTO_IP;
    //         inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);
    // #else // IPV6
    //         struct sockaddr_in6 destAddr;
    //         bzero(&destAddr.sin6_addr.un, sizeof(destAddr.sin6_addr.un));
    //         destAddr.sin6_family = AF_INET6;
    //         destAddr.sin6_port = htons(PORT);
    //         addr_family = AF_INET6;
    //         ip_protocol = IPPROTO_IPV6;
    //         inet6_ntoa_r(destAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
    // #endif

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDPLITE);
    if (sock < 0)
    {
      ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
      break;
    }
    ESP_LOGI(TAG, "Socket created");

    int err = bind(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err < 0)
    {
      ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    }
    ESP_LOGI(TAG, "Socket binded");

    struct timeval timeout = {0};
    timeout.tv_sec = 1;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    fcntl(sock, F_SETFL, O_NONBLOCK);

    while (1)
    {

      // ESP_LOGI(TAG, "Waiting for data");
      struct sockaddr_in6 sourceAddr; // Large enough for both IPv4 or IPv6
      socklen_t socklen = sizeof(sourceAddr);

      // int len = recvfrom(sock, rx_buffer, 1460, MSG_DONTWAIT, (struct sockaddr *)&sourceAddr, &socklen);
      // int len = 1024;
      // memcpy(rx_buffer, dummy, 1024);
      int len = recv(sock, rx_buffer, 1460, MSG_DONTWAIT);

      // Error occured during receiving
      if (len < 0)
      {
        // ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
        continue;
      }
      // Data received
      // Get the sender's ip address as string
      // if (sourceAddr.sin6_family == PF_INET)
      // {
      //   inet_ntoa_r(((struct sockaddr_in *)&sourceAddr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
      // }
      // else if (sourceAddr.sin6_family == PF_INET6)
      // {
      //   inet6_ntoa_r(sourceAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
      // }

      rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...
      // ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
      // ESP_LOGI(TAG, "%s", rx_buffer);

      // int err = sendto(sock, rx_buffer, len, 0, (struct sockaddr *)&sourceAddr, sizeof(sourceAddr));
      // if (err < 0)
      // {
      //   ESP_LOGE(TAG, "Error occured during sending: errno %d", errno);
      //   break;
      // }

      packet_count++;
      byte_count += len;

      auto now = xTaskGetTickCount();
      if ((now - last_report) > 1000)
      {
        // Serial.println(last_report - now);
        last_report = now;
        Serial.printf("packets/sec: %d\r\n", packet_count);
        Serial.printf("bytes/sec: %d\r\n", byte_count);
        packet_count = 0;
        byte_count = 0;
      }
    }

    if (sock != -1)
    {
      ESP_LOGE(TAG, "Shutting down socket and restarting...");
      shutdown(sock, 0);
      close(sock);
    }
  }
  vTaskDelete(NULL);
}

void WiFiEvent(WiFiEvent_t event)
{
  switch (event)
  {
  case SYSTEM_EVENT_ETH_START:
  case ARDUINO_EVENT_ETH_START:
    Serial.println("ETH Started");
    // set eth hostname here
    ETH.setHostname("esp32-ethernet");
    break;
  case SYSTEM_EVENT_ETH_CONNECTED:
  case ARDUINO_EVENT_ETH_CONNECTED:
    Serial.println("ETH Connected");
    break;
  case SYSTEM_EVENT_ETH_GOT_IP:
  case ARDUINO_EVENT_ETH_GOT_IP:
    Serial.print("ETH MAC: ");
    Serial.print(ETH.macAddress());
    Serial.print(", IPv4: ");
    Serial.print(ETH.localIP());
    if (ETH.fullDuplex())
    {
      Serial.print(", FULL_DUPLEX");
    }
    Serial.print(", ");
    Serial.print(ETH.linkSpeed());
    Serial.println("Mbps");
    eth_connected = true;

    xTaskCreatePinnedToCore(udp_server_task, "udp_task", 8 << 10, nullptr, 4, &udp_task, 1);
    // xTaskCreatePinnedToCore(tcp_server_task, "tcp_task", 20000, nullptr, 4, &tcp_server_task_h, 0);

    // xTaskCreatePinnedToCore(artnet_task, "artnet_rx", 16 << 10, nullptr, 4, &artnet_task_hdl, 0);

    // Serial.println("UDP listening on port 1337");
    break;
  case SYSTEM_EVENT_ETH_DISCONNECTED:
  case ARDUINO_EVENT_ETH_DISCONNECTED:
    Serial.println("ETH Disconnected");
    eth_connected = false;
    break;
  case SYSTEM_EVENT_ETH_STOP:
  case ARDUINO_EVENT_ETH_STOP:
    Serial.println("ETH Stopped");
    eth_connected = false;
    break;
  default:
    break;
  }
}

void setup()
{
  Serial.begin(115200);

  led_driver.initled((uint8_t *)hdr_pixel_data, LED_PINS, CLOCK_PIN, NUMSTRIPS, NUM_LEDS_PER_STRIP);

  WiFi.mode(WIFI_MODE_NULL);
  WiFi.onEvent(WiFiEvent);
  // ETH.begin(1, 17); // LAN8720A
  ETH.begin(1, 5, 23, 18, ETH_PHY_IP101);

  // artnet + udp stack is not yielding properly :(
  disableCore0WDT();
}

void loop()
{
  return;
  auto now = millis();
  handle_frame();
  auto e = millis() - now;
  delay(max(1000 / 70 - (int)e, 0));
}

void handle_frame()
{
  // yield();

  static int fc = 0;
  static unsigned long last_time = millis();

  // if (xSemaphoreTake(artnet_frame_ready_sem, 0) != pdTRUE)
  // {
  //   return;
  // }

  // fill_rainbow(pixel_data, NUM_PIXELS, fc, 1);
  // auto s = sin8(fc / 16);
  // for (size_t i = 0; i < NUM_PIXELS; i++)
  // {
  //   pixel_data[i] %= s;
  // }

  // auto t = micros();
  for (size_t i = 0; i < NUM_PIXELS; i++)
  {
    hdr_pixel_data[i] = CRGBA(pixel_data[i]);
    // auto c = CRGBA(pixel_data[i]);
    // for (size_t j = 0; j < NUMSTRIPS; j++)
    // {
    //   hdr_pixel_data[NUM_LEDS_PER_STRIP * j + i] = c;
    // }
  }
  // Serial.printf("hdr: %dus\r\n", micros() - t);
  led_driver.showPixels();

  if (fc++ % 256 == 0)
  {
    auto now = millis();
    auto fps = 256000.0 / (now - last_time);
    Serial.printf("FPS: %0.1f\r\n", fps);
    // auto hc = hdr_pixel_data[random(NUM_PIXELS)];
    // Serial.printf("CHDR: %d %d %d %d\r\n", hc.a, hc.r, hc.g, hc.b);
    last_time = now;
  }
}
