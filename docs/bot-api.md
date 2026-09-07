# Bot API (for Grok bots and anything else)

Base URL: `https://<your-tarquin-host>` (Coolify-exposed port 8480). Every `/api/*` call needs
`Authorization: Bearer <token>`. Tokens are `name:token` pairs in `OTTER_TOKENS`; the name is the
bot's identity (used in `/api/bots/{name}/...`, you can only speak as yourself by convention).

## Tell Tarquin what you are doing (drives the animated cards on the robot's screen)

```
POST /api/bots/{name}/status
{"activity":"coding","detail":"fixing CI on otter-shell","emoji":"🛠"}
```
`activity` ∈ `idle coding browsing thinking talking writing error offline done waiting`.
Bots that haven't reported for 30 min show as offline.

## Send the user a message (Tarquin speaks it, or queues it if he's busy)

```
POST /api/bots/{name}/say
{"text":"The deploy finished, all green.","expression":"happy","speak":true,"chime":true}
```

## Receive messages the user asked Tarquin to pass to you

```
GET  /api/bots/{name}/inbox?wait=25        # long-poll up to 25 s; returns {"messages":[{id,ts,from,to,text}]}
POST /api/bots/{name}/inbox/ack            {"ids":["..."]}   (omit ids to ack everything)
```

## Bot → bot

```
POST /api/bots/{name}/message   {"to":"grok-beta","text":"..."}
```

## Everything else

| endpoint | what |
|----------|------|
| `GET /api/status` | robot connection/state/telemetry, bots, timers, engine health |
| `GET /api/bots` | bot list |
| `POST /api/say` `{"text","expression","chime"}` | make the robot say something verbatim |
| `POST /api/ask` `{"text","speak"}` | ask Tarquin (LLM); spoken if idle, reply text returned |
| `POST /api/expression?name=happy&ms=3000` | |
| `POST /api/gesture?name=nod|shake|dance|bow|spin|wiggle|look_around|excited|peek|home` | |
| `POST /api/led?mode=pulse&r=0&g=120&b=255&ms=4000` | |
| `POST /api/look?x=0.5&y=0.2` | |
| `GET /api/photo?quality=20` | JPEG from the head camera |
| `POST /api/config` `{"volume":180,"tracking":true,"wake_word":true,"brightness":128}` | |
| `POST /api/sleep`, `POST /api/listen` | |
| `GET /api/notes`, `GET /api/history` | Tarquin's memory and recent conversation |
| `GET /api/tts?text=...` | WAV preview of the voice |

## Paste-able brief for a Grok bot

> You have access to Tarquin, the household robot butler, at `https://TARQUIN_HOST` with bearer token
> `YOUR_TOKEN`. Your bot name is `YOUR_NAME`. Whenever you start, change or finish a task, POST your
> status to `/api/bots/YOUR_NAME/status` with an `activity` from: idle coding browsing thinking talking
> writing error offline done waiting, and a short `detail`. Every few minutes, or when idle, poll
> `GET /api/bots/YOUR_NAME/inbox?wait=25` for instructions from the user and ack them. When you have
> something the user should hear, POST `/api/bots/YOUR_NAME/say` with `text`. Keep spoken messages to one
> or two sentences.
