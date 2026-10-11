# SL Departures

Live Stockholm metro departures and train positions on a
[Waveshare ESP32-C6-LCD-1.47](https://www.waveshare.com/esp32-c6-lcd-1.47.htm).

![Brommaplan on the green line](screenshots/brommaplan.png)

**Install from your browser:** https://rdahlborg.github.io/sl-departures
(Chrome or Edge on a computer, with the board on a USB-C data cable). After
flashing, the installer asks for your Wi-Fi details.

## Features

- Every station on the green, red and blue lines (10, 11, 13, 14, 17, 18, 19).
- Line map with four stations each side, with trains placed from live data.
- The next two departures in each direction, with countdowns and cancellations.
- Choose line and station with the BOOT button. Your choice is remembered.

| Where | Press | Double press | Hold |
|---|---|---|---|
| Departure board | next station | previous station | choose line |
| Line list | next line | previous line | select |
| Station list | next station | previous station | select |

## How trains are placed

SL has no public vehicle positions for the metro. Every departure from the
[SL Transport API](https://www.trafiklab.se/api/other-apis/sl/transport/) does
carry a journey ID shared across stations, though. So the board also fetches the
five stations on each side of yours. The furthest one that still lists a train
is where that train stops next. Its expected arrival there, and the time between
stations, place it along the track. The API needs no key, and a poll every 20
seconds takes 12 requests.

## Building

Needs [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/get-started/).

```sh
idf.py set-target esp32c6
idf.py -p PORT flash monitor
```

Then set Wi-Fi from the web installer (*Change Wi-Fi*), or, for development
builds, create `secrets.defaults`. It is git-ignored and only used when no
network has been saved:

```
CONFIG_WIFI_SSID="my-network"
CONFIG_WIFI_PASSWORD="secret"
```

Other options (timezone, poll interval, screen rotation) are under
*SL Departures Configuration* in `idf.py menuconfig`.

Pushing to `main` makes GitHub Actions build the firmware and publish the
installer page. Pushing a `v*` tag also attaches `firmware.bin` to the release.

### Layout

| Path | |
|---|---|
| `main/sl_api.c` | Polls the API, works out train positions |
| `main/ui.c` | LVGL screen: header, line map, departures, picker |
| `main/controls.c` | BOOT button gestures, saved station |
| `main/board.c` | Display, Wi-Fi, time sync |
| `main/improv.c`, `main/serial.c` | Improv Wi-Fi setup for the web installer, debug keys |
| `main/metro_data.c` | Station order per line (generated) |
| `tools/` | Generators for the station table and fonts, screenshot grabber |
| `site/` | Web installer page |

### Debugging over USB serial

`python tools/screenshot.py PORT out.png` saves the screen as a PNG. Sending
`n`, `p` or `h` simulates a press, double press or hold of the button.

### Regenerating station data

```sh
python tools/routes.py DIR      # needs sites_x.json and sp.json from the API in DIR
python tools/order.py DIR
python tools/gen_metro_data.py DIR/order.json main/metro_data.c
```

## Credits

- Data from [Trafiklab.se](https://www.trafiklab.se/) (SL Transport API).
  Not affiliated with SL or Trafiklab.
- [Montserrat](https://github.com/JulietaUla/Montserrat) font, SIL Open Font
  License 1.1 (`main/fonts/OFL.txt`).
- [LVGL](https://lvgl.io/), [ESP Web Tools](https://esphome.github.io/esp-web-tools/),
  [Improv Wi-Fi](https://www.improv-wifi.com/).

## License

MIT, see [LICENSE](LICENSE).
