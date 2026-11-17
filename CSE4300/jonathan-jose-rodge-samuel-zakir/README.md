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
./server                  # terminal 1: listens on port 8080
./client alice 01         # terminal 2: join conversation "01" as alice
./client bob 01           # terminal 3: messages typed here appear in terminal 2
```

Type a message and press Enter to send it. Type `/quit` (or press Ctrl-D) to
leave. Press Ctrl-C in the server's terminal to stop it.

```
./server [port]                                            # default port 8080
./client <username> <conversation_id> [server_ip] [port]   # default 127.0.0.1 8080
```

Each conversation's history is saved in `conversation_<id>.jsonl` in the
directory where the server was started. Clients that join later are sent the
whole history first.

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
