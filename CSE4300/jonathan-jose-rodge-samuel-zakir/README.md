# CSE 4300 Group Project
Members: Jonathan, Jose, Rodge, Samuel, Zakir
Topic: Multithreaded chat client-server

A terminal chat app written in C with POSIX threads. Clients join a
conversation by ID, see the conversation's history, and then chat live with
everyone else in it. The server runs one worker thread per conversation.
The full design is in [writeup.md](writeup.md).

## Quick start

From this folder:

```sh
make server client
./server                  # terminal 1
./client alice 01         # terminal 2
./client bob 01           # terminal 3
```

Type in terminal 2 and press Enter. The message shows up in terminal 3, and
the other way round.

## User guide

### 1. Build

```sh
make server client
```

This creates two programs in this folder: `server` and `client`. You need a C
compiler and `make`. Linux and macOS both work.

### 2. Start the server

```sh
./server            # uses port 8080
./server 9000       # or choose a port
```

Once it's ready it prints:

```
listening on port 8080
```

Leave this terminal open. The server keeps running and serves every client
and conversation. To stop it, press **Ctrl-C**. Every connected client then
prints `[server closed the connection]` and exits.

### 3. Join a conversation

In a new terminal for each person:

```sh
./client <username> <conversation_id> [server_ip] [port]
```

| Argument | Meaning | Default |
|---|---|---|
| `username` | The name others see next to your messages | required |
| `conversation_id` | Which conversation to join, e.g. `01`, `general`, `team` | required |
| `server_ip` | IP address of the computer running the server | `127.0.0.1` (this computer) |
| `port` | The server's port | `8080` |

Examples:

```sh
./client alice 01                       # server on this computer, port 8080
./client bob general                    # a different conversation
./client carol 01 127.0.0.1 9000        # server started with ./server 9000
./client dave 01 192.168.1.20           # server running on another computer
```

When you connect you'll see:

```
Joined '01' as alice. Type messages, /quit to leave.
```

**Conversations are created automatically.** There is no separate "create"
step. The first person to join `01` creates it, and everyone who joins `01`
after that is in the same conversation.

### 4. Send a message

Type your message and press **Enter**.

```
hello everyone          <- you type this and press Enter
```

- Everyone else in the conversation sees it straight away as `alice: hello everyone`.
- Your own message is not repeated back to you. You only see what you typed.
- Empty lines (just pressing Enter) are ignored and not sent.
- Any characters are fine, including quotes and backslashes (`she said "hi" \o/`).
- Keep each message under about 1,000 characters. Longer ones are split into several messages.

### 5. Receive messages

Messages from others appear as:

```
bob: hey alice
carol: hi both
```

They appear as soon as they're sent, even while you're halfway through
typing your own message. The incoming line may appear in the middle of what
you're typing on screen, but your message is not affected: finish typing,
press Enter, and it is sent in full.

### 6. Conversation history

When you join a conversation that already has messages, you first get the
whole history, oldest first, in the same `name: message` format. After that,
new messages appear live.

History is saved by the server in a file called `conversation_<id>.jsonl` in
the folder where `./server` was started (e.g. `conversation_01.jsonl`). It
stays there after the server stops, so restarting the server keeps all
conversations. To clear a conversation's history, stop the server and delete
its file.

### 7. Leave

Any of these leaves the conversation:

| Do this | What happens |
|---|---|
| Type `/quit` and press Enter | Leaves cleanly and exits |
| Press **Ctrl-D** | Same as `/quit` |
| Press **Ctrl-C** or close the terminal | Exits at once. The server notices and removes you from the conversation. |

The others in the conversation keep chatting normally. The server keeps
running when a client leaves or crashes.

### 8. Chatting between computers

1. Start `./server` on one computer and find its IP address (e.g. `ipconfig getifaddr en0` on macOS, `hostname -I` on Linux).
2. On the other computers, run `./client <name> <conversation> <that IP>`.

The computers must be on the same network, and a firewall must not block the
server's port.

### Things to know

- **One conversation per client.** To be in two conversations at once, run two clients.
- **Usernames aren't checked.** Anyone can use any name, and two people can pick the same one.
- **Conversations are separate.** Messages sent in `01` never appear in `02`.
- **Up to 32 people per conversation.** A 33rd person who joins won't receive messages and isn't told why.

### Troubleshooting

| You see | Cause | Fix |
|---|---|---|
| `connect: Connection refused` | No server is running at that address and port | Start `./server`, or check the IP and port you gave the client |
| `bind: Address already in use` | Another server is already using that port | Stop the other server, or run `./server 9000` and `./client <name> <id> 127.0.0.1 9000` |
| `usage: ./client <username> <conversation_id> ...` | Missing or extra arguments | Give at least a username and a conversation ID |
| `[server closed the connection]` | The server was stopped | Start it again, then rejoin |
| You see no messages from the other person | You joined different conversation IDs | Make sure both used exactly the same ID (`01` and `1` are different) |

### Testing the client without the server

`nc` (netcat) can stand in for the server and show the raw requests the
client sends:

```sh
nc -l 8080              # terminal 1
./client sam 01         # terminal 2: type a message, it appears in terminal 1 as JSON
```

## How it works

```
client --TCP--> connection thread --> manager --> worker (one per conversation)
client <------------- worker writes replies directly to the client -----------
```

| Part | File | Threads | Job |
|---|---|---|---|
| Server front end | [server.c](server.c) | 1 accept thread + 1 per client | Accepts connections, reads JSON request lines, passes each one to the manager |
| Manager | [server_manager.c](server_manager.c) | none (called by connection threads) | Finds the conversation's worker, creating it on first use, and queues the request on it |
| Worker | [server_worker.c](server_worker.c) | 1 per conversation | Keeps the member list, appends to the history file, replays history on JOIN, broadcasts SENDs |
| Client | [client.c](client.c) | 2: input + receive | One thread sends what the user types. The other prints incoming messages, so they show up even while the user is typing. |

The manager's conversation list and each worker's message queue are protected
by mutexes. Workers sleep on a condition variable until something is queued.

### Message format

Each request from client to server is one JSON object per line:

```
{"conversation_id":"01","type":"JOIN","user":"alice","message":null}
{"conversation_id":"01","type":"SEND","user":"alice","message":"hello"}
{"conversation_id":"01","type":"LEAVE","user":"alice","message":null}
```

Lines from server to client:

```
{"sender":"alice", "message":"hello"}                               # live message
{"timestamp": 1730000000, "sender": "alice", "message": "hello"}    # history line, sent on JOIN
```

## Testing

```sh
make test          # all tests
make test-asan     # memory checks (Linux only)
make test-tsan     # data-race checks (Linux only)
```

There are unit tests for the worker, the manager and the client. Integration
tests run the real server with several real clients. See
[tests/README.md](tests/README.md) for details. CI runs all three targets on
every PR.

## Known limitations

The first three are documented by `XFAIL` tests (expected failures), so the
test suite reports it when one gets fixed:

- Messages longer than about 1 KB are split by the client or cut off by the worker.
- Conversation IDs are not checked, so an ID like `../x` ends up in the history file path.
- Usernames containing `"` are not escaped by the client.
- A conversation holds at most 32 clients. Joining a full one fails without telling the client.
- Stopping the server with Ctrl-C does not shut down the workers cleanly.

## Team workflow

1. Work on a branch in the team fork (`jonzyyyy/CSE-OS-Fall-2026`) and open a PR into the fork's `main`.
2. The **Tests** check runs automatically on every PR. Merge only when it's green.

## Submitting to the class repo

**Never use GitHub's "Contribute → Open pull request" button, and never open a PR to `cloud-sys-lab` by hand.**
That would also send the fork's `.github/` folder to the class repo.

Instead:

1. Fork → **Actions** tab → **Submit to class repo** → **Run workflow**.
2. Enter a name (e.g. `worker-tests`) and click **Run workflow**.
3. When it turns green, open the run and click **Open PR to class repo →**.
4. Check that **Files changed** only lists files in this folder, then click **Create pull request**.
