#include <Arduino.h>
#include <FastLED.h>
#include <ETH.h>
#include <esp_eth.h>
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define NUMSTRIPS 8
#define NUM_LEDS_PER_STRIP 512
#define SNAKEPATTERN 0
#define ALTERNATEPATTERN 0
#include <I2SClockBasedLedDriver.h>

// struct CRGB
// {
//   uint8_t r;
//   uint8_t g;
//   uint8_t b;
// };

int LED_PINS[] = {32, 33, 14, 12, 13, 15, 2, 4};
const int CLOCK_PIN = 16;
I2SClockBasedLedDriver led_driver;

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

static void udp_server_task(void *pvParameters)
{
  char *rx_buffer;
  rx_buffer = (char *)malloc(16 << 10);
  char addr_str[128];
  int addr_family;
  int ip_protocol;

  int packet_count = 0;
  int byte_count = 0;
  uint32_t last_report = 0;

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
    timeout.tv_sec = 10;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (1)
    {

      // ESP_LOGI(TAG, "Waiting for data");
      struct sockaddr_in6 sourceAddr; // Large enough for both IPv4 or IPv6
      socklen_t socklen = sizeof(sourceAddr);

      int len = recvfrom(sock, rx_buffer, (16 << 10) - 1, 0, (struct sockaddr *)&sourceAddr, &socklen);

      // Error occured during receiving
      if (len < 0)
      {
        ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
        break;
      }
      // Data received
      else
      {
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

static void do_retransmit(const int sock)
{
  int len;
  char rx_buffer[16 << 10];

  static int packet_count = 0;
  static int byte_count = 0;
  static uint32_t last_report = 0;

  do
  {
    len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
    if (len < 0)
    {
      ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
    }
    else if (len == 0)
    {
      ESP_LOGW(TAG, "Connection closed");
    }
    else
    {
      rx_buffer[len] = 0; // Null-terminate whatever is received and treat it like a string
                          // ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);

      // send() can return less bytes than supplied length.
      // Walk-around for robust implementation.
      // int to_write = len;
      // while (to_write > 0)
      // {
      //   int written = send(sock, rx_buffer + (len - to_write), to_write, 0);
      //   if (written < 0)
      //   {
      //     ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
      //   }
      //   to_write -= written;
      // }

      packet_count++;
      byte_count += len;

      auto now = xTaskGetTickCount();
      if ((now - last_report) > 3000)
      {
        // Serial.println(last_report - now);
        last_report = now;
        Serial.printf("packets/sec: %d\r\n", packet_count / 3);
        Serial.printf("bytes/sec: %d\r\n", byte_count / 3);
        packet_count = 0;
        byte_count = 0;
      }
    }
  } while (len > 0);
}

#define PORT 1338
#define KEEPALIVE_IDLE 5
#define KEEPALIVE_INTERVAL 5
#define KEEPALIVE_COUNT 3

static void tcp_server_task(void *pvParameters)
{
  char addr_str[128];
  int addr_family = AF_INET; //(int)pvParameters;
  int ip_protocol = 0;
  int keepAlive = 1;
  int keepIdle = KEEPALIVE_IDLE;
  int keepInterval = KEEPALIVE_INTERVAL;
  int keepCount = KEEPALIVE_COUNT;
  struct sockaddr_storage dest_addr;

  if (addr_family == AF_INET)
  {
    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(PORT);
    ip_protocol = IPPROTO_TCP;
  }
#ifdef CONFIG_EXAMPLE_IPV6
  else if (addr_family == AF_INET6)
  {
    struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
    bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
    dest_addr_ip6->sin6_family = AF_INET6;
    dest_addr_ip6->sin6_port = htons(PORT);
    ip_protocol = IPPROTO_IPV6;
  }
#endif

  int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
  if (listen_sock < 0)
  {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    vTaskDelete(NULL);
    return;
  }
  int opt = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
  // Note that by default IPV6 binds to both protocols, it is must be disabled
  // if both protocols used at the same time (used in CI)
  setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

  ESP_LOGI(TAG, "Socket created");

  int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err != 0)
  {
    ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
    goto CLEAN_UP;
  }
  ESP_LOGI(TAG, "Socket bound, port %d", PORT);

  err = listen(listen_sock, 1);
  if (err != 0)
  {
    ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
    goto CLEAN_UP;
  }

  while (1)
  {

    ESP_LOGI(TAG, "Socket listening");

    struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
    socklen_t addr_len = sizeof(source_addr);
    int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
    if (sock < 0)
    {
      ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
      break;
    }

    // Set tcp keepalive option
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
    // Convert ip address to string
    if (source_addr.ss_family == PF_INET)
    {
      inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
    }
#ifdef CONFIG_EXAMPLE_IPV6
    else if (source_addr.ss_family == PF_INET6)
    {
      inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
    }
#endif
    ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

    do_retransmit(sock);

    shutdown(sock, 0);
    close(sock);
  }

CLEAN_UP:
  close(listen_sock);
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

    // xTaskCreatePinnedToCore(udp_server_task, "udp_task", 20000, nullptr, 4, &udp_task, 1);
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_task", 20000, nullptr, 4, &tcp_server_task_h, 0);

    Serial.println("UDP listening on port 1337");
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
  ETH.begin(1, 17);
}

void loop()
{
  // yield();

  static int fc = 0;
  static unsigned long last_time = millis();

  fill_rainbow(pixel_data, NUM_PIXELS, fc, 1);
  auto s = sin8(fc / 16);
  for (size_t i = 0; i < NUM_PIXELS; i++)
  {
    pixel_data[i] %= s;
  }

  // auto t = micros();
  for (size_t i = 0; i < NUM_PIXELS; i++)
  {
    hdr_pixel_data[i] = CRGBA(pixel_data[i]);
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
