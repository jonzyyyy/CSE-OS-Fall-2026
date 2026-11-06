# Architecture Formal Writeup

## Rough Draft / Abstract
```
Summary

Creating a multi-threaded client-server chat room implementation concepts include: threads, multi-threading, and maybe synchronization

The rough architecture would be:

1. Manager server thread
2. Worker server thread
3. Client main thread

Each worker thread is mapped to a unique conversation.

Manager server thread is responsible for:

1. Creating new worker threads for newly created conversations
2. Routing incoming messages to already existing conversations

Worker server threads are responsible for:

1. Sending new messages out to listening clients
2. Sending entire conversation for newly joined clients

Finally, there will be the clients themselves which allows user to:

- Choose a conversation to listen to
- Send messages to conversation

```

## Specification

Use as an example: [TCP Server-Client implementation in C](https://www.geeksforgeeks.org/c/tcp-server-client-implementation-in-c/)

### Message Payload Format
Format should also follow JSONL format as mentioned later in **Worker Server Thread**
```
type: "SEND" | "JOIN" | "LEAVE"
user: string
message: string | null
sockfd: string
conversation_id: string

ex. {"conversation_id":"01","type":"SEND","user":"mike","message":"hello world"}\n
```

### Manager Server Thread
Responsible for:
-  Creating worker threads for newly created conversations
-  Routing message traffic to existing conversation worker threads

A manager should receive a conversation ID + message itself 

Common flows:
- If the conversation ID exists --> place the message onto the respective worker thread's message queue
- Else conversation does NOT exist --> create a worker thread for conversation, and place onto worker's message queue

![Manager server thread diagram](https://github.com/jonzyyyy/CSE-OS-Fall-2026/blob/main/CSE4300/jonathan-jose-rodge-samuel-zakir/images/manager-server-thread-diagram.png?raw=true)

### Worker Server Thread
Responsible for:
- Sending entire conversation for newly joined clients to conversation
- Broadcasting new messages for already listening clients 

A worker should track:
1. Listening clients 
2. Message queue
3. Conversation history JSONL file

More on JSONL format: (https://jsonlines.org/)

Example of conversation JSONL file:
```
{"timestamp": 1730000000, "sender": "Alice", "message": "Hello"}
{"timestamp": 1730000001, "sender": "Bob", "message": "Hey Alice"}
{"timestamp": 1730000002, "sender": "Alice", "message": "How are you?"}
```

![Worker server thread diagram](https://github.com/jonzyyyy/CSE-OS-Fall-2026/blob/main/CSE4300/jonathan-jose-rodge-samuel-zakir/images/worker-server-thread-diagram.png?raw=true)

### Client
Responsible for:
- Listening for new messages from worker thread to display onto user screen
- Taking user input to send to manager server thread

In order for incoming messages to not be blocked by user input instruction (ex. a new message arrives while user typing) 
Client will need two threads:
- Network listening thread 
- User input listening thread 

Network listening thread:
- Worker threads will broadcast any new messages to the conversation to already listening client
- This thread should listen for those broadcast and print to user screen the new message

User Input Listening thread:
- User can type out their message
- Once user is done and presses "Enter" -> this thread should send that traffic out to manager server thread
