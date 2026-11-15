# Co-op and proximity chat

**Gameplay → Co-op** in the Insert menu (F1) opens the page. The code is `host/coop.c` (network, lobby, voice); the page and the
name markers are in `gpu/vk/vk_menu.cpp`; the game's side is a few calls in `host/hos_main.c`, `host/oot_fov.c` and
`host/nn_audio.c`. The renderer interface is version 23 (`MkwVkHost.coop_command`, `coop_status`, `coop_marks`).

## Playing

1. **Host:** open the page, choose whether to **Use a key** (Off: anyone with the address can join; On: type the key), press
   **Host**. The page shows the address to give your friends.
2. **Join:** type the host's address (`192.168.1.5` or `192.168.1.5:7770`), the key if there is one, press **Join**.
3. When everyone is in the list, the host presses **Start**. **From then on nobody new can join**: a new join is answered
   "already started". Players who were in keep their place.
4. After Start you see each other as a marker with a name and distance over the picture (an arrow at the edge when off screen;
   green while someone talks), and proximity chat is on.

**Voice chat** row: Off, Voice activity (the default), or Push to talk (the V key; `HOS_COOP_PTT_VK=<Windows key code>` changes it).
You hear players on **the same map**, not in a cutscene: full volume within 3 m, fading to nothing at 30 m, panned by the side they are
on. The microphone is the system's default.

Typing: letters, digits, `.` `-` `_` `:` and space; Backspace deletes, Delete clears, Enter keeps, Esc cancels; Ctrl+V pastes (Windows).
Your name, the address and the voice mode are kept in `hookshot_coop.ini` beside `image.bin`. **The key is never saved.**

## Network

One UDP port on the host, 7770 (`HOS_COOP_PORT` changes it). It works on a LAN or a VPN (Tailscale, ZeroTier, Radmin) with
nothing set up; over the internet the host must forward that UDP port on the router. The page shows the host's LAN address; the
public one is for the host to look up. Up to 8 players.

## What it does not do (read this)

- **The other players are drawn as markers, not as Links.** Spawning a second Link needs the game's actor code, which this change
  does not touch. Positions and maps are shared; nothing else of the game's state (items, flags, enemies) is.
- **Nothing is encrypted.** The key is a shared password checked by a SHA-256 of the host's nonce, the joiner's nonce and the key;
  it stops people who lack it from joining, and a tester sniffing the network can see positions and hear voices.
- Not run in the game yet: `host/coop.c` is tested alone (`tests/test_coop.py`), but the hooks in the game, the camera maths for
  the markers (the camera's eye and target at the view's +96 and +120, the field of view from `oot_fov.c`), the Co-op page and
  the audio mix need a run on a real build. Link's units are taken to be metres.

## Protocol (`host/coop.c`)

UDP, little endian, `"CS"`, version 1, type. INFO_REQ → INFO (started?, key needed?, count, nonce) → JOIN (name, nonce, proof) →
JOIN_OK (id, token) or DENY (started, key, full, version). Then STATE (client → host, 10 Hz), SNAP (host → all, 10 Hz, holds the
`started` flag, so a lost Start is sent again), VOICE / VOICE_RELAY (Opus 20 ms, relayed only to players in range), LEAVE, CLOSE.
